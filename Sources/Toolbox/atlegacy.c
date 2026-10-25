/*
 * InterfaceLib's AppleTalk and Ethernet glue (AppleTalk.h, ENET.h): each
 * call a Control call on its driver, as the glue made it.  .MPP's LAP
 * calls and .ATP's are appletalk.c's and appletalk_atp.c's, .XPP's ASP
 * and AFP calls xpp.c's.  The Ethernet driver (.ENET) is never open: its
 * calls on a reference number that isn't a driver of ours are notOpenErr.
 */
#include "legacy.h"
#include "tb_internal.h"

enum { mppRefNum = -10, atpRefNum = -11, xppRefNum = -41 };
enum {
    writeLAP = 243, detachPH = 244, attachPH = 245, killAllGetReq = 259,
    openSess = 255, closeSess = 254, userCommand = 253, userWrite = 252, getStatus = 251, afpCall = 250,
    getParms = 249, abortOS = 248, closeAll = 247, xCall = 246, zipGetLocalZones = 5, zipGetZoneList = 6,
};
enum { notOpenErr = -28 };

/* A Control call; an answer given at once is written to ioResult, and an
 * asynchronous call's completion routine queued. */
static int16_t control(gaddr pb, int16_t refnum, int16_t code, bool async)
{
    if (refnum)
        wr16(pb + IOParam_ioRefNum_off, (uint16_t)refnum);
    wr16(pb + CntrlParam_csCode_off, (uint16_t)code);
    int16_t r = tb_PBControl(pb, async);
    if (r == ioInProgress)
        return noErr;
    wr16(pb + IOParam_ioResult_off, (uint16_t)r);
    if (async && rd32(pb + IOParam_ioCompletion_off))
        io_complete_later(pb, r);
    return r;
}

/* ---- .XPP: ASP, AFP and ZIP ---- */

int16_t tb_ASPOpenSession(gaddr thePBptr, int8_t async) { return control(thePBptr, xppRefNum, openSess, async); }
int16_t tb_ASPCloseSession(gaddr thePBptr, int8_t async) { return control(thePBptr, xppRefNum, closeSess, async); }
int16_t tb_ASPAbortOS(gaddr thePBptr, int8_t async) { return control(thePBptr, xppRefNum, abortOS, async); }
int16_t tb_ASPGetParms(gaddr thePBptr, int8_t async) { return control(thePBptr, xppRefNum, getParms, async); }
int16_t tb_ASPCloseAll(gaddr thePBptr, int8_t async) { return control(thePBptr, xppRefNum, closeAll, async); }
int16_t tb_ASPUserWrite(gaddr thePBptr, int8_t async) { return control(thePBptr, xppRefNum, userWrite, async); }
int16_t tb_ASPUserCommand(gaddr thePBptr, int8_t async) { return control(thePBptr, xppRefNum, userCommand, async); }
int16_t tb_ASPGetStatus(gaddr thePBptr, int8_t async) { return control(thePBptr, xppRefNum, getStatus, async); }
int16_t tb_AFPCommand(gaddr thePBptr, int8_t async) { return control(thePBptr, xppRefNum, afpCall, async); }

int16_t tb_GetLocalZones(gaddr thePBptr, int8_t async)
{
    wr16(thePBptr + 28, zipGetLocalZones); /* xppSubCode */
    return control(thePBptr, xppRefNum, xCall, async);
}

int16_t tb_GetZoneList(gaddr thePBptr, int8_t async)
{
    wr16(thePBptr + 28, zipGetZoneList);
    return control(thePBptr, xppRefNum, xCall, async);
}

/* ---- .MPP's LAP calls, .ATP ---- */

int16_t tb_PAttachPH(gaddr thePBptr, int8_t async) { return control(thePBptr, mppRefNum, attachPH, async); }
int16_t tb_PDetachPH(gaddr thePBptr, int8_t async) { return control(thePBptr, mppRefNum, detachPH, async); }
int16_t tb_PWriteLAP(gaddr thePBptr, int8_t async) { return control(thePBptr, mppRefNum, writeLAP, async); }
int16_t tb_ATPKillAllGetReq(gaddr thePBPtr, int8_t async) { return control(thePBPtr, atpRefNum, killAllGetReq, async); }

/* BuildLAPwds: the frame's first three bytes its LAP header (the
 * destination node, then the type), one entry for the whole frame. */
void tb_BuildLAPwds(gaddr wdsPtr, gaddr dataPtr, int16_t destHost, int16_t prototype, int16_t frameLen)
{
    wr8(dataPtr, (uint8_t)destHost);
    wr8(dataPtr + 2, (uint8_t)prototype);
    wr16(wdsPtr, (uint16_t)frameLen);
    wr32(wdsPtr + 2, dataPtr);
    wr16(wdsPtr + 6, 0);
}

/* The network has no router (ZIP has no zones): no bridge's node. */
int16_t tb_GetBridgeAddress(void) { return 0; }

/* ATP stays loaded while AppleTalk is open. */
int16_t tb_ATPUnload(void) { return noErr; }

int16_t tb_LAPAddATQ(gaddr theATQEntry) { return at_transition_queue(theATQEntry, true); }
int16_t tb_LAPRmvATQ(gaddr theATQEntry) { return at_transition_queue(theATQEntry, false); }
void tb_ATEvent(int32_t event, gaddr infoPtr) { at_transition_event(event, infoPtr, false, 0); }
int16_t tb_ATPreFlightEvent(int32_t event, int32_t cancel, gaddr infoPtr)
{
    return (int16_t)at_transition_event(event, infoPtr, true, cancel);
}

/* ---- .ENET ---- */

enum {
    ENetDelMulti = 245, ENetAddMulti = 246, ENetAttachPH = 247, ENetDetachPH = 248, ENetWrite = 249,
    ENetRead = 250, ENetRdCancel = 251, ENetGetInfo = 252, ENetSetGeneral = 253,
};

static int16_t enet(gaddr pb, int16_t code, bool async)
{
    if (!driver_refnum((int16_t)rd16(pb + IOParam_ioRefNum_off))) {
        wr16(pb + IOParam_ioResult_off, (uint16_t)notOpenErr);
        if (async && rd32(pb + IOParam_ioCompletion_off))
            io_complete_later(pb, notOpenErr);
        return notOpenErr;
    }
    return control(pb, 0, code, async);
}

int16_t tb_EWrite(gaddr thePBptr, int8_t async) { return enet(thePBptr, ENetWrite, async); }
int16_t tb_EAttachPH(gaddr thePBptr, int8_t async) { return enet(thePBptr, ENetAttachPH, async); }
int16_t tb_EDetachPH(gaddr thePBptr, int8_t async) { return enet(thePBptr, ENetDetachPH, async); }
int16_t tb_ERead(gaddr thePBptr, int8_t async) { return enet(thePBptr, ENetRead, async); }
int16_t tb_ERdCancel(gaddr thePBptr, int8_t async) { return enet(thePBptr, ENetRdCancel, async); }
int16_t tb_EGetInfo(gaddr thePBptr, int8_t async) { return enet(thePBptr, ENetGetInfo, async); }
int16_t tb_ESetGeneral(gaddr thePBptr, int8_t async) { return enet(thePBptr, ENetSetGeneral, async); }
int16_t tb_EAddMulti(gaddr thePBptr, int8_t async) { return enet(thePBptr, ENetAddMulti, async); }
int16_t tb_EDelMulti(gaddr thePBptr, int8_t async) { return enet(thePBptr, ENetDelMulti, async); }
