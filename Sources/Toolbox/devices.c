/*
 * The Device Manager's installation and immediate calls, the drive queue,
 * slot retraces, deferred user functions, the Extensions Manager's notification calls and the
 * File Transfer Manager's System 7.5 calls: what InterfaceLib had for them.
 */
#include <string.h>

#include "ctb.h"
#include "legacy.h"
#include "tb_internal.h"

enum {
    qErr = -1, badUnitErr = -21, dRemovErr = -25, nsDrvErr = -56, noDriveErr = -64, slotNumErr = -360,
};

/* ---- Drivers ---- */

int16_t tb_DriverInstall(gaddr drvrPtr, int16_t refNum) { return driver_install(drvrPtr, refNum); }
int16_t tb_DriverInstallReserveMem(gaddr drvrPtr, int16_t refNum) { return driver_install(drvrPtr, refNum); }

/* DrvrRemove ($A03E): the unit table's entry gone, unless the driver is
 * open. */
int16_t tb_DrvrRemove(int16_t refNum)
{
    int16_t unit = (int16_t)(-1 - refNum);
    gaddr table = rd32(LM_UTableBase);
    if (refNum >= 0 || !table || unit >= (int16_t)rd16(LM_UnitNtryCnt))
        return badUnitErr;
    gaddr h = rd32(table + 4 * (uint32_t)unit);
    if (!h || !rd32(h))
        return qErr; /* nothing installed there */
    if (rd16(rd32(h) + DCtlEntry_dCtlFlags_off) & 0x20) /* dOpened */
        return dRemovErr;
    wr32(table + 4 * (uint32_t)unit, 0);
    if (!driver_refnum(refNum)) /* ours keep their entries' memory */
        tb_DisposeHandle(h);
    return noErr;
}

/* The immediate calls: done at once, outside the driver's queue. */
int16_t tb_PBOpenImmed(gaddr paramBlock) { return tb_PBOpen(paramBlock, false); }
int16_t tb_PBCloseImmed(gaddr paramBlock) { return tb_PBClose(paramBlock, false); }
int16_t tb_PBReadImmed(gaddr paramBlock) { return tb_PBRead(paramBlock, false); }
int16_t tb_PBWriteImmed(gaddr paramBlock) { return tb_PBWrite(paramBlock, false); }
int16_t tb_PBStatusImmed(gaddr paramBlock) { return tb_PBStatus(paramBlock, false); }
int16_t tb_PBKillIOImmed(gaddr paramBlock) { return tb_PBKillIO(paramBlock, false); }

/* ---- The drive queue ---- */

/* AddDrive: the element named for the drive and its driver, at the end
 * of the drive queue. */
void tb_AddDrive(int16_t drvrRefNum, int16_t drvNum, gaddr qEl)
{
    if (!qEl)
        return;
    wr16(qEl + DrvQEl_dQDrive_off, (uint16_t)drvNum);
    wr16(qEl + DrvQEl_dQRefNum_off, (uint16_t)drvrRefNum);
    wr16(qEl + DrvQEl_qType_off, 0);
    tb_Enqueue(qEl, LM_DrvQHdr);
}

static gaddr find_drive(int16_t drvNum)
{
    for (gaddr e = rd32(LM_DrvQHdr + 2); e; e = rd32(e))
        if ((int16_t)rd16(e + DrvQEl_dQDrive_off) == drvNum)
            return e;
    return 0;
}

/* DriveStatus: the drive's driver's Status call drvStsCode (8). */
int16_t tb_DriveStatus(int16_t drvNum, gaddr status)
{
    gaddr e = find_drive(drvNum);
    if (!e)
        return nsDrvErr;
    gaddr pb = tb_NewPtr(CntrlParam_SIZEOF, false, true);
    if (!pb)
        return memFullErr;
    wr16(pb + IOParam_ioRefNum_off, rd16(e + DrvQEl_dQRefNum_off));
    wr16(pb + IOParam_ioVRefNum_off, (uint16_t)drvNum);
    wr16(pb + CntrlParam_csCode_off, 8);
    int16_t r = tb_PBStatus(pb, false);
    if (r == noErr && status)
        gmemcpy(status, pb + CntrlParam_csParam_off, DrvSts_SIZEOF);
    tb_DisposePtr(pb);
    return r;
}

/* ---- Slot retraces ($A071, $A072) ---- */

/* AttachVBL: the system's retrace tasks follow a slot's video; slot 0's
 * is the only video there is. */
int16_t tb_AttachVBL(int16_t theSlot) { return theSlot == 0 ? noErr : slotNumErr; }

int16_t tb_DoVBLTask(int16_t theSlot)
{
    if (theSlot != 0)
        return slotNumErr;
    time_vbl_task();
    return noErr;
}

/* DeferUserFn ($A08F): with virtual memory off (Gestalt 'vm  ' 0) nothing
 * waits on a page fault, so the function runs now, A0 its argument. */
int16_t tb_DeferUserFn(gaddr userFunction, gaddr argument)
{
    if (!userFunction)
        return paramErr;
    cpu68k *c = g_cpu;
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d);
    memcpy(a, c->a, sizeof a);
    uint16_t sr = c->sr;
    c->a[0] = argument;
    guest_call_regs(userFunction);
    memcpy(c->d, d, sizeof d);
    memcpy(c->a, a, sizeof a);
    c->sr = sr;
    return noErr;
}

/* ---- The Disk Initialization Package's extended calls: drivevol.c ---- */

/* The C glue: dizero's name a C string, dibadmount's point by address. */
int16_t tb_dizero(int16_t drvnum, gaddr volName)
{
    gaddr p = tb_NewPtr(256, false, true);
    if (!p)
        return memFullErr;
    uint32_t n = 0;
    while (volName && n < 255 && rd8(volName + n))
        n++;
    wr8(p, (uint8_t)n);
    if (n)
        gmemcpy(p + 1, volName, n);
    int16_t r = tb_DIZero(drvnum, p);
    tb_DisposePtr(p);
    return r;
}

int16_t tb_dibadmount(gaddr where, int32_t evtMessage) { return tb_DIBadMount(where ? rd32(where) : 0, evtMessage); }

/* ---- The Extensions Manager ($AA7D) ---- */

/* Extensions load at startup; Blue Box loads none, so the procedures are
 * kept but no notification ever comes, and the table has no elements. */
enum { MAX_EXT_PROCS = 8, EXT_ELEMENT_SIZE = 94, EXT_HEADER_SIZE = 20 };
static gaddr notification_procs[MAX_EXT_PROCS], table_procs[MAX_EXT_PROCS];

static int16_t add_proc(gaddr *list, gaddr proc)
{
    if (!proc)
        return paramErr;
    for (int i = 0; i < MAX_EXT_PROCS; i++)
        if (!list[i]) {
            list[i] = proc;
            return noErr;
        }
    return memFullErr;
}

static int16_t remove_proc(gaddr *list, gaddr proc)
{
    for (int i = 0; i < MAX_EXT_PROCS; i++)
        if (proc && list[i] == proc) {
            list[i] = 0;
            return noErr;
        }
    return paramErr;
}

int16_t tb_InstallExtensionNotificationProc(gaddr extNotificationProc)
{
    return add_proc(notification_procs, extNotificationProc);
}

int16_t tb_RemoveExtensionNotificationProc(gaddr extNotificationProc)
{
    return remove_proc(notification_procs, extNotificationProc);
}

int16_t tb_InstallExtensionTableHandlerProc(gaddr extMgrProc, gaddr extTable)
{
    int16_t r = add_proc(table_procs, extMgrProc);
    if (r)
        return r;
    gaddr h = tb_NewHandle(EXT_HEADER_SIZE, true, true);
    if (!h) {
        remove_proc(table_procs, extMgrProc);
        return memFullErr;
    }
    gaddr t = rd32(h);
    wr32(t, EXT_HEADER_SIZE);      /* extTableHeaderSize */
    wr32(t + 4, 0x00000100);       /* extTableVersion: kExtensionTableVersion */
    wr32(t + 12, EXT_ELEMENT_SIZE); /* extElementSize; no elements */
    if (extTable)
        wr32(extTable, h);
    return noErr;
}

int16_t tb_RemoveExtensionTableHandlerProc(gaddr extMgrProc) { return remove_proc(table_procs, extMgrProc); }

/* ---- The File Transfer Manager's System 7.5 calls ---- */

/* FTSendAsync and FTReceiveAsync: the transfer goes on through FTExec, as
 * FTSend's and FTReceive's do. */
int16_t tb_FTSendAsync(gaddr hFT, int16_t numFiles, gaddr pFSSpec, gaddr notifyProc)
{
    uint32_t a[4] = { hFT, (uint32_t)(int32_t)numFiles, pFSSpec, notifyProc };
    return (int16_t)ft_send(0x0224, a);
}

int16_t tb_FTReceiveAsync(gaddr hFT, gaddr pFSSpec, gaddr notifyProc)
{
    uint32_t a[3] = { hFT, pFSSpec, notifyProc };
    return (int16_t)ft_receive(0x0225, a);
}

/* FTCompletionAsync: the transfer taken on, as FTExec takes it. */
int16_t tb_FTCompletionAsync(gaddr hFT, uint16_t completionCall)
{
    uint32_t a[1] = { hFT };
    ft_exec(0x020A, a);
    return 0; /* ftNoErr */
}

void devices_reset(void)
{
    memset(notification_procs, 0, sizeof notification_procs);
    memset(table_procs, 0, sizeof table_procs);
}

/* ADBOp: InterfaceLib's, its arguments as _ADBOp's ADBOpBlock has them. */
int16_t tb_ADBOp(gaddr data, gaddr procp, gaddr buffer, int16_t command)
{
    return adb_op(buffer, procp, data, (uint8_t)command);
}
