/*
 * The File System Manager's HFS utilities (FSM.h): the calls a file system
 * plug-in makes into the File Manager for its file control blocks, volume
 * control blocks, working directories, drives, path names and cache.  They
 * work on the File Manager's own tables: the FCB table (FCBSPtr), the
 * volume queue (VCBQHdr) and the drive queue (DrvQHdr) in guest memory,
 * and its working directories.
 *
 * A plug-in's file system has no place here to keep volumes of its own: a
 * VCB it adds is in the queue, found by name, drive and reference number
 * and taken out again, but there is no host folder behind it for the File
 * Manager's other calls to read.  Nor is there a disk to hold blocks: the
 * cache's blocks are those of a file's fork, and a request for the blocks
 * of a volume comes back volOffLinErr, as the volume of an unmounted disk.
 */
#include <string.h>

#include "filemgr_internal.h"
#include "tb_internal.h"

enum {
    rfNumErr = -51, nsvErr = -35, nsDrvErr = -56, bdNamErr = -37, volOffLinErr = -53, volOnLinErr = -55,
    fBsyErr = -47, opWrErr = -49, wPrErr = -44, permErr = -54, tmfoErr = -42, wrPermErr = -61,
};
enum { FCB_LEN = 94, FCB_ID = 0, FCB_FLAGS = 4, FCB_EOF = 8, FCB_PLEN = 12, FCB_CRPS = 16, FCB_VPTR = 20,
       FCB_DIRID = 58, FCB_NAME = 62 };
enum { fsCurPerm = 0, fsRdPerm = 1, fsWrPerm = 2, fsRdWrPerm = 3, fsRdWrShPerm = 4 };
enum { fcbWriteMask = 0x01, fcbResourceMask = 0x02, fcbSharedWriteMask = 0x10, fcbFileLockedMask = 0x20 };
enum { dtmvError = 0, dtmvFullPathname, dtmvVRefNum, dtmvWDRefNum, dtmvDriveNum, dtmvDefault };
enum { gbReadMask = 1, gbExistMask = 2, gbNoReadMask = 4, gbReleaseMask = 8 };
enum { rbWriteMask = 1, rbTrashMask = 2, rbDirtyMask = 4, rbFreeMask = 0x0A };
enum { fcTrashMask = 1 };
enum { chNoBuf = 1, chInUse = 2, chnotfound = 3, chNotInUse = 4 };
enum { BLOCK = 512, MAX_BLOCKS = 32 };

/* ---- Names ---- */

static uint8_t fold(uint8_t c) { return c >= 'A' && c <= 'Z' ? (uint8_t)(c + 32) : c; }

/* Two Pascal strings the same ignoring case. */
static bool pstr_same(gaddr a, gaddr b)
{
    if (rd8(a) != rd8(b))
        return false;
    for (uint32_t i = 1; i <= rd8(a); i++)
        if (fold(rd8(a + i)) != fold(rd8(b + i)))
            return false;
    return true;
}

/* ---- File control blocks ---- */

static int slot_of(int16_t refnum) { return (refnum - 2) / FCB_LEN; }

int16_t tb_UTAllocateFCB(gaddr fileRefNum, gaddr fileCtrlBlockPtr)
{
    int16_t rn;
    int16_t err = fm_fcb_claim(&rn);
    if (err)
        return err;
    if (fileRefNum)
        wr16(fileRefNum, (uint16_t)rn);
    if (fileCtrlBlockPtr)
        wr32(fileCtrlBlockPtr, fm_fcb_addr(rn));
    return noErr;
}

int16_t tb_UTReleaseFCB(int16_t fileRefNum) { return fm_fcb_release(fileRefNum); }

int16_t tb_UTResolveFCB(int16_t fileRefNum, gaddr fileCtrlBlockPtr)
{
    gaddr f = fm_fcb_addr(fileRefNum);
    if (!f || !fm_fcb_in_use(fileRefNum))
        return rfNumErr;
    if (fileCtrlBlockPtr)
        wr32(fileCtrlBlockPtr, f);
    return noErr;
}

/* The next FCB from a slot on that is the file asked for (by number, and
 * name if one is given) on the volume (any if none). */
static int16_t locate_from(gaddr vcb, uint32_t fileNum, gaddr name, int from, gaddr refp, gaddr fcbp)
{
    for (int i = from; i < fm_fcb_slots(); i++) {
        int16_t rn = (int16_t)(2 + i * FCB_LEN);
        gaddr f = fm_fcb_addr(rn);
        if (!f || !rd32(f + FCB_ID) || rd32(f + FCB_ID) != fileNum)
            continue;
        if (vcb && rd32(f + FCB_VPTR) != vcb)
            continue;
        if (name && !pstr_same(name, f + FCB_NAME))
            continue;
        if (refp)
            wr16(refp, (uint16_t)rn);
        if (fcbp)
            wr32(fcbp, f);
        return noErr;
    }
    return fnfErr;
}

int16_t tb_UTLocateFCB(gaddr vcb, uint32_t fileNum, gaddr namePtr, gaddr fileRefNum, gaddr fileCtrlBlockPtr)
{
    return locate_from(vcb, fileNum, namePtr, 0, fileRefNum, fileCtrlBlockPtr);
}

/* UTLocateNextFCB: from the one after the refnum given. */
int16_t tb_UTLocateNextFCB(gaddr vcb, uint32_t fileNum, gaddr namePtr, gaddr fileRefNum, gaddr fileCtrlBlockPtr)
{
    int16_t after = fileRefNum ? (int16_t)rd16(fileRefNum) : 0;
    if (after && !fm_fcb_addr(after))
        return rfNumErr;
    return locate_from(vcb, fileNum, namePtr, after ? slot_of(after) + 1 : 0, fileRefNum, fileCtrlBlockPtr);
}

/* UTIndexFCB: the next open file of the volume after the refnum (0 for
 * the first); fnfErr when there are no more. */
int16_t tb_UTIndexFCB(gaddr vcb, gaddr fileRefNum, gaddr fileCtrlBlockPtr)
{
    int16_t after = fileRefNum ? (int16_t)rd16(fileRefNum) : 0;
    if (after && !fm_fcb_addr(after))
        return rfNumErr;
    for (int i = after ? slot_of(after) + 1 : 0; i < fm_fcb_slots(); i++) {
        int16_t rn = (int16_t)(2 + i * FCB_LEN);
        gaddr f = fm_fcb_addr(rn);
        if (!f || !rd32(f + FCB_ID) || (vcb && rd32(f + FCB_VPTR) != vcb))
            continue;
        if (fileRefNum)
            wr16(fileRefNum, (uint16_t)rn);
        if (fileCtrlBlockPtr)
            wr32(fileCtrlBlockPtr, f);
        return noErr;
    }
    return fnfErr;
}

int16_t tb_UTResolveFileRefNum(gaddr fileCtrlBlockPtr, gaddr fileRefNum)
{
    int16_t rn = fm_fcb_refnum_at(fileCtrlBlockPtr);
    if (!rn)
        return rfNumErr;
    if (fileRefNum)
        wr16(fileRefNum, (uint16_t)rn);
    return noErr;
}

int16_t tb_UTCheckFCB(gaddr fileCtrlBlockPtr)
{
    int16_t rn = fm_fcb_refnum_at(fileCtrlBlockPtr);
    return rn && fm_fcb_in_use(rn) ? noErr : rfNumErr;
}

int16_t tb_UTCheckFileRefNum(int16_t fileRefNum) { return fm_fcb_addr(fileRefNum) && fm_fcb_in_use(fileRefNum) ? noErr : rfNumErr; }

/* The record's size, as FSFCBLen says. */
int16_t tb_UTGetForkControlBlockSize(gaddr fcbSize)
{
    if (!fcbSize)
        return paramErr;
    wr32(fcbSize, rd16(LM_FSFCBLen));
    return noErr;
}

/* UTAdjustEOF: a logical end beyond the physical end is brought back to it. */
int16_t tb_UTAdjustEOF(int16_t fileRefNum)
{
    gaddr f = fm_fcb_addr(fileRefNum);
    if (!f || !fm_fcb_in_use(fileRefNum))
        return rfNumErr;
    if (rd32(f + FCB_EOF) > rd32(f + FCB_PLEN))
        wr32(f + FCB_EOF, rd32(f + FCB_PLEN));
    return noErr;
}

/* The FCBs in the search list: the File Manager's own open files are
 * always in it; a plug-in's are once it adds them. */
static uint8_t in_search[512];

int16_t tb_UTAddFCBToSearchList(gaddr fileCtrlBlockPtr)
{
    int16_t rn = fm_fcb_refnum_at(fileCtrlBlockPtr);
    if (!rn || !fm_fcb_in_use(rn))
        return rfNumErr;
    in_search[slot_of(rn) % sizeof in_search] = 1;
    return noErr;
}

int16_t tb_UTRemoveFCBFromSearchList(gaddr fileCtrlBlockPtr)
{
    int16_t rn = fm_fcb_refnum_at(fileCtrlBlockPtr);
    if (!rn || !fm_fcb_in_use(rn))
        return rfNumErr;
    in_search[slot_of(rn) % sizeof in_search] = 0;
    return noErr;
}

/* Whether an FCB is one the search list holds: a plug-in's slot when added,
 * every other. */
static bool searchable(int16_t rn)
{
    return fm_fcb_in_use(rn) && (in_search[slot_of(rn) % sizeof in_search] || !fm_fcb_is_claimed(rn));
}

int16_t tb_UTLocateFCBInSearchList(gaddr vcb, uint32_t fileNum, gaddr fileRefNum, gaddr fileCtrlBlockPtr)
{
    for (int i = 0; i < fm_fcb_slots(); i++) {
        int16_t rn = (int16_t)(2 + i * FCB_LEN);
        gaddr f = fm_fcb_addr(rn);
        if (!f || !fileNum || rd32(f + FCB_ID) != fileNum || (vcb && rd32(f + FCB_VPTR) != vcb) || !searchable(rn))
            continue;
        if (fileRefNum)
            wr16(fileRefNum, (uint16_t)rn);
        if (fileCtrlBlockPtr)
            wr32(fileCtrlBlockPtr, f);
        return noErr;
    }
    return fnfErr;
}

/* ---- Permissions ---- */

/* How the other open forks of a file stand in the way of a permission:
 * whether any is open for writing, and whether all are shared. */
static void other_opens(gaddr vcb, uint32_t fileNum, bool resource, bool search, int16_t *same, bool *writer,
                        bool *unshared)
{
    *writer = *unshared = false;
    if (same)
        *same = 0;
    for (int i = 0; i < fm_fcb_slots(); i++) {
        int16_t rn = (int16_t)(2 + i * FCB_LEN);
        gaddr f = fm_fcb_addr(rn);
        if (!f || !rd32(f + FCB_ID) || rd32(f + FCB_ID) != fileNum || (vcb && rd32(f + FCB_VPTR) != vcb))
            continue;
        if (search && !searchable(rn))
            continue;
        if (!!(rd8(f + FCB_FLAGS) & fcbResourceMask) != resource)
            continue;
        if (same && !*same)
            *same = rn;
        if (rd8(f + FCB_FLAGS) & fcbWriteMask) {
            *writer = true;
            if (!(rd8(f + FCB_FLAGS) & fcbSharedWriteMask))
                *unshared = true;
        }
    }
}

/* The permission a request comes to, or the error: writing a locked file
 * is permErr, writing what another writes opWrErr; the current permission
 * is read and write where it can be, else read only. */
static int16_t resolve_permission(int8_t asked, bool locked, bool writer, bool unshared, int8_t *granted)
{
    int8_t p = asked;
    if (p == fsCurPerm)
        p = !locked && !writer ? fsRdWrPerm : fsRdPerm;
    if (p == fsWrPerm || p == fsRdWrPerm || p == fsRdWrShPerm) {
        if (locked)
            return permErr;
        if (p == fsRdWrShPerm ? unshared : writer)
            return opWrErr;
    }
    *granted = p;
    return noErr;
}

/* UTCheckPermission(vcb, *modByte, fileNum, paramBlock): the permission
 * asked in *modByte becomes the one granted. */
int16_t tb_UTCheckPermission(gaddr vcb, gaddr modByte, uint32_t fileNum, gaddr paramBlock)
{
    if (!modByte)
        return paramErr;
    int16_t asked = (int16_t)rd16(modByte);
    if (asked < fsCurPerm || asked > fsRdWrShPerm)
        return paramErr;
    bool writer, unshared;
    other_opens(vcb, fileNum, false, false, NULL, &writer, &unshared);
    bool locked = vcb && (rd16(vcb + VCB_vcbAtrb_off) & 0x8080);
    if (locked && asked >= fsWrPerm)
        return wPrErr;
    int8_t granted;
    int16_t err = resolve_permission((int8_t)asked, false, writer, unshared, &granted);
    if (!err)
        wr16(modByte, (uint16_t)granted);
    return err;
}

/* UTCheckForkPermissions(vcb, fileNum, forkID, fileLocked, permissions,
 * useSearchList, *fcbFlags, *openForkRefNum): the FCB flags a new open of
 * the fork would have, and the refnum of an open of it already. */
int16_t tb_UTCheckForkPermissions(gaddr vcb, uint32_t fileNum, uint32_t forkID, int8_t fileLocked, int8_t permissions,
                                  int8_t useSearchList, gaddr fcbFlags, gaddr openForkRefNum)
{
    if (permissions < fsCurPerm || permissions > fsRdWrShPerm)
        return paramErr;
    bool writer, unshared;
    int16_t same;
    bool resource = forkID == 0xFF;
    other_opens(vcb, fileNum, resource, useSearchList != 0, &same, &writer, &unshared);
    int8_t granted;
    int16_t err = resolve_permission(permissions, fileLocked != 0, writer, unshared, &granted);
    if (err)
        return err;
    uint8_t flags = 0;
    if (granted == fsWrPerm || granted == fsRdWrPerm || granted == fsRdWrShPerm)
        flags |= fcbWriteMask;
    if (granted == fsRdWrShPerm)
        flags |= fcbSharedWriteMask;
    if (fileLocked)
        flags |= fcbFileLockedMask;
    if (resource)
        flags |= fcbResourceMask;
    if (fcbFlags)
        wr8(fcbFlags, flags);
    if (openForkRefNum)
        wr16(openForkRefNum, (uint16_t)same);
    return noErr;
}

/* ---- Volumes ---- */

static gaddr vcb_first(void) { return rd32(LM_VCBQHdr + 2); }
static gaddr vcb_next(gaddr v) { return rd32(v); }

static bool vcb_listed(gaddr v)
{
    for (gaddr q = vcb_first(); q; q = vcb_next(q))
        if (q == v)
            return true;
    return false;
}

int16_t tb_UTAllocateVCB(gaddr sysVCBLength, gaddr volCtrlBlockPtr, uint16_t addSize)
{
    if (!volCtrlBlockPtr)
        return paramErr;
    gaddr v = tb_NewPtr((int32_t)(VCB_SIZEOF + addSize), true, true);
    if (!v)
        return memFullErr;
    if (sysVCBLength)
        wr16(sysVCBLength, VCB_SIZEOF);
    wr32(volCtrlBlockPtr, v);
    return noErr;
}

/* A vRefNum no volume has: the volumes count down from -1. */
static int16_t new_vref(void)
{
    for (int16_t r = -2; r > -32000; r--) {
        bool taken = false;
        for (gaddr q = vcb_first(); q && !taken; q = vcb_next(q))
            taken = (int16_t)rd16(q + VCB_vcbVRefNum_off) == r;
        if (!taken)
            return r;
    }
    return 0;
}

int16_t tb_UTAddNewVCB(int16_t driveNum, gaddr vRefNum, gaddr volCtrlBlockPtr)
{
    if (driveNum <= 0 || !volCtrlBlockPtr || !vRefNum)
        return paramErr;
    /* The same volume already mounted: the same name and date. */
    for (gaddr q = vcb_first(); q; q = vcb_next(q))
        if (q != volCtrlBlockPtr && pstr_same(q + VCB_vcbVN_off, volCtrlBlockPtr + VCB_vcbVN_off) &&
            rd32(q + VCB_vcbCrDate_off) == rd32(volCtrlBlockPtr + VCB_vcbCrDate_off))
            return volOnLinErr;
    if (vcb_listed(volCtrlBlockPtr))
        return volOnLinErr;
    int16_t r = new_vref();
    if (!r)
        return tmfoErr;
    wr16(volCtrlBlockPtr + VCB_vcbDrvNum_off, (uint16_t)driveNum);
    wr16(volCtrlBlockPtr + VCB_vcbVRefNum_off, (uint16_t)r);
    wr32(volCtrlBlockPtr, 0);
    gaddr tail = rd32(LM_VCBQHdr + 6);
    if (tail)
        wr32(tail, volCtrlBlockPtr);
    else
        wr32(LM_VCBQHdr + 2, volCtrlBlockPtr);
    wr32(LM_VCBQHdr + 6, volCtrlBlockPtr);
    wr16(vRefNum, (uint16_t)r);
    return noErr;
}

/* UTDisposeVCB: out of the queue, unless it is the startup volume's or has
 * files open on it. */
int16_t tb_UTDisposeVCB(gaddr volCtrlBlockPtr)
{
    gaddr prev = 0;
    gaddr q = vcb_first();
    while (q && q != volCtrlBlockPtr) {
        prev = q;
        q = vcb_next(q);
    }
    if (!q)
        return nsvErr;
    if (q == vcb_first() || q == rd32(LM_DefVCBPtr))
        return volOnLinErr;
    for (int i = 0; i < fm_fcb_slots(); i++) {
        gaddr f = fm_fcb_addr((int16_t)(2 + i * FCB_LEN));
        if (f && rd32(f + FCB_ID) && rd32(f + FCB_VPTR) == q)
            return fBsyErr;
    }
    wr32(prev, vcb_next(q));
    if (rd32(LM_VCBQHdr + 6) == q)
        wr32(LM_VCBQHdr + 6, prev);
    tb_DisposePtr(q);
    return noErr;
}

/* The VCB a reference number leads to: a volume's, a working directory's,
 * a drive's (a positive number) or the default volume's (0). */
static gaddr vcb_of_ref(int16_t ref)
{
    if (ref == 0)
        return rd32(LM_DefVCBPtr);
    if (ref > 0) {
        for (gaddr q = vcb_first(); q; q = vcb_next(q))
            if ((int16_t)rd16(q + VCB_vcbDrvNum_off) == ref)
                return q;
        return 0;
    }
    int16_t vref = ref;
    int32_t dir;
    if (fm_wd_index(ref) >= 0 && !fm_wd_resolve(ref, &vref, &dir))
        return 0;
    for (gaddr q = vcb_first(); q; q = vcb_next(q))
        if ((int16_t)rd16(q + VCB_vcbVRefNum_off) == vref)
            return q;
    return 0;
}

int16_t tb_UTLocateVCBByRefNum(int16_t refNum, gaddr vRefNum, gaddr volCtrlBlockPtr)
{
    gaddr v = vcb_of_ref(refNum);
    if (!v)
        return nsvErr;
    if (vRefNum)
        wr16(vRefNum, rd16(v + VCB_vcbVRefNum_off));
    if (volCtrlBlockPtr)
        wr32(volCtrlBlockPtr, v);
    return noErr;
}

/* The volumes of a name from one on, and how many more there are after
 * the one found. */
static int16_t find_by_name(gaddr name, gaddr after, gaddr more, gaddr vRefNum, gaddr volCtrlBlockPtr)
{
    gaddr found = 0;
    int extra = 0;
    bool past = after == 0;
    for (gaddr q = vcb_first(); q; q = vcb_next(q)) {
        if (q == after)
            past = true;
        else if (past && pstr_same(name, q + VCB_vcbVN_off)) {
            if (!found)
                found = q;
            else
                extra++;
        }
    }
    if (!found)
        return nsvErr;
    if (more)
        wr16(more, (uint16_t)extra);
    if (vRefNum)
        wr16(vRefNum, rd16(found + VCB_vcbVRefNum_off));
    if (volCtrlBlockPtr)
        wr32(volCtrlBlockPtr, found);
    return noErr;
}

int16_t tb_UTLocateVCBByName(gaddr namePtr, gaddr moreMatches, gaddr vRefNum, gaddr volCtrlBlockPtr)
{
    if (!namePtr || !rd8(namePtr) || rd8(namePtr) > 27)
        return bdNamErr;
    return find_by_name(namePtr, 0, moreMatches, vRefNum, volCtrlBlockPtr);
}

/* UTLocateNextVCB: the next of the same name after the VCB in *volCtrlBlockPtr. */
int16_t tb_UTLocateNextVCB(gaddr namePtr, gaddr moreMatches, gaddr vRefNum, gaddr volCtrlBlockPtr)
{
    if (!namePtr || !rd8(namePtr) || rd8(namePtr) > 27 || !volCtrlBlockPtr)
        return bdNamErr;
    return find_by_name(namePtr, rd32(volCtrlBlockPtr), moreMatches, vRefNum, volCtrlBlockPtr);
}

/* The checks of a volume's or file's state, each with the error a call
 * on it would meet. */
int16_t tb_UTCheckVolRefNum(int16_t vRefNum) { return vcb_of_ref(vRefNum) ? noErr : nsvErr; }
int16_t tb_UTCheckVolOffline(int16_t vRefNum) { return vcb_of_ref(vRefNum) ? noErr : nsvErr; }

int16_t tb_UTCheckVolModifiable(int16_t vRefNum)
{
    gaddr v = vcb_of_ref(vRefNum);
    if (!v)
        return nsvErr;
    return rd16(v + VCB_vcbAtrb_off) & 0x8080 ? wPrErr : noErr;
}

int16_t tb_UTCheckFileModifiable(int16_t fileRefNum)
{
    gaddr f = fm_fcb_addr(fileRefNum);
    if (!f || !fm_fcb_in_use(fileRefNum))
        return rfNumErr;
    return rd8(f + FCB_FLAGS) & fcbWriteMask ? noErr : wrPermErr;
}

/* Drives: the queue element of a drive number. */
int16_t tb_UTFindDrive(int16_t driveNum, gaddr driveQElementPtr)
{
    for (gaddr e = rd32(LM_DrvQHdr + 2); e; e = rd32(e + DrvQEl_qLink_off))
        if ((int16_t)rd16(e + DrvQEl_dQDrive_off) == driveNum) {
            if (driveQElementPtr)
                wr32(driveQElementPtr, e);
            return noErr;
        }
    return nsDrvErr;
}

/* The drives are fixed disks, which do not eject (DiskEject says so). */
int16_t tb_UTEjectVol(gaddr volCtrlBlockPtr)
{
    if (!vcb_listed(volCtrlBlockPtr))
        return nsvErr;
    return tb_DiskEject((int16_t)rd16(volCtrlBlockPtr + VCB_vcbDrvNum_off));
}

/* ---- Working directories ---- */

int16_t tb_UTCheckWDRefNum(int16_t wdRefNum) { return fm_wd_index(wdRefNum) >= 0 || vcb_of_ref(wdRefNum) ? noErr : nsvErr; }

int16_t tb_UTAllocateWDCB(gaddr paramBlock)
{
    if (!paramBlock)
        return paramErr;
    return tb_PBOpenWD(paramBlock, false);
}

int16_t tb_UTReleaseWDCB(int16_t wdRefNum)
{
    if (fm_wd_index(wdRefNum) < 0)
        return nsvErr;
    gaddr pb = tb_NewPtr(IOParam_SIZEOF > WDPBRec_SIZEOF ? IOParam_SIZEOF : WDPBRec_SIZEOF, true, true);
    if (!pb)
        return memFullErr;
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)wdRefNum);
    int16_t err = tb_PBCloseWD(pb, false);
    tb_DisposePtr(pb);
    return err;
}

/* A working directory's control block (WDCBRec: its VCB, directory, hint
 * and process), made when asked for. */
static gaddr wdcbs;

int16_t tb_UTResolveWDCB(int32_t procID, int16_t wdIndex, int16_t wdRefNum, gaddr wdCtrlBlockPtr)
{
    int16_t ref = wdRefNum;
    if (wdIndex > 0) { /* the nth one in use */
        int n = 0;
        ref = 0;
        for (int i = 0; i < 64 && !ref; i++)
            if (fm_wd_index((int16_t)(-32000 - i)) >= 0 && ++n == wdIndex)
                ref = (int16_t)(-32000 - i);
        if (!ref)
            return nsvErr;
    }
    int16_t vref;
    int32_t dir;
    if (!ref || !fm_wd_resolve(ref, &vref, &dir))
        return nsvErr;
    gaddr v = vcb_of_ref(vref);
    if (!v)
        return nsvErr;
    if (!wdcbs)
        wdcbs = tb_NewPtr(16, true, true);
    if (!wdcbs)
        return memFullErr;
    wr32(wdcbs, v);
    wr32(wdcbs + 4, (uint32_t)dir);
    wr32(wdcbs + 8, 0);
    wr32(wdcbs + 12, (uint32_t)procID);
    if (wdCtrlBlockPtr)
        wr32(wdCtrlBlockPtr, wdcbs);
    return noErr;
}

/* The default volume and directory. */
int16_t tb_UTSetDefaultVol(int32_t nodeHint, uint32_t dirID, int16_t refNum)
{
    gaddr pb = tb_NewPtr(WDPBRec_SIZEOF > IOParam_SIZEOF ? WDPBRec_SIZEOF : IOParam_SIZEOF, true, true);
    if (!pb)
        return memFullErr;
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)refNum);
    wr32(pb + WDPBRec_ioWDDirID_off, dirID);
    int16_t err = tb_PBHSetVol(pb, false);
    tb_DisposePtr(pb);
    return err;
}

int16_t tb_UTGetDefaultVol(gaddr paramBlock)
{
    if (!paramBlock)
        return paramErr;
    return tb_PBHGetVol(paramBlock, false);
}

/* UTCheckDirBusy: a directory with a file open in it, or a working
 * directory on it, is busy. */
int16_t tb_UTCheckDirBusy(gaddr volCtrlBlockPtr, uint32_t dirID)
{
    for (int i = 0; i < fm_fcb_slots(); i++) {
        gaddr f = fm_fcb_addr((int16_t)(2 + i * FCB_LEN));
        if (f && rd32(f + FCB_ID) && rd32(f + FCB_DIRID) == dirID &&
            (!volCtrlBlockPtr || rd32(f + FCB_VPTR) == volCtrlBlockPtr))
            return fBsyErr;
    }
    for (int i = 0; i < 64; i++) {
        int16_t vref;
        int32_t dir;
        if (fm_wd_index((int16_t)(-32000 - i)) >= 0 && fm_wd_resolve((int16_t)(-32000 - i), &vref, &dir) &&
            (uint32_t)dir == dirID)
            return fBsyErr;
    }
    return noErr;
}

/* ---- Path names ---- */

/* UTParsePathname(*volNamelength, name): how long the volume name is that
 * a full path name starts with ("Disk:folder:file" is 4), 0 for a partial
 * path or a bare name. */
int16_t tb_UTParsePathname(gaddr volNamelength, gaddr namePtr)
{
    if (!volNamelength || !namePtr)
        return paramErr;
    uint32_t n = rd8(namePtr), colon = 0;
    while (colon < n && rd8(namePtr + 1 + colon) != ':')
        colon++;
    uint32_t len = colon < n ? colon : 0; /* no colon: not a path to a volume */
    if (len > 27)
        return bdNamErr;
    wr16(volNamelength, (uint16_t)len);
    return noErr;
}

/* UTGetPathComponentName(parseRec): the component of the path at
 * startOffset (from the first character), up to a colon. */
int16_t tb_UTGetPathComponentName(gaddr parseRec)
{
    if (!parseRec || !rd32(parseRec))
        return paramErr;
    gaddr name = rd32(parseRec);
    int16_t start = (int16_t)rd16(parseRec + 4);
    uint32_t n = rd8(name);
    if (start < 0 || (uint32_t)start > n)
        return paramErr;
    uint32_t end = (uint32_t)start;
    while (end < n && rd8(name + 1 + end) != ':')
        end++;
    wr16(parseRec + 6, (uint16_t)(end - (uint32_t)start));
    wr8(parseRec + 9, end < n);                 /* stopped at a colon */
    wr8(parseRec + 8, end < n && end + 1 < n);  /* and there is more after it */
    return noErr;
}

/* UTDetermineVol(pb, *status, *moreMatches, *vRefNum, *vcb): the volume
 * a parameter block's name and volume reference choose, and how. */
int16_t tb_UTDetermineVol(gaddr paramBlock, gaddr status, gaddr moreMatches, gaddr vRefNum, gaddr volCtrlBlockPtr)
{
    if (!paramBlock)
        return paramErr;
    gaddr name = rd32(paramBlock + IOParam_ioNamePtr_off);
    int16_t ref = (int16_t)rd16(paramBlock + IOParam_ioVRefNum_off);
    int16_t how = dtmvDefault, err = noErr;
    gaddr v = 0;
    int16_t more = 0;
    if (name && rd8(name)) {
        int16_t vol_len;
        gaddr tmp = tb_NewPtr(2, true, true);
        if (!tmp)
            return memFullErr;
        err = tb_UTParsePathname(tmp, name);
        vol_len = (int16_t)rd16(tmp);
        tb_DisposePtr(tmp);
        if (err) {
            if (status)
                wr16(status, dtmvError);
            return err;
        }
        if (vol_len) {
            gaddr found = 0;
            int extra = 0;
            for (gaddr q = vcb_first(); q; q = vcb_next(q)) {
                bool same = true;
                if (rd8(q + VCB_vcbVN_off) != vol_len)
                    same = false;
                for (int i = 1; same && i <= vol_len; i++)
                    same = fold(rd8(q + VCB_vcbVN_off + (uint32_t)i)) == fold(rd8(name + (uint32_t)i));
                if (same) {
                    if (!found)
                        found = q;
                    else
                        extra++;
                }
            }
            v = found;
            more = (int16_t)extra;
            how = dtmvFullPathname;
            err = v ? noErr : nsvErr;
        }
    }
    if (how == dtmvDefault) {
        if (ref == 0)
            v = rd32(LM_DefVCBPtr);
        else {
            v = vcb_of_ref(ref);
            how = ref > 0 ? dtmvDriveNum : fm_wd_index(ref) >= 0 ? dtmvWDRefNum : dtmvVRefNum;
        }
        err = v ? noErr : nsvErr;
    }
    if (status)
        wr16(status, err ? dtmvError : (uint16_t)how);
    if (moreMatches)
        wr16(moreMatches, (uint16_t)more);
    if (!err) {
        if (vRefNum)
            wr16(vRefNum, rd16(v + VCB_vcbVRefNum_off));
        if (volCtrlBlockPtr)
            wr32(volCtrlBlockPtr, v);
    }
    return err;
}

/* ---- The cache ---- */

/* Cache blocks are 512 bytes of an open file's fork, held in memory
 * until released; a volume has no blocks to cache. */
static struct {
    bool used, inuse, dirty;
    gaddr buf;
    int16_t refnum;
    uint32_t block;
} cache[MAX_BLOCKS];

void os9rest_fsm_reset(void)
{
    memset(cache, 0, sizeof cache);
    memset(in_search, 0, sizeof in_search);
    wdcbs = 0;
}

static int block_at(gaddr buf)
{
    for (int i = 0; i < MAX_BLOCKS; i++)
        if (cache[i].used && cache[i].buf == buf)
            return i;
    return -1;
}

static int16_t block_read(int i)
{
    uint32_t got;
    for (uint32_t k = 0; k < BLOCK; k++)
        wr8(cache[i].buf + k, 0);
    int16_t err = fm_fork_io(cache[i].refnum, false, 1 /* fsFromStart */, (int64_t)cache[i].block * BLOCK, BLOCK,
                             cache[i].buf, &got);
    return err == -39 /* eofErr: a block past the end reads short */ ? noErr : err;
}

/* A block written back reaches the fork's data below its end of file. */
static int16_t block_write(int i)
{
    char path[1024];
    bool rsrc, write;
    uint32_t pos, len, got;
    if (!fm_fork_state(cache[i].refnum, path, sizeof path, &rsrc, &write, &pos, &len))
        return rfNumErr;
    uint64_t start = (uint64_t)cache[i].block * BLOCK;
    if (start >= len) {
        cache[i].dirty = false;
        return noErr;
    }
    uint32_t n = len - (uint32_t)start < BLOCK ? len - (uint32_t)start : BLOCK;
    int16_t err = fm_fork_io(cache[i].refnum, true, 1, (int64_t)start, n, cache[i].buf, &got);
    if (!err)
        cache[i].dirty = false;
    return err;
}

static void block_free(int i)
{
    if (cache[i].buf)
        tb_DisposePtr(cache[i].buf);
    memset(&cache[i], 0, sizeof cache[i]);
}

int16_t tb_UTGetBlock(int16_t refNum, gaddr log2PhyProc, uint32_t blockNum, int16_t gbOption, gaddr buffer)
{
    if (!buffer)
        return paramErr;
    if (!fm_fcb_addr(refNum) || !fm_fcb_in_use(refNum))
        return volOffLinErr; /* not a fork: a volume's blocks */
    int at = -1;
    for (int i = 0; i < MAX_BLOCKS; i++)
        if (cache[i].used && cache[i].refnum == refNum && cache[i].block == blockNum)
            at = i;
    if (at >= 0) {
        if (cache[at].inuse)
            return chInUse;
        if (gbOption & gbReadMask) {
            int16_t err = block_read(at);
            if (err)
                return err;
        }
    } else {
        if (gbOption & gbExistMask)
            return chnotfound;
        for (int i = 0; i < MAX_BLOCKS && at < 0; i++)
            if (!cache[i].used)
                at = i;
        if (at < 0)
            return chNoBuf;
        gaddr b = tb_NewPtr(BLOCK, true, true);
        if (!b)
            return memFullErr;
        cache[at].used = true;
        cache[at].buf = b;
        cache[at].refnum = refNum;
        cache[at].block = blockNum;
        if (!(gbOption & gbNoReadMask)) {
            int16_t err = block_read(at);
            if (err) {
                block_free(at);
                return err;
            }
        }
    }
    cache[at].inuse = !(gbOption & gbReleaseMask);
    wr32(buffer, cache[at].buf);
    return noErr;
}

int16_t tb_UTReleaseBlock(gaddr buffer, int16_t rbOption)
{
    int i = block_at(buffer);
    if (i < 0)
        return chnotfound;
    if (!cache[i].inuse)
        return chNotInUse;
    cache[i].inuse = false;
    if (rbOption & rbDirtyMask)
        cache[i].dirty = true;
    int16_t err = noErr;
    if ((rbOption & rbWriteMask) && cache[i].dirty)
        err = block_write(i);
    if (rbOption & rbTrashMask)
        block_free(i);
    return err;
}

int16_t tb_UTMarkDirty(gaddr buffer)
{
    int i = block_at(buffer);
    if (i < 0)
        return chnotfound;
    cache[i].dirty = true;
    return noErr;
}

/* UTFlushCache(refNum, option): the dirty blocks of a fork (all if the
 * refnum is 0) written back, and dropped with the trash bit. */
int16_t tb_UTFlushCache(int16_t refNum, int16_t fcOption)
{
    int16_t first = noErr;
    for (int i = 0; i < MAX_BLOCKS; i++) {
        if (!cache[i].used || (refNum && cache[i].refnum != refNum) || cache[i].inuse)
            continue;
        if (cache[i].dirty) {
            int16_t err = block_write(i);
            if (err && !first)
                first = err;
        }
        if (fcOption & fcTrashMask)
            block_free(i);
    }
    return first;
}

static int16_t trash_where(bool (*match)(int, gaddr, uint32_t, int16_t), gaddr vcb, uint32_t arg, int16_t ref)
{
    for (int i = 0; i < MAX_BLOCKS; i++)
        if (cache[i].used && !cache[i].inuse && match(i, vcb, arg, ref))
            block_free(i);
    return noErr;
}

static bool on_vcb(int i, gaddr vcb)
{
    gaddr f = fm_fcb_addr(cache[i].refnum);
    return f && (!vcb || rd32(f + FCB_VPTR) == vcb);
}

static bool match_volume(int i, gaddr vcb, uint32_t arg, int16_t ref) { return on_vcb(i, vcb); }
static bool match_file(int i, gaddr vcb, uint32_t arg, int16_t ref)
{
    gaddr f = fm_fcb_addr(cache[i].refnum);
    return on_vcb(i, vcb) && f && rd32(f + FCB_ID) == arg;
}
static bool match_range(int i, gaddr vcb, uint32_t arg, int16_t ref) { return cache[i].refnum == ref; }

int16_t tb_UTTrashVolBlocks(gaddr volCtrlBlockPtr) { return trash_where(match_volume, volCtrlBlockPtr, 0, 0); }

int16_t tb_UTTrashFileBlocks(gaddr volCtrlBlockPtr, uint32_t fileNum)
{
    return trash_where(match_file, volCtrlBlockPtr, fileNum, 0);
}

/* UTTrashBlocks(begin, count, vcb, fileRefNum, option): the blocks of the
 * fork that the byte range touches. */
int16_t tb_UTTrashBlocks(uint32_t beginPosition, uint32_t byteCount, gaddr volCtrlBlockPtr, int16_t fileRefNum,
                         int16_t tbOption)
{
    uint64_t first = beginPosition / BLOCK, last = ((uint64_t)beginPosition + byteCount + BLOCK - 1) / BLOCK;
    for (int i = 0; i < MAX_BLOCKS; i++)
        if (cache[i].used && !cache[i].inuse && cache[i].refnum == fileRefNum && cache[i].block >= first &&
            cache[i].block < last)
            block_free(i);
    return noErr;
}

/* UTBlockInFQHashP(vRefNum, diskBlock): whether a block is in the cache
 * (the volumes' blocks never are). */
int16_t tb_UTBlockInFQHashP(int16_t vRefNum, uint32_t diskBlock) { return chnotfound; }

/* UTCacheReadIP and UTCacheWriteIP: the fork's bytes at a position. */
static int16_t cache_io(bool write, uint32_t pos, gaddr buf, int16_t ref, uint32_t count, gaddr actual)
{
    uint32_t got = 0;
    if (!fm_fcb_addr(ref) || !fm_fcb_in_use(ref))
        return volOffLinErr;
    int16_t err = fm_fork_io(ref, write, 1 /* fsFromStart */, pos, count, buf, &got);
    if (actual)
        wr32(actual, got);
    return err;
}

int16_t tb_UTCacheReadIP(gaddr log2PhyProc, uint32_t filePosition, gaddr ioBuffer, int16_t fileRefNum,
                         uint32_t reqCount, gaddr actCount, int16_t cacheOption)
{
    return cache_io(false, filePosition, ioBuffer, fileRefNum, reqCount, actCount);
}

int16_t tb_UTCacheWriteIP(gaddr log2PhyProc, uint32_t filePosition, gaddr ioBuffer, int16_t fileRefNum,
                          uint32_t reqCount, gaddr actCount, int16_t cacheOption)
{
    return cache_io(true, filePosition, ioBuffer, fileRefNum, reqCount, actCount);
}

/* A volume is files in a folder, with no blocks to read or write. */
int16_t tb_UTVolCacheReadIP(gaddr volCtrlBlockPtr, uint32_t blockPosition, gaddr ioBuffer, uint32_t reqCount,
                            gaddr actCount, int16_t cacheOption)
{
    if (actCount)
        wr32(actCount, 0);
    return vcb_listed(volCtrlBlockPtr) ? volOffLinErr : nsvErr;
}

int16_t tb_UTVolCacheWriteIP(gaddr volCtrlBlockPtr, uint32_t blockPosition, gaddr ioBuffer, uint32_t reqCount,
                             gaddr actCount, int16_t cacheOption)
{
    if (actCount)
        wr32(actCount, 0);
    return vcb_listed(volCtrlBlockPtr) ? volOffLinErr : nsvErr;
}
