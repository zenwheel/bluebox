/*
 * Mac OS 9's File Manager calls for HFS Plus: objects named by FSRefs,
 * forks opened by name with 64-bit positions, iterators over folders,
 * catalog information and Unicode names; the Alias, Folder and Resource
 * Manager calls on FSRefs; and the File System Manager's.
 *
 * An FSRef is 'BBrf', the volume's refnum and the object's node number in
 * the File Manager's directory table (filemgr_internal.h), which follows
 * the object through renames and moves for the run.  A Unicode name is
 * the host's (UTF-8) as UTF-16, with '/' in a Mac name ':' on the host as
 * everywhere else; names may be as long as the host's.
 */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>

#include "filemgr_internal.h"
#include "forks.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

enum {
    nsvErr = -35, bdNamErr = -37, eofErr = -39, posErr = -40, wPrErr = -44, fBsyErr = -47, dupFNErr = -48,
    opWrErr = -49, rfNumErr = -51, wrPermErr = -61, badMovErr = -122, notAFileErr = -1302, diffVolErr = -1303,
    kMPTimeoutErr = -29296,
};
enum {
    errFSBadFSRef = -1401, errFSBadInfoBitmap = -1405, errFSMissingCatInfo = -1406, errFSNotAFolder = -1407,
    errFSForkNotFound = -1409, errFSNameTooLong = -1410, errFSMissingName = -1411, errFSBadPosMode = -1412,
    errFSBadAllocFlags = -1413, errFSNoMoreItems = -1417, errFSBadItemCount = -1418, errFSBadSearchParams = -1419,
    errFSRefsDifferent = -1420, errFSForkExists = -1421, errFSBadIteratorFlags = -1422,
    errFSIteratorNotFound = -1423, errFSIteratorNotSupported = -1424, errFSBadForkName = -1402,
};
enum {
    kFSCatInfoTextEncoding = 0x1, kFSCatInfoNodeFlags = 0x2, kFSCatInfoVolume = 0x4, kFSCatInfoParentDirID = 0x8,
    kFSCatInfoNodeID = 0x10, kFSCatInfoCreateDate = 0x20, kFSCatInfoContentMod = 0x40, kFSCatInfoAttrMod = 0x80,
    kFSCatInfoAccessDate = 0x100, kFSCatInfoBackupDate = 0x200, kFSCatInfoPermissions = 0x400,
    kFSCatInfoFinderInfo = 0x800, kFSCatInfoFinderXInfo = 0x1000, kFSCatInfoValence = 0x2000,
    kFSCatInfoDataSizes = 0x4000, kFSCatInfoRsrcSizes = 0x8000, kFSCatInfoSharingFlags = 0x10000,
    kFSCatInfoUserPrivs = 0x20000, kFSCatInfoGettableInfo = 0x3FFFF, kFSCatInfoSettableInfo = 0x1FE3,
};
enum { kFSVolInfoGettableInfo = 0xFFFF, kFSVolInfoSettableInfo = 0x3004 };

#define REF_SIG 0x42427266u /* 'BBrf' */
#define FSREF_SIZE 80u
#define UNISTR_SIZE 512u

/* ---------------------------------------------------------------------- */
/* FSRefs, names and dates */

static void make_ref(gaddr ref, const char *path)
{
    if (!ref)
        return;
    gmemset(ref, 0, FSREF_SIZE);
    wr32(ref, REF_SIG);
    wr16(ref + 4, (uint16_t)fm_vref_of_path(path));
    wr32(ref + 8, (uint32_t)fm_node_of_path(path));
}

/* The host path of an FSRef: errFSBadFSRef for one that is none, fnfErr
 * for one whose object is gone. */
static int16_t ref_path(gaddr ref, char *out, size_t outsize)
{
    if (!ref || rd32(ref) != REF_SIG)
        return errFSBadFSRef;
    if (!fm_path_of_node((int32_t)rd32(ref + 8), out, outsize))
        return fnfErr;
    return fm_host_exists(out) && fm_visible(out) ? noErr : fnfErr;
}

/* For Core Foundation's URLs (carbon_cfurl.c). */
void hfs_make_fsref(gaddr ref, const char *path) { make_ref(ref, path); }
int16_t hfs_fsref_path(gaddr ref, char *out, size_t outsize) { return ref_path(ref, out, outsize); }

/* A directory's host path from its FSRef. */
static int16_t ref_dir(gaddr ref, char *out, size_t outsize)
{
    int16_t err = ref_path(ref, out, outsize);
    return err ? err : fm_is_dir(out) ? noErr : errFSNotAFolder;
}

/* A Unicode (UTF-16) name as a host name: UTF-8, '/' as ':'. */
static int16_t uni_to_host(gaddr name, uint32_t len, char *out, size_t outsize)
{
    if (!name || !len)
        return errFSMissingName;
    if (len > 255)
        return errFSNameTooLong;
    size_t k = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t c = rd16(name + 2 * i);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < len) {
            uint32_t d = rd16(name + 2 * (i + 1));
            if (d >= 0xDC00 && d < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
                i++;
            }
        }
        if (c == 0 || c == ':')
            return bdNamErr; /* ':' separates a pathname's parts */
        if (c == '/')
            c = ':';
        uint8_t u[4];
        size_t n;
        if (c < 0x80) {
            u[0] = (uint8_t)c;
            n = 1;
        } else if (c < 0x800) {
            u[0] = (uint8_t)(0xC0 | c >> 6);
            u[1] = (uint8_t)(0x80 | (c & 0x3F));
            n = 2;
        } else if (c < 0x10000) {
            u[0] = (uint8_t)(0xE0 | c >> 12);
            u[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
            u[2] = (uint8_t)(0x80 | (c & 0x3F));
            n = 3;
        } else {
            u[0] = (uint8_t)(0xF0 | c >> 18);
            u[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3F));
            u[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
            u[3] = (uint8_t)(0x80 | (c & 0x3F));
            n = 4;
        }
        if (k + n >= outsize || k + n > NAME_MAX)
            return errFSNameTooLong;
        memcpy(out + k, u, n);
        k += n;
    }
    out[k] = 0;
    if (!strcmp(out, ".") || !strcmp(out, ".."))
        return bdNamErr;
    return noErr;
}

/* A host name (UTF-8) as an HFSUniStr255, ':' as '/'. */
static void host_to_uni(const char *name, gaddr out)
{
    if (!out)
        return;
    const uint8_t *p = (const uint8_t *)name;
    uint16_t n = 0;
    while (*p && n < 255) {
        uint32_t c = *p++;
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (more)
            c &= 0x3F >> more;
        for (; more && (*p & 0xC0) == 0x80; more--)
            c = c << 6 | (*p++ & 0x3F);
        if (c == ':')
            c = '/';
        if (c >= 0x10000) {
            if (n + 2 > 255)
                break;
            c -= 0x10000;
            wr16(out + 2 + 2u * n++, (uint16_t)(0xD800 + (c >> 10)));
            wr16(out + 2 + 2u * n++, (uint16_t)(0xDC00 + (c & 0x3FF)));
        } else {
            wr16(out + 2 + 2u * n++, (uint16_t)c);
        }
    }
    wr16(out, n);
}

static void put_name_of(const char *path, gaddr out)
{
    char name[NAME_MAX * 2 + 1];
    fm_display_name(path, name, sizeof name);
    host_to_uni(name, out);
}

/* The host's offset from UTC at a moment (seconds since 1970). */
static long gmtoff_at(int64_t t)
{
    struct tm lt;
    time_t tt = (time_t)t;
    localtime_r(&tt, &lt);
    return lt.tm_gmtoff;
}

#define MAC_EPOCH 2082844800LL

/* A Mac (local) time as a UTCDateTime: 48 bits of seconds since 1904, UTC. */
static void put_utc(gaddr p, uint32_t local)
{
    gmemset(p, 0, 8);
    if (!local)
        return;
    int64_t u = (int64_t)local - MAC_EPOCH;
    u -= gmtoff_at(u);
    uint64_t s = (uint64_t)(u + MAC_EPOCH);
    wr16(p, (uint16_t)(s >> 32));
    wr32(p + 2, (uint32_t)s);
}

static uint32_t get_utc(gaddr p)
{
    uint64_t s = (uint64_t)rd16(p) << 32 | rd32(p + 2);
    if (!s)
        return 0;
    int64_t u = (int64_t)s - MAC_EPOCH;
    return (uint32_t)(u + gmtoff_at(u) + MAC_EPOCH);
}

static void put64(gaddr p, uint64_t v)
{
    wr32(p, (uint32_t)(v >> 32));
    wr32(p + 4, (uint32_t)v);
}

static uint64_t get64(gaddr p) { return (uint64_t)rd32(p) << 32 | rd32(p + 4); }

/* ---------------------------------------------------------------------- */
/* Catalog information */

static void put_cat_info(gaddr ci, const char *path, uint32_t which)
{
    if (!ci || !which)
        return;
    fm_info in;
    fm_info_of(path, &in);
    if (which & kFSCatInfoTextEncoding)
        wr32(ci + FSCatalogInfo_textEncodingHint_off, 0); /* kTextEncodingMacRoman */
    if (which & kFSCatInfoNodeFlags) {
        uint16_t f = (uint16_t)((in.dir ? 0x10 : 0) | (in.data_open ? 0x08 : 0) | (in.rsrc_open ? 0x04 : 0) |
                                (in.data_open || in.rsrc_open ? 0x80 : 0));
        wr16(ci + FSCatalogInfo_nodeFlags_off, f);
    }
    if (which & kFSCatInfoVolume)
        wr16(ci + FSCatalogInfo_volume_off, (uint16_t)fm_vref_of_path(path));
    if (which & kFSCatInfoParentDirID)
        wr32(ci + FSCatalogInfo_parentDirID_off, fm_parent_cat_id(path));
    if (which & kFSCatInfoNodeID)
        wr32(ci + FSCatalogInfo_nodeID_off, fm_cat_id(path));
    if (which & kFSCatInfoSharingFlags)
        wr8(ci + FSCatalogInfo_sharingFlags_off, 0);
    if (which & kFSCatInfoUserPrivs)
        wr8(ci + FSCatalogInfo_userPrivileges_off, 0);
    if (which & kFSCatInfoCreateDate)
        put_utc(ci + FSCatalogInfo_createDate_off, in.created);
    if (which & kFSCatInfoContentMod)
        put_utc(ci + FSCatalogInfo_contentModDate_off, in.modified);
    if (which & kFSCatInfoAttrMod)
        put_utc(ci + FSCatalogInfo_attributeModDate_off, in.modified);
    if (which & kFSCatInfoAccessDate)
        put_utc(ci + FSCatalogInfo_accessDate_off, in.modified);
    if (which & kFSCatInfoBackupDate)
        gmemset(ci + FSCatalogInfo_backupDate_off, 0, 8);
    if (which & kFSCatInfoPermissions)
        gmemset(ci + FSCatalogInfo_permissions_off, 0, 16);
    if (which & kFSCatInfoFinderInfo)
        gput(ci + FSCatalogInfo_finderInfo_off, in.finder_info, 16);
    if (which & kFSCatInfoFinderXInfo)
        gput(ci + FSCatalogInfo_extFinderInfo_off, in.finder_info + 16, 16);
    if (which & kFSCatInfoValence)
        wr32(ci + FSCatalogInfo_valence_off, in.dir ? in.valence : 0);
    if (which & kFSCatInfoDataSizes) {
        put64(ci + FSCatalogInfo_dataLogicalSize_off, in.data_len);
        put64(ci + FSCatalogInfo_dataPhysicalSize_off, (in.data_len + 511u) & ~511u);
    }
    if (which & kFSCatInfoRsrcSizes) {
        put64(ci + FSCatalogInfo_rsrcLogicalSize_off, in.rsrc_len);
        put64(ci + FSCatalogInfo_rsrcPhysicalSize_off, (in.rsrc_len + 511u) & ~511u);
    }
}

/* What a file keeps of its catalog information: Finder information and
 * the creation and modification dates.  Locks, permissions and the other
 * dates are not kept, as on the File Manager's other calls. */
static int16_t set_cat_info(const char *path, uint32_t which, gaddr ci)
{
    if (which & ~(uint32_t)kFSCatInfoSettableInfo)
        return errFSBadInfoBitmap;
    if (!which)
        return noErr;
    if (!ci)
        return errFSMissingCatInfo;
    uint8_t fi[16], fx[16];
    gget(fi, ci + FSCatalogInfo_finderInfo_off, 16);
    gget(fx, ci + FSCatalogInfo_extFinderInfo_off, 16);
    uint32_t cr = which & kFSCatInfoCreateDate ? get_utc(ci + FSCatalogInfo_createDate_off) : 0;
    uint32_t md = which & kFSCatInfoContentMod ? get_utc(ci + FSCatalogInfo_contentModDate_off) : 0;
    if (!(which & (kFSCatInfoFinderInfo | kFSCatInfoFinderXInfo)) && !cr && !md)
        return noErr;
    return fm_set_info(path, which & kFSCatInfoFinderInfo ? fi : NULL, which & kFSCatInfoFinderXInfo ? fx : NULL, cr,
                       md);
}

/* ---------------------------------------------------------------------- */
/* Making FSRefs, comparing them */

static int16_t make_fsref(int16_t vref, int32_t dirid, gaddr name, gaddr new_ref)
{
    char path[PATH_MAX];
    int16_t err = fm_resolve(vref, dirid, name, path, sizeof path);
    if (err)
        return err;
    if (!fm_host_exists(path))
        return fnfErr;
    make_ref(new_ref, path);
    return noErr;
}

int16_t tb_FSpMakeFSRef(gaddr source, gaddr newRef)
{
    if (!source)
        return paramErr;
    return make_fsref((int16_t)rd16(source + FSSpec_vRefNum_off), (int32_t)rd32(source + FSSpec_parID_off),
                      source + FSSpec_name_off, newRef);
}

int16_t tb_PBMakeFSRef(gaddr pb, bool async)
{
    return make_fsref((int16_t)rd16(pb + FSRefParam_ioVRefNum_off), (int32_t)rd32(pb + FSRefParam_ioDirID_off),
                      rd32(pb + FSRefParam_ioNamePtr_off), rd32(pb + FSRefParam_newRef_off));
}

/* An object in a directory by its Unicode name. */
static int16_t child_of(gaddr parent, uint32_t len, gaddr name, char *out, size_t outsize)
{
    char dir[PATH_MAX], host[NAME_MAX * 4 + 1];
    int16_t err = ref_dir(parent, dir, sizeof dir);
    if (!err)
        err = uni_to_host(name, len, host, sizeof host);
    if (!err)
        fm_child_path(dir, host, out, outsize);
    return err;
}

int16_t tb_FSMakeFSRefUnicode(gaddr parentRef, uint32_t nameLength, gaddr name, uint32_t textEncodingHint,
                              gaddr newRef)
{
    char path[PATH_MAX];
    int16_t err = child_of(parentRef, nameLength, name, path, sizeof path);
    if (err)
        return err;
    if (!fm_host_exists(path) || !fm_visible(path))
        return fnfErr;
    make_ref(newRef, path);
    return noErr;
}

int16_t tb_PBMakeFSRefUnicode(gaddr pb, bool async)
{
    return tb_FSMakeFSRefUnicode(rd32(pb + FSRefParam_ref_off), rd32(pb + FSRefParam_nameLength_off),
                                 rd32(pb + FSRefParam_name_off), rd32(pb + FSRefParam_textEncodingHint_off),
                                 rd32(pb + FSRefParam_newRef_off));
}

int16_t tb_FSCompareFSRefs(gaddr ref1, gaddr ref2)
{
    char a[PATH_MAX], b[PATH_MAX];
    int16_t err = ref_path(ref1, a, sizeof a);
    if (!err)
        err = ref_path(ref2, b, sizeof b);
    if (err)
        return err;
    return rd32(ref1 + 8) == rd32(ref2 + 8) ? noErr : errFSRefsDifferent;
}

int16_t tb_PBCompareFSRefs(gaddr pb, bool async)
{
    return tb_FSCompareFSRefs(rd32(pb + FSRefParam_ref_off), rd32(pb + FSRefParam_parentRef_off));
}

/* ---------------------------------------------------------------------- */
/* Creating, deleting, moving, renaming, exchanging */

static int16_t create_object(gaddr parent, uint32_t len, gaddr name, uint32_t which, gaddr ci, bool dir,
                             gaddr new_ref, gaddr new_spec, gaddr new_dir_id)
{
    if (which & ~(uint32_t)kFSCatInfoSettableInfo)
        return errFSBadInfoBitmap;
    char path[PATH_MAX];
    int16_t err = child_of(parent, len, name, path, sizeof path);
    if (err)
        return err;
    if ((err = dir ? fm_create_dir(path) : fm_create_file(path)))
        return err;
    if (which && ci)
        set_cat_info(path, which, ci);
    make_ref(new_ref, path);
    if (new_spec)
        fm_spec_of_path(path, new_spec);
    if (new_dir_id)
        wr32(new_dir_id, fm_cat_id(path));
    return noErr;
}

int16_t tb_FSCreateFileUnicode(gaddr parentRef, uint32_t nameLength, gaddr name, uint32_t whichInfo, gaddr catalogInfo,
                               gaddr newRef, gaddr newSpec)
{
    return create_object(parentRef, nameLength, name, whichInfo, catalogInfo, false, newRef, newSpec, 0);
}

int16_t tb_FSCreateDirectoryUnicode(gaddr parentRef, uint32_t nameLength, gaddr name, uint32_t whichInfo,
                                    gaddr catalogInfo, gaddr newRef, gaddr newSpec, gaddr newDirID)
{
    return create_object(parentRef, nameLength, name, whichInfo, catalogInfo, true, newRef, newSpec, newDirID);
}

int16_t tb_PBCreateFileUnicode(gaddr pb, bool async)
{
    return create_object(rd32(pb + FSRefParam_ref_off), rd32(pb + FSRefParam_nameLength_off),
                         rd32(pb + FSRefParam_name_off), rd32(pb + FSRefParam_whichInfo_off),
                         rd32(pb + FSRefParam_catInfo_off), false, rd32(pb + FSRefParam_newRef_off),
                         rd32(pb + FSRefParam_spec_off), 0);
}

int16_t tb_PBCreateDirectoryUnicode(gaddr pb, bool async)
{
    return create_object(rd32(pb + FSRefParam_ref_off), rd32(pb + FSRefParam_nameLength_off),
                         rd32(pb + FSRefParam_name_off), rd32(pb + FSRefParam_whichInfo_off),
                         rd32(pb + FSRefParam_catInfo_off), true, rd32(pb + FSRefParam_newRef_off),
                         rd32(pb + FSRefParam_spec_off), pb + FSRefParam_ioDirID_off);
}

int16_t tb_FSDeleteObject(gaddr ref)
{
    char path[PATH_MAX];
    int16_t err = ref_path(ref, path, sizeof path);
    if (err)
        return err;
    if (fm_is_volume_root(path))
        return fBsyErr;
    return fm_delete(path);
}

int16_t tb_PBDeleteObject(gaddr pb, bool async) { return tb_FSDeleteObject(rd32(pb + FSRefParam_ref_off)); }

int16_t tb_FSMoveObject(gaddr ref, gaddr destDirectory, gaddr newRef)
{
    char from[PATH_MAX], dir[PATH_MAX], dest[PATH_MAX];
    int16_t err = ref_path(ref, from, sizeof from);
    if (!err)
        err = ref_dir(destDirectory, dir, sizeof dir);
    if (err)
        return err;
    if (fm_vref_of_path(from) != fm_vref_of_path(dir))
        return diffVolErr;
    if (fm_is_volume_root(from))
        return badMovErr;
    if ((err = fm_move(from, dir, dest, sizeof dest)))
        return err;
    make_ref(newRef, dest);
    return noErr;
}

int16_t tb_PBMoveObject(gaddr pb, bool async)
{
    return tb_FSMoveObject(rd32(pb + FSRefParam_ref_off), rd32(pb + FSRefParam_parentRef_off),
                           rd32(pb + FSRefParam_newRef_off));
}

int16_t tb_FSRenameUnicode(gaddr ref, uint32_t nameLength, gaddr name, uint32_t textEncodingHint, gaddr newRef)
{
    char from[PATH_MAX], host[NAME_MAX * 4 + 1];
    int16_t err = ref_path(ref, from, sizeof from);
    if (!err)
        err = uni_to_host(name, nameLength, host, sizeof host);
    if (err)
        return err;
    if (fm_is_volume_root(from) || !strcmp(from, "/"))
        return wPrErr; /* the volume's name and the Host folder's are fixed */
    char to[PATH_MAX];
    const char *slash = strrchr(from, '/');
    snprintf(to, sizeof to, "%.*s/%s", (int)(slash - from), from, host);
    if ((err = fm_rename(from, to)))
        return err;
    make_ref(newRef, to);
    return noErr;
}

int16_t tb_PBRenameUnicode(gaddr pb, bool async)
{
    return tb_FSRenameUnicode(rd32(pb + FSRefParam_ref_off), rd32(pb + FSRefParam_nameLength_off),
                              rd32(pb + FSRefParam_name_off), rd32(pb + FSRefParam_textEncodingHint_off),
                              rd32(pb + FSRefParam_newRef_off));
}

int16_t tb_FSExchangeObjects(gaddr ref, gaddr destRef)
{
    char a[PATH_MAX], b[PATH_MAX];
    int16_t err = ref_path(ref, a, sizeof a);
    if (!err)
        err = ref_path(destRef, b, sizeof b);
    if (err)
        return err;
    if (fm_vref_of_path(a) != fm_vref_of_path(b))
        return diffVolErr;
    return fm_exchange(a, b);
}

int16_t tb_PBExchangeObjects(gaddr pb, bool async)
{
    return tb_FSExchangeObjects(rd32(pb + FSRefParam_ref_off), rd32(pb + FSRefParam_parentRef_off));
}

/* ---------------------------------------------------------------------- */
/* Catalog information */

/* The folder an object is in: the Host folder ("/") is in the main
 * volume's root. */
static void parent_path(const char *path, char *out, size_t outsize)
{
    if (!strcmp(path, "/")) {
        int16_t vref, drive, driver;
        uint8_t name[32];
        fm_volume(1, 0, &vref, out, outsize, name, &drive, &driver);
        return;
    }
    snprintf(out, outsize, "%s", path);
    char *slash = strrchr(out, '/');
    if (slash == out)
        slash[1] = 0;
    else if (slash)
        *slash = 0;
}

int16_t tb_FSGetCatalogInfo(gaddr ref, uint32_t whichInfo, gaddr catalogInfo, gaddr outName, gaddr fsSpec,
                            gaddr parentRef)
{
    if (whichInfo & ~(uint32_t)kFSCatInfoGettableInfo)
        return errFSBadInfoBitmap;
    char path[PATH_MAX];
    int16_t err = ref_path(ref, path, sizeof path);
    if (err)
        return err;
    put_cat_info(catalogInfo, path, whichInfo);
    put_name_of(path, outName);
    if (fsSpec)
        fm_spec_of_path(path, fsSpec);
    if (parentRef) {
        if (fm_is_volume_root(path)) {
            gmemset(parentRef, 0, FSREF_SIZE); /* the root has no parent */
        } else {
            char parent[PATH_MAX];
            parent_path(path, parent, sizeof parent);
            make_ref(parentRef, parent);
        }
    }
    return noErr;
}

int16_t tb_PBGetCatalogInfo(gaddr pb, bool async)
{
    return tb_FSGetCatalogInfo(rd32(pb + FSRefParam_ref_off), rd32(pb + FSRefParam_whichInfo_off),
                               rd32(pb + FSRefParam_catInfo_off), rd32(pb + FSRefParam_outName_off),
                               rd32(pb + FSRefParam_spec_off), rd32(pb + FSRefParam_parentRef_off));
}

int16_t tb_FSSetCatalogInfo(gaddr ref, uint32_t whichInfo, gaddr catalogInfo)
{
    char path[PATH_MAX];
    int16_t err = ref_path(ref, path, sizeof path);
    return err ? err : set_cat_info(path, whichInfo, catalogInfo);
}

int16_t tb_PBSetCatalogInfo(gaddr pb, bool async)
{
    return tb_FSSetCatalogInfo(rd32(pb + FSRefParam_ref_off), rd32(pb + FSRefParam_whichInfo_off),
                               rd32(pb + FSRefParam_catInfo_off));
}

/* ---------------------------------------------------------------------- */
/* Iterators: a folder's entries as they were when it was opened (flat),
 * or the objects under it, depth first, for catalog searches (subtree). */

#define MAX_ITERATORS 64
#define ITER_TAG 0x46490000u /* 'FI' and the slot */

static struct {
    bool used, subtree;
    char root[PATH_MAX];
    char **list; /* flat */
    int count;
    uint32_t next; /* entries returned or looked at */
} iters[MAX_ITERATORS];

static void close_iter(int i)
{
    for (int k = 0; k < iters[i].count; k++)
        free(iters[i].list[k]);
    free(iters[i].list);
    memset(&iters[i], 0, sizeof iters[i]);
}

static int iter_slot(uint32_t it)
{
    uint32_t i = (it & 0xFFFF) - 1;
    return (it & 0xFFFF0000u) == ITER_TAG && i < MAX_ITERATORS && iters[i].used ? (int)i : -1;
}

int16_t tb_FSOpenIterator(gaddr container, uint32_t iteratorFlags, gaddr iterator)
{
    if (iteratorFlags > 1)
        return errFSBadIteratorFlags;
    char dir[PATH_MAX];
    int16_t err = ref_dir(container, dir, sizeof dir);
    if (err)
        return err;
    for (int i = 0; i < MAX_ITERATORS; i++) {
        if (iters[i].used)
            continue;
        iters[i].used = true;
        iters[i].subtree = iteratorFlags == 1;
        snprintf(iters[i].root, sizeof iters[i].root, "%s", dir);
        if (!iters[i].subtree)
            iters[i].count = fm_list_dir(dir, &iters[i].list);
        if (iterator)
            wr32(iterator, ITER_TAG | (uint32_t)(i + 1));
        return noErr;
    }
    return -42; /* tmfoErr */
}

int16_t tb_PBOpenIterator(gaddr pb, bool async)
{
    return tb_FSOpenIterator(rd32(pb + FSCatalogBulkParam_container_off),
                             rd32(pb + FSCatalogBulkParam_iteratorFlags_off), pb + FSCatalogBulkParam_iterator_off);
}

int16_t tb_FSCloseIterator(uint32_t iterator)
{
    int i = iter_slot(iterator);
    if (i < 0)
        return errFSIteratorNotFound;
    close_iter(i);
    return noErr;
}

int16_t tb_PBCloseIterator(gaddr pb, bool async)
{
    return tb_FSCloseIterator(rd32(pb + FSCatalogBulkParam_iterator_off));
}

/* One object's outputs, the n-th of a call. */
static void put_item(const char *path, uint32_t n, uint32_t which, gaddr cis, gaddr refs, gaddr specs, gaddr names)
{
    if (cis)
        put_cat_info(cis + n * FSCatalogInfo_SIZEOF, path, which);
    if (refs)
        make_ref(refs + n * FSREF_SIZE, path);
    if (specs)
        fm_spec_of_path(path, specs + n * FSSpec_SIZEOF);
    if (names)
        put_name_of(path, names + n * UNISTR_SIZE);
}

/* At the end, errFSNoMoreItems comes with the last objects. */
int16_t tb_FSGetCatalogInfoBulk(uint32_t iterator, uint32_t maximumObjects, gaddr actualObjects,
                                gaddr containerChanged, uint32_t whichInfo, gaddr catalogInfos, gaddr refs,
                                gaddr specs, gaddr names)
{
    if (actualObjects)
        wr32(actualObjects, 0);
    if (containerChanged)
        wr8(containerChanged, 0);
    int i = iter_slot(iterator);
    if (i < 0)
        return errFSIteratorNotFound;
    if (iters[i].subtree)
        return errFSIteratorNotSupported;
    if (!maximumObjects)
        return errFSBadItemCount;
    if (whichInfo & ~(uint32_t)kFSCatInfoGettableInfo)
        return errFSBadInfoBitmap;
    uint32_t n = 0;
    while (n < maximumObjects && iters[i].next < (uint32_t)iters[i].count) {
        const char *path = iters[i].list[iters[i].next++];
        if (!fm_host_exists(path))
            continue; /* deleted since */
        put_item(path, n++, whichInfo, catalogInfos, refs, specs, names);
    }
    if (actualObjects)
        wr32(actualObjects, n);
    return n < maximumObjects ? errFSNoMoreItems : noErr;
}

int16_t tb_PBGetCatalogInfoBulk(gaddr pb, bool async)
{
    return tb_FSGetCatalogInfoBulk(
        rd32(pb + FSCatalogBulkParam_iterator_off), rd32(pb + FSCatalogBulkParam_maximumItems_off),
        pb + FSCatalogBulkParam_actualItems_off, pb + FSCatalogBulkParam_containerChanged_off,
        rd32(pb + FSCatalogBulkParam_whichInfo_off), rd32(pb + FSCatalogBulkParam_catalogInfo_off),
        rd32(pb + FSCatalogBulkParam_refs_off), rd32(pb + FSCatalogBulkParam_specs_off),
        rd32(pb + FSCatalogBulkParam_names_off));
}

/* ---- Catalog searches */

enum {
    fsSBPartialName = 1, fsSBFullName = 2, fsSBFlAttrib = 4, fsSBFlFndrInfo = 8, fsSBDrNmFls = 16,
    fsSBFlLgLen = 32, fsSBFlPyLen = 64, fsSBFlRLgLen = 128, fsSBFlRPyLen = 256, fsSBFlCrDat = 512,
    fsSBFlMdDat = 1024, fsSBFlBkDat = 2048, fsSBFlXFndrInfo = 4096, fsSBFlParID = 8192, fsSBNegate = 16384,
    fsSBNodeID = 0x8000, fsSBAttributeModDate = 0x10000, fsSBAccessDate = 0x20000, fsSBPermissions = 0x40000,
};

typedef struct {
    gaddr rec, i1, i2;
    uint32_t bits;
    char name[NAME_MAX * 4 + 1]; /* the name looked for, folded */
    uint32_t skip, seen, found, want, which;
    gaddr cis, refs, specs, names;
} search;

static void fold(char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z')
            *s = (char)(*s - 'A' + 'a');
}

static bool in_range64(const search *s, uint32_t off)
{
    uint64_t v = get64(s->rec + off);
    return v >= get64(s->i1 + off) && v <= get64(s->i2 + off);
}

static bool in_range32(const search *s, uint32_t off)
{
    uint32_t v = rd32(s->rec + off);
    return v >= rd32(s->i1 + off) && v <= rd32(s->i2 + off);
}

static bool masked(const search *s, uint32_t off, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if ((rd8(s->rec + off + i) ^ rd8(s->i1 + off + i)) & rd8(s->i2 + off + i))
            return false;
    return true;
}

static bool matches(search *s, const char *path)
{
    gmemset(s->rec, 0, FSCatalogInfo_SIZEOF);
    put_cat_info(s->rec, path, kFSCatInfoGettableInfo);
    bool folder = rd16(s->rec + FSCatalogInfo_nodeFlags_off) & 0x10;
    bool ok = true;
    uint32_t b = s->bits;
    if (b & (fsSBPartialName | fsSBFullName)) {
        char name[NAME_MAX * 2 + 1];
        fm_display_name(path, name, sizeof name);
        for (char *p = name; *p; p++)
            if (*p == ':')
                *p = '/';
        fold(name);
        ok = b & fsSBFullName ? !strcmp(name, s->name) : strstr(name, s->name) != NULL;
    }
    if (ok && (b & fsSBFlAttrib))
        ok = !((rd16(s->rec + FSCatalogInfo_nodeFlags_off) ^ rd16(s->i1 + FSCatalogInfo_nodeFlags_off)) &
               rd16(s->i2 + FSCatalogInfo_nodeFlags_off));
    if (ok && (b & fsSBFlFndrInfo))
        ok = masked(s, FSCatalogInfo_finderInfo_off, 16);
    if (ok && (b & fsSBFlXFndrInfo))
        ok = masked(s, FSCatalogInfo_extFinderInfo_off, 16);
    if (ok && (b & fsSBPermissions))
        ok = masked(s, FSCatalogInfo_permissions_off, 16);
    if (!folder) {
        if (ok && (b & fsSBFlLgLen))
            ok = in_range64(s, FSCatalogInfo_dataLogicalSize_off);
        if (ok && (b & fsSBFlPyLen))
            ok = in_range64(s, FSCatalogInfo_dataPhysicalSize_off);
        if (ok && (b & fsSBFlRLgLen))
            ok = in_range64(s, FSCatalogInfo_rsrcLogicalSize_off);
        if (ok && (b & fsSBFlRPyLen))
            ok = in_range64(s, FSCatalogInfo_rsrcPhysicalSize_off);
    } else if (ok && (b & fsSBDrNmFls)) {
        ok = in_range32(s, FSCatalogInfo_valence_off);
    }
    if (ok && (b & fsSBFlCrDat))
        ok = in_range64(s, FSCatalogInfo_createDate_off);
    if (ok && (b & fsSBFlMdDat))
        ok = in_range64(s, FSCatalogInfo_contentModDate_off);
    if (ok && (b & fsSBFlBkDat))
        ok = in_range64(s, FSCatalogInfo_backupDate_off);
    if (ok && (b & fsSBAttributeModDate))
        ok = in_range64(s, FSCatalogInfo_attributeModDate_off);
    if (ok && (b & fsSBAccessDate))
        ok = in_range64(s, FSCatalogInfo_accessDate_off);
    if (ok && (b & fsSBFlParID))
        ok = in_range32(s, FSCatalogInfo_parentDirID_off);
    if (ok && (b & fsSBNodeID))
        ok = in_range32(s, FSCatalogInfo_nodeID_off);
    return b & fsSBNegate ? !ok : ok;
}

/* Depth first under a folder; false once enough are found.  The Host
 * folder (the host's whole disk) is not gone into. */
static bool search_under(search *s, const char *dir, int depth)
{
    char **paths;
    int n = fm_list_dir(dir, &paths);
    bool go_on = true;
    for (int i = 0; i < n; i++) {
        if (go_on && strcmp(paths[i], "/")) {
            if (s->seen++ >= s->skip && matches(s, paths[i])) {
                put_item(paths[i], s->found++, s->which, s->cis, s->refs, s->specs, s->names);
                go_on = s->found < s->want;
            }
            if (go_on && depth < 64 && fm_is_dir(paths[i]))
                go_on = search_under(s, paths[i], depth + 1);
        }
        free(paths[i]);
    }
    free(paths);
    return go_on;
}

int16_t tb_FSCatalogSearch(uint32_t iterator, gaddr searchCriteria, uint32_t maximumObjects, gaddr actualObjects,
                           gaddr containerChanged, uint32_t whichInfo, gaddr catalogInfos, gaddr refs, gaddr specs,
                           gaddr names)
{
    if (actualObjects)
        wr32(actualObjects, 0);
    if (containerChanged)
        wr8(containerChanged, 0);
    int i = iter_slot(iterator);
    if (i < 0)
        return errFSIteratorNotFound;
    if (!iters[i].subtree)
        return errFSIteratorNotSupported;
    if (!maximumObjects)
        return errFSBadItemCount;
    if (whichInfo & ~(uint32_t)kFSCatInfoGettableInfo)
        return errFSBadInfoBitmap;
    if (!searchCriteria)
        return errFSBadSearchParams;
    search s = { .i1 = rd32(searchCriteria + FSSearchParams_searchInfo1_off),
                 .i2 = rd32(searchCriteria + FSSearchParams_searchInfo2_off),
                 .bits = rd32(searchCriteria + FSSearchParams_searchBits_off),
                 .skip = iters[i].next, .want = maximumObjects, .which = whichInfo,
                 .cis = catalogInfos, .refs = refs, .specs = specs, .names = names };
    if (s.bits & (fsSBPartialName | fsSBFullName)) {
        int16_t err = uni_to_host(rd32(searchCriteria + FSSearchParams_searchName_off),
                                  rd32(searchCriteria + FSSearchParams_searchNameLength_off), s.name, sizeof s.name);
        if (err)
            return errFSBadSearchParams;
        for (char *p = s.name; *p; p++)
            if (*p == ':')
                *p = '/';
        fold(s.name);
    }
    if ((s.bits & ~(uint32_t)(fsSBPartialName | fsSBFullName | fsSBNegate)) && (!s.i1 || !s.i2))
        return errFSBadSearchParams;
    s.rec = tb_NewPtr(FSCatalogInfo_SIZEOF, true, true);
    if (!s.rec)
        return memFullErr;
    bool done = search_under(&s, iters[i].root, 0);
    tb_DisposePtr(s.rec);
    iters[i].next = s.seen;
    if (actualObjects)
        wr32(actualObjects, s.found);
    return done ? errFSNoMoreItems : noErr;
}

int16_t tb_PBCatalogSearch(gaddr pb, bool async)
{
    return tb_FSCatalogSearch(
        rd32(pb + FSCatalogBulkParam_iterator_off), rd32(pb + FSCatalogBulkParam_searchParams_off),
        rd32(pb + FSCatalogBulkParam_maximumItems_off), pb + FSCatalogBulkParam_actualItems_off,
        pb + FSCatalogBulkParam_containerChanged_off, rd32(pb + FSCatalogBulkParam_whichInfo_off),
        rd32(pb + FSCatalogBulkParam_catalogInfo_off), rd32(pb + FSCatalogBulkParam_refs_off),
        rd32(pb + FSCatalogBulkParam_specs_off), rd32(pb + FSCatalogBulkParam_names_off));
}

/* ---------------------------------------------------------------------- */
/* Forks: every file has a data fork (the empty name) and a resource fork
 * ("RESOURCE_FORK"), and there are no others. */

static const char rsrc_fork_name[] = "RESOURCE_FORK";

/* 0 the data fork, 1 the resource fork, -1 another. */
static int fork_kind(uint32_t len, gaddr name)
{
    if (!len)
        return 0;
    if (!name || len != sizeof rsrc_fork_name - 1)
        return -1;
    for (uint32_t i = 0; i < len; i++)
        if (rd16(name + 2 * i) != (uint8_t)rsrc_fork_name[i])
            return -1;
    return 1;
}

static void put_fork_name(gaddr out, bool rsrc) { host_to_uni(rsrc ? rsrc_fork_name : "", out); }

int16_t tb_FSGetDataForkName(gaddr dataForkName)
{
    put_fork_name(dataForkName, false);
    return noErr;
}

int16_t tb_FSGetResourceForkName(gaddr resourceForkName)
{
    put_fork_name(resourceForkName, true);
    return noErr;
}

/* A file (not a folder) from its FSRef. */
static int16_t ref_file(gaddr ref, char *out, size_t outsize)
{
    int16_t err = ref_path(ref, out, outsize);
    return err ? err : fm_is_dir(out) ? notAFileErr : noErr;
}

int16_t tb_FSCreateFork(gaddr ref, uint32_t forkNameLength, gaddr forkName)
{
    char path[PATH_MAX];
    int16_t err = ref_file(ref, path, sizeof path);
    if (err)
        return err;
    return fork_kind(forkNameLength, forkName) < 0 ? errFSBadForkName : errFSForkExists;
}

int16_t tb_PBCreateFork(gaddr pb, bool async)
{
    return tb_FSCreateFork(rd32(pb + FSForkIOParam_ref_off), rd32(pb + FSForkIOParam_forkNameLength_off),
                           rd32(pb + FSForkIOParam_forkName_off));
}

/* The data and resource forks always exist: deleting one empties it. */
int16_t tb_FSDeleteFork(gaddr ref, uint32_t forkNameLength, gaddr forkName)
{
    char path[PATH_MAX];
    int16_t err = ref_file(ref, path, sizeof path);
    if (err)
        return err;
    int kind = fork_kind(forkNameLength, forkName);
    if (kind < 0)
        return errFSForkNotFound;
    fm_info in;
    fm_info_of(path, &in);
    if (kind ? in.rsrc_open : in.data_open)
        return fBsyErr;
    int16_t rn = 0;
    if ((err = fm_open_fork(path, kind == 1, 2 /* fsWrPerm */, &rn)))
        return err;
    err = fm_fork_set_eof(rn, 1 /* fsFromStart */, 0);
    int16_t cerr = fm_close_fork(rn);
    return err ? err : cerr;
}

int16_t tb_PBDeleteFork(gaddr pb, bool async)
{
    return tb_FSDeleteFork(rd32(pb + FSForkIOParam_ref_off), rd32(pb + FSForkIOParam_forkNameLength_off),
                           rd32(pb + FSForkIOParam_forkName_off));
}

/* The forks with something in them, the data fork first; the iterator's
 * first long (initialize) says how many were returned. */
int16_t tb_FSIterateForks(gaddr ref, gaddr forkIterator, gaddr forkName, gaddr forkSize, gaddr forkPhysicalSize)
{
    char path[PATH_MAX];
    int16_t err = ref_path(ref, path, sizeof path);
    if (err)
        return err;
    if (!forkIterator)
        return paramErr;
    fm_info in;
    fm_info_of(path, &in);
    uint32_t index = rd32(forkIterator);
    int kind = -1;
    for (uint32_t k = 0, i = 0; k < 2 && kind < 0; k++) {
        bool there = !in.dir && (k ? in.rsrc_len : in.data_len);
        if (there && i++ == index)
            kind = (int)k;
    }
    if (kind < 0)
        return errFSNoMoreItems;
    wr32(forkIterator, index + 1);
    uint32_t len = kind ? in.rsrc_len : in.data_len;
    put_fork_name(forkName, kind == 1);
    if (forkSize)
        put64(forkSize, len);
    if (forkPhysicalSize)
        put64(forkPhysicalSize, (len + 511u) & ~511u);
    return noErr;
}

int16_t tb_PBIterateForks(gaddr pb, bool async)
{
    return tb_FSIterateForks(rd32(pb + FSForkIOParam_ref_off), pb + FSForkIOParam_forkIterator_off,
                             rd32(pb + FSForkIOParam_outForkName_off), pb + FSForkIOParam_positionOffset_off,
                             pb + FSForkIOParam_allocationAmount_off);
}

int16_t tb_FSOpenFork(gaddr ref, uint32_t forkNameLength, gaddr forkName, int8_t permissions, gaddr forkRefNum)
{
    char path[PATH_MAX];
    int16_t err = ref_file(ref, path, sizeof path);
    if (err)
        return err;
    int kind = fork_kind(forkNameLength, forkName);
    if (kind < 0)
        return errFSForkNotFound;
    int16_t rn = 0;
    err = fm_open_fork(path, kind == 1, (uint8_t)permissions, &rn);
    if (rn && forkRefNum)
        wr16(forkRefNum, (uint16_t)rn);
    return err;
}

int16_t tb_PBOpenFork(gaddr pb, bool async)
{
    return tb_FSOpenFork(rd32(pb + FSForkIOParam_ref_off), rd32(pb + FSForkIOParam_forkNameLength_off),
                         rd32(pb + FSForkIOParam_forkName_off), (int8_t)rd8(pb + FSForkIOParam_permissions_off),
                         pb + FSForkIOParam_forkRefNum_off);
}

/* Newline mode is not for forks. */
static int16_t fork_rw(int16_t rn, bool write, uint16_t mode, int64_t off, uint32_t count, gaddr buf, gaddr actual)
{
    if (actual)
        wr32(actual, 0);
    if (mode & 0x80)
        return errFSBadPosMode;
    if (!buf && count)
        return paramErr;
    uint32_t n = 0;
    int16_t err = fm_fork_io(rn, write, mode, off, count, buf, &n);
    if (actual)
        wr32(actual, n);
    return err;
}

int16_t tb_FSReadFork(int16_t forkRefNum, uint16_t positionMode, gaddr positionOffset, uint32_t requestCount,
                      gaddr buffer, gaddr actualCount)
{
    return fork_rw(forkRefNum, false, positionMode, positionOffset ? (int64_t)get64(positionOffset) : 0,
                   requestCount, buffer, actualCount);
}

int16_t tb_FSWriteFork(int16_t forkRefNum, uint16_t positionMode, gaddr positionOffset, uint32_t requestCount,
                       gaddr buffer, gaddr actualCount)
{
    return fork_rw(forkRefNum, true, positionMode, positionOffset ? (int64_t)get64(positionOffset) : 0,
                   requestCount, buffer, actualCount);
}

static int16_t pb_fork_rw(gaddr pb, bool write)
{
    return fork_rw((int16_t)rd16(pb + FSForkIOParam_forkRefNum_off), write, rd16(pb + FSForkIOParam_positionMode_off),
                   (int64_t)get64(pb + FSForkIOParam_positionOffset_off), rd32(pb + FSForkIOParam_requestCount_off),
                   rd32(pb + FSForkIOParam_buffer_off), pb + FSForkIOParam_actualCount_off);
}

int16_t tb_PBReadFork(gaddr pb, bool async) { return pb_fork_rw(pb, false); }
int16_t tb_PBWriteFork(gaddr pb, bool async) { return pb_fork_rw(pb, true); }

/* Where a fork's mark is, and how long it is. */
static int16_t fork_where(int16_t rn, gaddr pos, gaddr size)
{
    bool rsrc, write;
    uint32_t p, len;
    if (!fm_fork_state(rn, NULL, 0, &rsrc, &write, &p, &len))
        return rfNumErr;
    if (pos)
        put64(pos, p);
    if (size)
        put64(size, len);
    return noErr;
}

int16_t tb_FSGetForkPosition(int16_t forkRefNum, gaddr position) { return fork_where(forkRefNum, position, 0); }
int16_t tb_FSGetForkSize(int16_t forkRefNum, gaddr forkSize) { return fork_where(forkRefNum, 0, forkSize); }

int16_t tb_PBGetForkPosition(gaddr pb, bool async)
{
    return fork_where((int16_t)rd16(pb + FSForkIOParam_forkRefNum_off), pb + FSForkIOParam_positionOffset_off, 0);
}

int16_t tb_PBGetForkSize(gaddr pb, bool async)
{
    return fork_where((int16_t)rd16(pb + FSForkIOParam_forkRefNum_off), 0, pb + FSForkIOParam_positionOffset_off);
}

static int16_t set_fork_position(int16_t rn, uint16_t mode, int64_t off)
{
    return mode & 0x80 ? errFSBadPosMode : fm_fork_seek(rn, mode, off);
}

static int16_t set_fork_size(int16_t rn, uint16_t mode, int64_t off)
{
    return mode & 0x80 ? errFSBadPosMode : fm_fork_set_eof(rn, mode, off);
}

int16_t tb_FSSetForkPosition(int16_t forkRefNum, uint16_t positionMode, gaddr positionOffset)
{
    return set_fork_position(forkRefNum, positionMode, positionOffset ? (int64_t)get64(positionOffset) : 0);
}

int16_t tb_FSSetForkSize(int16_t forkRefNum, uint16_t positionMode, gaddr positionOffset)
{
    return set_fork_size(forkRefNum, positionMode, positionOffset ? (int64_t)get64(positionOffset) : 0);
}

int16_t tb_PBSetForkPosition(gaddr pb, bool async)
{
    return set_fork_position((int16_t)rd16(pb + FSForkIOParam_forkRefNum_off), rd16(pb + FSForkIOParam_positionMode_off),
                             (int64_t)get64(pb + FSForkIOParam_positionOffset_off));
}

int16_t tb_PBSetForkSize(gaddr pb, bool async)
{
    return set_fork_size((int16_t)rd16(pb + FSForkIOParam_forkRefNum_off), rd16(pb + FSForkIOParam_positionMode_off),
                         (int64_t)get64(pb + FSForkIOParam_positionOffset_off));
}

/* Space is the host's to find: all of it is there at once, and the
 * fork's length stays as it was. */
static int16_t allocate_fork(int16_t rn, uint16_t flags, uint64_t count, gaddr actual)
{
    if (actual)
        put64(actual, 0);
    if (flags & ~7u)
        return errFSBadAllocFlags;
    bool rsrc, write;
    uint32_t pos, len;
    if (!fm_fork_state(rn, NULL, 0, &rsrc, &write, &pos, &len))
        return rfNumErr;
    if (!write)
        return wrPermErr;
    if (actual)
        put64(actual, count);
    return noErr;
}

int16_t tb_FSAllocateFork(int16_t forkRefNum, uint16_t flags, uint16_t positionMode, gaddr positionOffset,
                          gaddr requestCount, gaddr actualCount)
{
    return allocate_fork(forkRefNum, flags, requestCount ? get64(requestCount) : 0, actualCount);
}

int16_t tb_PBAllocateFork(gaddr pb, bool async)
{
    return allocate_fork((int16_t)rd16(pb + FSForkIOParam_forkRefNum_off),
                         rd16(pb + FSForkIOParam_allocationFlags_off), get64(pb + FSForkIOParam_allocationAmount_off),
                         pb + FSForkIOParam_allocationAmount_off);
}

int16_t tb_FSFlushFork(int16_t forkRefNum) { return fm_flush_fork(forkRefNum); }
int16_t tb_FSCloseFork(int16_t forkRefNum) { return fm_close_fork(forkRefNum); }

int16_t tb_PBFlushFork(gaddr pb, bool async)
{
    return fm_flush_fork((int16_t)rd16(pb + FSForkIOParam_forkRefNum_off));
}

int16_t tb_PBCloseFork(gaddr pb, bool async)
{
    return fm_close_fork((int16_t)rd16(pb + FSForkIOParam_forkRefNum_off));
}

/* An open fork, given or the next after the iterator (a refnum) on a
 * volume (0 for all of them). */
int16_t tb_FSGetForkCBInfo(int16_t desiredRefNum, int16_t volume, gaddr iterator, gaddr actualRefNum, gaddr forkInfo,
                           gaddr ref, gaddr outForkName)
{
    char path[PATH_MAX];
    bool rsrc, write;
    uint32_t pos, len;
    int16_t rn = desiredRefNum;
    if (!rn) {
        rn = iterator ? (int16_t)rd16(iterator) : 0;
        while ((rn = fm_next_fork(rn)) != 0) {
            if (fm_fork_state(rn, path, sizeof path, &rsrc, &write, &pos, &len) &&
                (!volume || (path[0] && fm_vref_of_path(path) == volume)))
                break;
        }
        if (!rn)
            return errFSNoMoreItems;
        if (iterator)
            wr16(iterator, (uint16_t)rn);
    }
    if (!fm_fork_state(rn, path, sizeof path, &rsrc, &write, &pos, &len))
        return rfNumErr;
    if (actualRefNum)
        wr16(actualRefNum, (uint16_t)rn);
    if (forkInfo) {
        gmemset(forkInfo, 0, FSForkInfo_SIZEOF);
        wr8(forkInfo + FSForkInfo_flags_off, (uint8_t)((write ? 0x01 : 0) | (rsrc ? 0x02 : 0)));
        wr8(forkInfo + FSForkInfo_permissions_off, write ? 3 : 1); /* fsRdWrPerm, fsRdPerm */
        if (path[0]) {
            wr16(forkInfo + FSForkInfo_volume_off, (uint16_t)fm_vref_of_path(path));
            wr32(forkInfo + FSForkInfo_nodeID_off, fm_cat_id(path));
        }
        wr32(forkInfo + FSForkInfo_forkID_off, rsrc ? 0xFF : 0);
        put64(forkInfo + FSForkInfo_currentPosition_off, pos);
        put64(forkInfo + FSForkInfo_logicalEOF_off, len);
        put64(forkInfo + FSForkInfo_physicalEOF_off, (len + 511u) & ~511u);
    }
    if (ref) {
        if (path[0])
            make_ref(ref, path);
        else
            gmemset(ref, 0, FSREF_SIZE);
    }
    put_fork_name(outForkName, rsrc);
    return noErr;
}

int16_t tb_PBGetForkCBInfo(gaddr pb, bool async)
{
    return tb_FSGetForkCBInfo((int16_t)rd16(pb + FSForkCBInfoParam_desiredRefNum_off),
                              (int16_t)rd16(pb + FSForkCBInfoParam_volumeRefNum_off),
                              pb + FSForkCBInfoParam_iterator_off, pb + FSForkCBInfoParam_actualRefNum_off,
                              rd32(pb + FSForkCBInfoParam_forkInfo_off), rd32(pb + FSForkCBInfoParam_ref_off),
                              rd32(pb + FSForkCBInfoParam_forkName_off));
}

/* ---------------------------------------------------------------------- */
/* Volumes */

/* The host's size of a volume's folder, in bytes. */
static void host_space(const char *root, uint64_t *total, uint64_t *avail)
{
    struct statvfs sv;
    if (statvfs(root, &sv) != 0) {
        *total = *avail = 0;
        return;
    }
    *total = (uint64_t)sv.f_blocks * sv.f_frsize;
    *avail = (uint64_t)sv.f_bavail * sv.f_frsize;
}

int16_t tb_FSGetVolumeInfo(int16_t volume, uint32_t volumeIndex, gaddr actualVolume, uint32_t whichInfo, gaddr info,
                           gaddr volumeName, gaddr rootDirectory)
{
    if (whichInfo & ~(uint32_t)kFSVolInfoGettableInfo)
        return errFSBadInfoBitmap;
    int16_t vref, drive, driver;
    char root[PATH_MAX];
    uint8_t name[32];
    if (!fm_volume(volumeIndex, volume, &vref, root, sizeof root, name, &drive, &driver))
        return nsvErr;
    if (actualVolume)
        wr16(actualVolume, (uint16_t)vref);
    if (volumeName) {
        char utf8[128];
        macroman_to_utf8(name + 1, name[0], utf8, sizeof utf8);
        host_to_uni(utf8, volumeName);
    }
    make_ref(rootDirectory, root);
    if (info && whichInfo) {
        uint32_t created = 0, modified = 0;
        fm_info in;
        if (fm_info_of(root, &in)) {
            created = in.created;
            modified = in.modified;
        }
        uint64_t total, avail;
        host_space(root, &total, &avail);
        const uint32_t block = 4096;
        if (whichInfo & 0x0001)
            put_utc(info + FSVolumeInfo_createDate_off, created);
        if (whichInfo & 0x0002)
            put_utc(info + FSVolumeInfo_modifyDate_off, modified);
        if (whichInfo & 0x0004)
            gmemset(info + FSVolumeInfo_backupDate_off, 0, 8);
        if (whichInfo & 0x0008)
            gmemset(info + FSVolumeInfo_checkedDate_off, 0, 8);
        if (whichInfo & 0x0010)
            wr32(info + FSVolumeInfo_fileCount_off, 0); /* not counted */
        if (whichInfo & 0x0020)
            wr32(info + FSVolumeInfo_folderCount_off, 0);
        if (whichInfo & 0x0040) {
            put64(info + FSVolumeInfo_totalBytes_off, total);
            put64(info + FSVolumeInfo_freeBytes_off, avail);
        }
        if (whichInfo & 0x0080) {
            wr32(info + FSVolumeInfo_blockSize_off, block);
            wr32(info + FSVolumeInfo_totalBlocks_off, total / block > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)(total / block));
            wr32(info + FSVolumeInfo_freeBlocks_off, avail / block > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)(avail / block));
        }
        if (whichInfo & 0x0100)
            wr32(info + FSVolumeInfo_nextAllocation_off, 0);
        if (whichInfo & 0x0200)
            wr32(info + FSVolumeInfo_rsrcClumpSize_off, 65536);
        if (whichInfo & 0x0400)
            wr32(info + FSVolumeInfo_dataClumpSize_off, 65536);
        if (whichInfo & 0x0800)
            wr32(info + FSVolumeInfo_nextCatalogID_off, 0x20000); /* past every file number */
        if (whichInfo & 0x1000)
            gmemset(info + FSVolumeInfo_finderInfo_off, 0, 32);
        if (whichInfo & 0x2000)
            wr16(info + FSVolumeInfo_flags_off, vref == -1 ? 0 : 0x8080); /* mounted folders are locked */
        if (whichInfo & 0x4000) {
            wr16(info + FSVolumeInfo_filesystemID_off, 0);
            wr16(info + FSVolumeInfo_signature_off, 0x482B); /* 'H+' */
        }
        if (whichInfo & 0x8000) {
            wr16(info + FSVolumeInfo_driveNumber_off, (uint16_t)drive);
            wr16(info + FSVolumeInfo_driverRefNum_off, (uint16_t)driver);
        }
    }
    return noErr;
}

int16_t tb_PBGetVolumeInfo(gaddr pb, bool async)
{
    return tb_FSGetVolumeInfo((int16_t)rd16(pb + FSVolumeInfoParam_ioVRefNum_off),
                              rd32(pb + FSVolumeInfoParam_volumeIndex_off), pb + FSVolumeInfoParam_ioVRefNum_off,
                              rd32(pb + FSVolumeInfoParam_whichInfo_off), rd32(pb + FSVolumeInfoParam_volumeInfo_off),
                              rd32(pb + FSVolumeInfoParam_volumeName_off), rd32(pb + FSVolumeInfoParam_ref_off));
}

/* Nothing of a volume's is kept (as PBSetVInfo); a mounted folder is
 * locked. */
int16_t tb_FSSetVolumeInfo(int16_t volume, uint32_t whichInfo, gaddr info)
{
    if (whichInfo & ~(uint32_t)kFSVolInfoSettableInfo)
        return errFSBadInfoBitmap;
    int16_t vref, drive, driver;
    char root[PATH_MAX];
    uint8_t name[32];
    if (!fm_volume(0, volume, &vref, root, sizeof root, name, &drive, &driver))
        return nsvErr;
    return vref == -1 ? noErr : wPrErr;
}

int16_t tb_PBSetVolumeInfo(gaddr pb, bool async)
{
    return tb_FSSetVolumeInfo((int16_t)rd16(pb + FSVolumeInfoParam_ioVRefNum_off),
                              rd32(pb + FSVolumeInfoParam_whichInfo_off), rd32(pb + FSVolumeInfoParam_volumeInfo_off));
}

/* PBHGetVInfo's answer with the volume's real size in bytes after it. */
int16_t tb_PBXGetVolInfo(gaddr pb, bool async)
{
    int16_t err = tb_PBHGetVInfo(pb, async);
    if (err)
        return err;
    int16_t vref, drive, driver;
    char root[PATH_MAX];
    uint8_t name[32];
    uint64_t total = 0, avail = 0;
    if (fm_volume(0, (int16_t)rd16(pb + IOParam_ioVRefNum_off), &vref, root, sizeof root, name, &drive, &driver))
        host_space(root, &total, &avail);
    put64(pb + XVolumeParam_ioVTotalBytes_off, total);
    put64(pb + XVolumeParam_ioVFreeBytes_off, avail);
    return noErr;
}

/* PBMakeFSSpec: FSMakeFSSpec with ioMisc pointing at the FSSpec. */
int16_t tb_PBMakeFSSpec(gaddr pb, bool async)
{
    gaddr spec = rd32(pb + HIoParam_ioMisc_off);
    if (!spec)
        return paramErr;
    return tb_FSMakeFSSpec((int16_t)rd16(pb + IOParam_ioVRefNum_off), (int32_t)rd32(pb + HFileParam_ioDirID_off),
                           rd32(pb + IOParam_ioNamePtr_off), spec);
}

/* ProDOS file types and AFP short names are an AppleShare server's; the
 * local volume has none (its GetVolParms says so). */
int16_t tb_PBGetXCatInfo(gaddr pb, bool async) { return paramErr; }

/* _AllocContig: space is the host's, contiguous or not. */
int16_t tb_PBAllocContig(gaddr pb, bool async) { return tb_PBAllocate(pb, async); }

int16_t tb_AllocContig(int16_t refNum, gaddr count)
{
    bool rsrc, write;
    uint32_t pos, len;
    if (!fm_fork_state(refNum, NULL, 0, &rsrc, &write, &pos, &len))
        return rfNumErr;
    return write ? noErr : wrPermErr; /* *count allocated, as asked */
}

/* The volume's open files written out; there is no cache to discard. */
int16_t tb_PBHTrashVolumeCaches(gaddr pb)
{
    int16_t vref, drive, driver;
    char root[PATH_MAX];
    uint8_t name[32];
    if (!fm_volume(0, (int16_t)rd16(pb + IOParam_ioVRefNum_off), &vref, root, sizeof root, name, &drive, &driver))
        return nsvErr;
    fm_flush_volume();
    return noErr;
}

void machine_pass_tick(void);

/* Wait for an asynchronous call to finish: Duration is milliseconds, or
 * microseconds when negative. */
int16_t tb_PBWaitIOComplete(gaddr paramBlock, int32_t timeout)
{
    if (!paramBlock)
        return paramErr;
    int64_t us = timeout == 0x7FFFFFFF ? -1 : timeout >= 0 ? (int64_t)timeout * 1000 : -(int64_t)timeout;
    while ((int16_t)rd16(paramBlock + IOParam_ioResult_off) > 0) {
        if (us == 0 || (g_cpu && g_cpu->halted))
            return kMPTimeoutErr;
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
        serial_poll();
        appletalk_poll();
        machine_pass_tick();
        if (us > 0)
            us = us > 1000 ? us - 1000 : 0;
    }
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* The File System Manager: no foreign file systems but those a program
 * installs, kept as their FSDs for GetFSInfo; none has a volume here. */

enum {
    fsmFFSNotFoundErr = -431, fsmBadFSDLenErr = -434, fsmDuplicateFSIDErr = -435, fsmUnknownFSMMessageErr = -438,
};
enum { FSD_fsdLength = 4, FSD_fileSystemFSID = 8 };

#define MAX_FSDS 16
static gaddr fsds[MAX_FSDS];
static int nfsds;

static int fsd_index(int16_t fsid)
{
    for (int i = 0; i < nfsds; i++)
        if ((int16_t)rd16(fsds[i] + FSD_fileSystemFSID) == fsid)
            return i;
    return -1;
}

int16_t tb_InstallFS(gaddr fsdPtr)
{
    if (!fsdPtr || rd16(fsdPtr + FSD_fsdLength) < 10)
        return fsmBadFSDLenErr;
    int16_t fsid = (int16_t)rd16(fsdPtr + FSD_fileSystemFSID);
    if (fsid == 0 || fsd_index(fsid) >= 0)
        return fsmDuplicateFSIDErr; /* 0 is HFS's */
    if (nfsds >= MAX_FSDS)
        return memFullErr;
    fsds[nfsds++] = fsdPtr;
    wr32(fsdPtr, 0); /* fsdLink */
    if (nfsds > 1)
        wr32(fsds[nfsds - 2], fsdPtr);
    return noErr;
}

int16_t tb_RemoveFS(int16_t fsid)
{
    int i = fsd_index(fsid);
    if (i < 0)
        return fsmFFSNotFoundErr;
    memmove(&fsds[i], &fsds[i + 1], (size_t)(nfsds - i - 1) * sizeof fsds[0]);
    nfsds--;
    for (int k = 0; k < nfsds; k++)
        wr32(fsds[k], k + 1 < nfsds ? fsds[k + 1] : 0);
    return noErr;
}

/* By index (-1, key from 1), by FSID (0) or by a refnum (1: every volume
 * and file here is HFS's, which has no FSD). */
int16_t tb_GetFSInfo(int16_t selector, int16_t key, gaddr bufSize, gaddr fsdPtr)
{
    int i = selector == -1 ? key - 1 : selector == 0 ? fsd_index(key) : -1;
    if (i < 0 || i >= nfsds)
        return fsmFFSNotFoundErr;
    if (!bufSize || !fsdPtr)
        return paramErr;
    uint16_t have = rd16(fsds[i] + FSD_fsdLength), want = rd16(bufSize);
    uint16_t n = want < have ? want : have;
    gmemcpy(fsdPtr, fsds[i], n);
    wr16(bufSize, n);
    return noErr;
}

int16_t tb_SetFSInfo(int16_t fsid, int16_t bufSize, gaddr fsdPtr)
{
    int i = fsd_index(fsid);
    if (i < 0)
        return fsmFFSNotFoundErr;
    if (!fsdPtr || bufSize < 0)
        return paramErr;
    uint16_t have = rd16(fsds[i] + FSD_fsdLength);
    gaddr link = rd32(fsds[i]);
    gmemcpy(fsds[i], fsdPtr, (uint32_t)bufSize < have ? (uint32_t)bufSize : have);
    wr32(fsds[i], link); /* the queue stays the File System Manager's */
    return noErr;
}

/* fsmNopMessage and fsmDrvQElChangedMessage need nothing done; the icon
 * of a foreign file system's disk there is none of. */
int16_t tb_InformFSM(int16_t theMessage, gaddr paramBlock)
{
    if (theMessage == 0 || theMessage == 1)
        return noErr;
    return theMessage == 2 ? fsmFFSNotFoundErr : fsmUnknownFSMMessageErr;
}

/* A message for an installed file system: none of its volumes is here,
 * so there is nothing for it to answer. */
int16_t tb_InformFFS(int16_t fsid, gaddr paramBlock) { return fsd_index(fsid) < 0 ? fsmFFSNotFoundErr : noErr; }

/* ---------------------------------------------------------------------- */
/* The Resource Manager on FSRefs */

int16_t tb_FSOpenResFile(gaddr ref, int8_t permission)
{
    char path[PATH_MAX];
    int16_t err = ref_file(ref, path, sizeof path);
    if (err) {
        set_res_err(err);
        return -1;
    }
    return rm_open_path(path, permission);
}

void tb_FSCreateResFile(gaddr parentRef, uint32_t nameLength, gaddr name, uint32_t whichInfo, gaddr catalogInfo,
                        gaddr newRef, gaddr newSpec)
{
    char path[PATH_MAX];
    int16_t err = whichInfo & ~(uint32_t)kFSCatInfoSettableInfo ? errFSBadInfoBitmap
                                                                 : child_of(parentRef, nameLength, name, path, sizeof path);
    if (!err && fm_host_exists(path))
        err = dupFNErr;
    if (!err && !fm_writable(path))
        err = wPrErr;
    if (err) {
        set_res_err(err);
        return;
    }
    uint32_t type = 0, creator = 0;
    if ((whichInfo & kFSCatInfoFinderInfo) && catalogInfo) {
        type = rd32(catalogInfo + FSCatalogInfo_finderInfo_off);
        creator = rd32(catalogInfo + FSCatalogInfo_finderInfo_off + 4);
    }
    rm_create_path(path, type, creator);
    if (rd16(LM_ResErr))
        return;
    if (whichInfo && catalogInfo)
        set_cat_info(path, whichInfo, catalogInfo);
    make_ref(newRef, path);
    if (newSpec)
        fm_spec_of_path(path, newSpec);
}

static int8_t already_open(const char *path, gaddr inChain, gaddr refNum)
{
    bool chain = false;
    int16_t rn = 0;
    bool open = rm_path_open(path, &chain, &rn);
    if (inChain)
        wr8(inChain, chain ? 1 : 0);
    if (refNum)
        wr16(refNum, (uint16_t)(open ? rn : 0));
    set_res_err(noErr);
    return open ? 1 : 0;
}

int8_t tb_FSResourceFileAlreadyOpen(gaddr resourceFileRef, gaddr inChain, gaddr refNum)
{
    char path[PATH_MAX];
    int16_t err = ref_path(resourceFileRef, path, sizeof path);
    if (err) {
        set_res_err(err);
        return 0;
    }
    return already_open(path, inChain, refNum);
}

int8_t tb_FSpResourceFileAlreadyOpen(gaddr resourceFile, gaddr inChain, gaddr refNum)
{
    char path[PATH_MAX];
    int16_t err = resourceFile ? fm_resolve((int16_t)rd16(resourceFile + FSSpec_vRefNum_off),
                                            (int32_t)rd32(resourceFile + FSSpec_parID_off),
                                            resourceFile + FSSpec_name_off, path, sizeof path)
                               : paramErr;
    if (!err && !fm_host_exists(path))
        err = fnfErr;
    if (err) {
        set_res_err(err);
        return 0;
    }
    return already_open(path, inChain, refNum);
}

/* ---------------------------------------------------------------------- */
/* The Alias Manager on FSRefs: through FSSpecs. */

static gaddr temp_spec(void) { return tb_NewPtr(FSSpec_SIZEOF, true, true); }

/* An FSSpec for an FSRef (the caller disposes of it). */
static int16_t spec_of_ref(gaddr ref, gaddr *spec)
{
    *spec = 0;
    char path[PATH_MAX];
    int16_t err = ref_path(ref, path, sizeof path);
    if (err)
        return err;
    if (!(*spec = temp_spec()))
        return memFullErr;
    fm_spec_of_path(path, *spec);
    return noErr;
}

/* The FSRef of an FSSpec's object. */
static int16_t ref_of_spec(gaddr spec, gaddr ref)
{
    return make_fsref((int16_t)rd16(spec + FSSpec_vRefNum_off), (int32_t)rd32(spec + FSSpec_parID_off),
                      spec + FSSpec_name_off, ref);
}

int16_t tb_FSNewAlias(gaddr fromFile, gaddr target, gaddr inAlias)
{
    gaddr spec;
    int16_t err = spec_of_ref(target, &spec);
    if (!err)
        err = tb_NewAlias(0, spec, inAlias);
    if (spec)
        tb_DisposePtr(spec);
    return err;
}

int16_t tb_FSNewAliasMinimal(gaddr target, gaddr inAlias) { return tb_FSNewAlias(0, target, inAlias); }

int16_t tb_FSUpdateAlias(gaddr fromFile, gaddr target, gaddr alias, gaddr wasChanged)
{
    gaddr spec;
    int16_t err = spec_of_ref(target, &spec);
    if (!err)
        err = tb_UpdateAlias(0, spec, alias, wasChanged);
    if (spec)
        tb_DisposePtr(spec);
    return err;
}

int16_t tb_FSResolveAlias(gaddr fromFile, gaddr alias, gaddr target, gaddr wasChanged)
{
    gaddr spec = temp_spec();
    if (!spec)
        return memFullErr;
    int16_t err = tb_ResolveAlias(0, alias, spec, wasChanged);
    if (!err)
        err = ref_of_spec(spec, target);
    tb_DisposePtr(spec);
    return err;
}

int16_t tb_FSResolveAliasWithMountFlags(gaddr fromFile, gaddr inAlias, gaddr target, gaddr wasChanged,
                                        uint32_t mountFlags)
{
    return tb_FSResolveAlias(fromFile, inAlias, target, wasChanged);
}

int16_t tb_FSFollowFinderAlias(gaddr fromFile, gaddr alias, int8_t logon, gaddr target, gaddr wasChanged)
{
    return tb_FSResolveAlias(fromFile, alias, target, wasChanged);
}

/* No volume needs mounting (or asking the user about): the flags change
 * nothing. */
int16_t tb_ResolveAliasWithMountFlags(gaddr fromFile, gaddr alias, gaddr target, gaddr wasChanged, uint32_t mountFlags)
{
    return tb_ResolveAlias(fromFile, alias, target, wasChanged);
}

int16_t tb_ResolveAliasFileWithMountFlags(gaddr theSpec, int8_t resolveAliasChains, gaddr targetIsFolder,
                                          gaddr wasAliased, uint32_t mountFlags)
{
    return tb_ResolveAliasFile(theSpec, resolveAliasChains, targetIsFolder, wasAliased);
}

int16_t tb_FSResolveAliasFile(gaddr theRef, int8_t resolveAliasChains, gaddr targetIsFolder, gaddr wasAliased)
{
    gaddr spec;
    int16_t err = spec_of_ref(theRef, &spec);
    if (!err)
        err = tb_ResolveAliasFile(spec, resolveAliasChains, targetIsFolder, wasAliased);
    if (!err)
        err = ref_of_spec(spec, theRef);
    if (spec)
        tb_DisposePtr(spec);
    return err;
}

int16_t tb_FSResolveAliasFileWithMountFlags(gaddr theRef, int8_t resolveAliasChains, gaddr targetIsFolder,
                                            gaddr wasAliased, uint32_t mountFlags)
{
    return tb_FSResolveAliasFile(theRef, resolveAliasChains, targetIsFolder, wasAliased);
}

/* An alias file has the Finder's isAlias flag; a folder never is one. */
static int16_t is_alias_file(const char *path, gaddr aliasFileFlag, gaddr folderFlag)
{
    fm_info in;
    if (!fm_info_of(path, &in))
        return fnfErr;
    bool alias = !in.dir && (in.finder_info[8] & 0x80);
    if (aliasFileFlag)
        wr8(aliasFileFlag, alias ? 1 : 0);
    if (folderFlag)
        wr8(folderFlag, in.dir ? 1 : 0);
    return noErr;
}

int16_t tb_FSIsAliasFile(gaddr fileRef, gaddr aliasFileFlag, gaddr folderFlag)
{
    char path[PATH_MAX];
    int16_t err = ref_path(fileRef, path, sizeof path);
    return err ? err : is_alias_file(path, aliasFileFlag, folderFlag);
}

int16_t tb_IsAliasFile(gaddr fileFSSpec, gaddr aliasFileFlag, gaddr folderFlag)
{
    if (!fileFSSpec)
        return paramErr;
    char path[PATH_MAX];
    int16_t err = fm_resolve((int16_t)rd16(fileFSSpec + FSSpec_vRefNum_off), (int32_t)rd32(fileFSSpec + FSSpec_parID_off),
                             fileFSSpec + FSSpec_name_off, path, sizeof path);
    return err ? err : is_alias_file(path, aliasFileFlag, folderFlag);
}

/* ---------------------------------------------------------------------- */
/* The Folder Manager */

int16_t tb_FSFindFolder(int16_t vRefNum, uint32_t folderType, int8_t createFolder, gaddr foundRef)
{
    char path[PATH_MAX];
    int16_t err = fm_find_folder(folderType, createFolder, path, sizeof path);
    if (!err)
        make_ref(foundRef, path);
    return err;
}

/* The flags (no aliases followed, no user's folders) and their data
 * change nothing: there is one user, and no aliases on the way. */
int16_t tb_FSFindFolderExtended(int16_t vol, uint32_t foldType, int8_t createFolder, uint32_t flags, gaddr data,
                                gaddr foundRef)
{
    return tb_FSFindFolder(vol, foldType, createFolder, foundRef);
}

int16_t tb_FindFolderExtended(int16_t vol, uint32_t foldType, int8_t createFolder, uint32_t flags, gaddr data,
                              gaddr vRefNum, gaddr dirID)
{
    return tb_FindFolder(vol, (int32_t)foldType, createFolder, vRefNum, dirID);
}

/* Folders aren't counted in and out of use: there is nothing to release
 * but the folder must be one FindFolder knows. */
int16_t tb_ReleaseFolder(int16_t vRefNum, uint32_t folderType)
{
    char path[PATH_MAX];
    return fm_find_folder(folderType, false, path, sizeof path);
}

#define MAX_NOTIFY 16
static struct {
    gaddr proc, refcon;
    uint32_t options;
} notify[MAX_NOTIFY];
static int nnotify;

int16_t tb_FolderManagerRegisterNotificationProc(gaddr notificationProc, gaddr refCon, uint32_t options)
{
    if (!notificationProc)
        return paramErr;
    if (nnotify >= MAX_NOTIFY)
        return memFullErr;
    notify[nnotify].proc = notificationProc;
    notify[nnotify].refcon = refCon;
    notify[nnotify].options = options;
    nnotify++;
    return noErr;
}

int16_t tb_FolderManagerUnregisterNotificationProc(gaddr notificationProc, gaddr refCon)
{
    for (int i = 0; i < nnotify; i++)
        if (notify[i].proc == notificationProc && notify[i].refcon == refCon) {
            memmove(&notify[i], &notify[i + 1], (size_t)(nnotify - i - 1) * sizeof notify[0]);
            nnotify--;
            return noErr;
        }
    return paramErr;
}

/* Each registered procedure is told (pascal OSStatus (message, arg,
 * refCon)); kStopIfAnyNotificationProcReturnsErrorBit (31) stops at the
 * first that answers an error, and that is the answer. */
int32_t tb_FolderManagerRegisterCallNotificationProcs(uint32_t message, gaddr arg, uint32_t options)
{
    enum { uppFolderManagerNotificationProcInfo = 0x0FF0 };
    int n = nnotify;
    gaddr procs[MAX_NOTIFY], refcons[MAX_NOTIFY];
    for (int i = 0; i < n; i++) {
        procs[i] = notify[i].proc;
        refcons[i] = notify[i].refcon;
    }
    for (int i = 0; i < n; i++) {
        uint32_t args[3] = { message, arg, refcons[i] };
        int32_t r = (int32_t)mm_call(procs[i], uppFolderManagerNotificationProcInfo, args, 3);
        if (r && (options & 0x80000000u))
            return r;
    }
    return noErr;
}

/* ---------------------------------------------------------------------- */

void hfsplus_reset(void)
{
    for (int i = 0; i < MAX_ITERATORS; i++)
        if (iters[i].used)
            close_iter(i);
    nnotify = 0;
    nfsds = 0;
}

/* ---------------------------------------------------------------------- */
/* PowerPC exports whose arguments the generated glue can't take: a 64-bit
 * value in two registers (high word first), or a 68k trap whose flag bit
 * InterfaceLib gave a name of its own (the Device Manager's immediate
 * calls, the same traps as the HFS ones). */

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v)                                                                                                     \
    do {                                                                                                              \
        c->gpr[3] = (uint32_t)(int32_t)(v);                                                                           \
        c->pc = c->lr;                                                                                                \
    } while (0)

static int64_t r64(ppc_cpu *c, int n) { return (int64_t)((uint64_t)R(n) << 32 | R(n + 1)); }

/* FSReadFork(forkRefNum, positionMode, SInt64 positionOffset, requestCount, buffer, *actualCount) */
static void g_FSReadFork(ppc_cpu *c) { RETURN(fork_rw((int16_t)R(0), false, (uint16_t)R(1), r64(c, 2), R(4), R(5), R(6))); }
static void g_FSWriteFork(ppc_cpu *c) { RETURN(fork_rw((int16_t)R(0), true, (uint16_t)R(1), r64(c, 2), R(4), R(5), R(6))); }
/* FSSetForkPosition / FSSetForkSize(forkRefNum, positionMode, SInt64 positionOffset) */
static void g_FSSetForkPosition(ppc_cpu *c) { RETURN(set_fork_position((int16_t)R(0), (uint16_t)R(1), r64(c, 2))); }
static void g_FSSetForkSize(ppc_cpu *c) { RETURN(set_fork_size((int16_t)R(0), (uint16_t)R(1), r64(c, 2))); }
/* FSAllocateFork(forkRefNum, flags, positionMode, SInt64 positionOffset, UInt64 requestCount, UInt64 *actualCount) */
static void g_FSAllocateFork(ppc_cpu *c) { RETURN(allocate_fork((int16_t)R(0), (uint16_t)R(1), (uint64_t)r64(c, 5), R(7))); }

/* PBxxxImmed(pb): the trap with bit $0200, synchronous. */
static void g_PBOpenImmed(ppc_cpu *c) { RETURN(pb_call_done(R(0), tb_PBHOpen(R(0), false), false)); }
static void g_PBCloseImmed(ppc_cpu *c) { RETURN(pb_call_done(R(0), tb_PBClose(R(0), false), false)); }
static void g_PBReadImmed(ppc_cpu *c) { RETURN(pb_call_done(R(0), tb_PBRead(R(0), false), false)); }
static void g_PBWriteImmed(ppc_cpu *c) { RETURN(pb_call_done(R(0), tb_PBWrite(R(0), false), false)); }
static void g_PBStatusImmed(ppc_cpu *c) { RETURN(pb_call_done(R(0), tb_PBStatus(R(0), false), false)); }
static void g_PBKillIOImmed(ppc_cpu *c) { RETURN(pb_call_done(R(0), tb_PBKillIO(R(0), false), false)); }
static void g_PBUnmountVolImmed(ppc_cpu *c) { RETURN(pb_call_done(R(0), tb_PBUnmountVol(R(0)), false)); }

const ppc_export hfs_exports[] = {
    { "FSAllocateFork", g_FSAllocateFork },
    { "FSReadFork", g_FSReadFork },
    { "FSSetForkPosition", g_FSSetForkPosition },
    { "FSSetForkSize", g_FSSetForkSize },
    { "FSWriteFork", g_FSWriteFork },
    { "PBCloseImmed", g_PBCloseImmed },
    { "PBKillIOImmed", g_PBKillIOImmed },
    { "PBOpenImmed", g_PBOpenImmed },
    { "PBReadImmed", g_PBReadImmed },
    { "PBStatusImmed", g_PBStatusImmed },
    { "PBUnmountVolImmed", g_PBUnmountVolImmed },
    { "PBWriteImmed", g_PBWriteImmed },
};
const unsigned hfs_export_count = sizeof hfs_exports / sizeof hfs_exports[0];

/* ---------------------------------------------------------------------- */
/* Resource files by fork name (CarbonLib 1.4: Resources.h) */

int16_t rm_open_data_fork(const char *path); /* rsrcmgr.c */

/* FSOpenResourceFile(ref, forkNameLength, forkName, permissions, refNum):
 * the resource fork (named so), or a data fork read as a resource file;
 * an error is returned, and ResError set. */
int16_t tb_FSOpenResourceFile(gaddr ref, uint32_t forkNameLength, gaddr forkName, int8_t permissions, gaddr refNum)
{
    char path[PATH_MAX];
    if (refNum)
        wr16(refNum, 0);
    int16_t err = ref_file(ref, path, sizeof path);
    int kind = err ? 0 : fork_kind(forkNameLength, forkName);
    if (!err && kind < 0)
        err = errFSBadForkName;
    int16_t rn = -1;
    if (!err && kind == 1) {
        rn = rm_open_path(path, permissions);
        if (rn < 0)
            err = (int16_t)rd16(LM_ResErr) ? (int16_t)rd16(LM_ResErr) : -39;
    } else if (!err) {
        if (permissions & 2) /* a data fork is read here */
            err = wrPermErr;
        else {
            rn = rm_open_data_fork(path);
            if (rn < 0)
                err = rn;
        }
    }
    set_res_err(err);
    if (!err && refNum)
        wr16(refNum, (uint16_t)rn);
    return err;
}

/* FSCreateResourceFile(parentRef, nameLength, name, whichInfo, catalogInfo,
 * forkNameLength, forkName, newRef, newSpec): a new file with an empty
 * resource fork (a data fork can't hold one here: it is read-only). */
int16_t tb_FSCreateResourceFile(gaddr parentRef, uint32_t nameLength, gaddr name, uint32_t whichInfo, gaddr catalogInfo,
                                uint32_t forkNameLength, gaddr forkName, gaddr newRef, gaddr newSpec)
{
    int kind = fork_kind(forkNameLength, forkName);
    if (kind < 0)
        return errFSBadForkName;
    if (kind == 0)
        return -4; /* unimpErr */
    tb_FSCreateResFile(parentRef, nameLength, name, whichInfo, catalogInfo, newRef, newSpec);
    return (int16_t)rd16(LM_ResErr);
}
