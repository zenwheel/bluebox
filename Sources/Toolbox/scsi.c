/*
 * The SCSI Manager, for a SCSI bus with nothing on it but the Mac itself
 * (ID 7): Blue Box's volumes are host folders, not SCSI disks.  Gestalt's
 * 'hdwr' says the machine has a SCSI chip, and 'scsi' that SCSI Manager
 * 4.3 is there; disk utilities scanning the bus find no devices.
 *
 * The original SCSI Manager (_SCSIDispatch, $A815, the selector on the
 * stack): SCSIGet arbitrates for the free bus; SCSISelect of any target
 * times out (scCommErr) and leaves the bus free; with no target selected,
 * the calls of a command's phases are out of sequence.
 *
 * SCSI Manager 4.3 (_SCSIAtomic, $A089, the selector in D0): the
 * transport answers for one bus, 0; an I/O to any target times out in
 * selection.  Drivers' reference-number cross-references are kept.  A
 * third party's SIM can't register a bus: there is no hardware for it.
 *
 * The ATA Manager (PowerPC only, as on a G3) has no ATA buses either.
 */
#include <string.h>

#include "legacy.h"
#include "tb_internal.h"

/* ---- The original SCSI Manager ---- */

enum {
    scCommErr = 2, scBadParmsErr = 4, scMgrBusyErr = 7, scSequenceErr = 8,
};

static bool arbitrated; /* SCSIGet has the bus */

int16_t tb_SCSIReset(void)
{
    arbitrated = false;
    return noErr;
}

int16_t tb_SCSIGet(void)
{
    if (arbitrated)
        return scMgrBusyErr;
    arbitrated = true;
    return noErr;
}

/* No target answers selection: the bus goes free again. */
static int16_t select_target(int16_t id)
{
    if (!arbitrated)
        return scSequenceErr;
    arbitrated = false;
    if (id < 0 || id > 7)
        return scBadParmsErr;
    return scCommErr;
}

int16_t tb_SCSISelect(int16_t targetID) { return select_target(targetID); }
int16_t tb_SCSISelAtn(int16_t targetID) { return select_target(targetID); }

/* The phases of a command: there is never a target connected. */
int16_t tb_SCSICmd(gaddr buffer, int16_t count) { return scSequenceErr; }
int16_t tb_SCSIRead(gaddr tibPtr) { return scSequenceErr; }
int16_t tb_SCSIRBlind(gaddr tibPtr) { return scSequenceErr; }
int16_t tb_SCSIWrite(gaddr tibPtr) { return scSequenceErr; }
int16_t tb_SCSIWBlind(gaddr tibPtr) { return scSequenceErr; }
int16_t tb_SCSIMsgIn(gaddr message) { return scSequenceErr; }
int16_t tb_SCSIMsgOut(int16_t message) { return scSequenceErr; }

int16_t tb_SCSIComplete(gaddr stat, gaddr message, uint32_t wait)
{
    arbitrated = false;
    return scSequenceErr;
}

/* The bus's status lines: none asserted (no BSY, no SEL). */
int16_t tb_SCSIStat(void) { return 0; }

/* ---- SCSI Manager 4.3 ---- */

enum {
    scsiRequestAborted = -7934, scsiUnableToAbort = -7933, scsiUnableToTerminate = -7927,
    scsiSelectTimeout = -7926, scsiDeviceNotThere = -7885, scsiNoSuchXref = -7882,
    scsiFunctionNotAvailable = -7871, scsiBusInvalid = -7869, scsiTIDInvalid = -7868, scsiLUNInvalid = -7867,
};
/* SCSIHdr */
enum { sh_result = 10, sh_device = 12, sh_completion = 16, sh_size = 36, SCSI_IO_SIZE = 176 };
enum { HOST_ID = 7 };

/* Cross-references from a device to its driver's reference number. */
enum { MAX_XREFS = 16 };
static struct {
    uint32_t device;
    int16_t refnum;
    uint16_t flags;
} xrefs[MAX_XREFS];
static int nxrefs;

void scsi_reset(void)
{
    arbitrated = false;
    nxrefs = 0;
}

/* A device on our bus: bus 0, a target other than the Mac, LUN 0-7. */
static int16_t check_device(gaddr pb)
{
    uint8_t bus = rd8(pb + sh_device + 1), target = rd8(pb + sh_device + 2), lun = rd8(pb + sh_device + 3);
    if (bus != 0)
        return scsiBusInvalid;
    if (target > 7 || target == HOST_ID)
        return scsiTIDInvalid;
    if (lun > 7)
        return scsiLUNInvalid;
    return noErr;
}

static void put_name(gaddr at, const char *s)
{
    gmemset(at, 0, 16);
    gput(at, s, (uint32_t)strlen(s));
}

static int16_t bus_inquiry(gaddr pb)
{
    uint8_t bus = rd8(pb + sh_device + 1);
    if (bus != 0 && bus != 0xFF) /* 0xFF: the transport itself */
        return scsiBusInvalid;
    gmemset(pb + sh_size, 0, 172 - sh_size);
    wr16(pb + 38, 2);                    /* scsiMaxTransferType: blind and polled */
    wr32(pb + 40, 1u << 0 | 1u << 1 | 1u << 2); /* buffers, TIBs, scatter/gather */
    wr16(pb + 44, SCSI_IO_SIZE);         /* scsiIOpbSize */
    wr16(pb + 46, SCSI_IO_SIZE);         /* scsiMaxIOpbSize */
    wr8(pb + 52, 1);                     /* scsiVersionNumber */
    wr8(pb + 68, 0);                     /* scsiHiBusID: one bus */
    wr8(pb + 69, HOST_ID);               /* scsiInitiatorID */
    wr16(pb + 84, 7);                    /* scsiMaxTarget */
    wr16(pb + 86, 7);                    /* scsiMaxLUN */
    put_name(pb + 88, "Blue Box");       /* scsiSIMVendor */
    put_name(pb + 104, "Blue Box");      /* scsiHBAVendor */
    put_name(pb + 120, "Blue Box");      /* scsiControllerFamily */
    put_name(pb + 136, "no devices");    /* scsiControllerType */
    gput(pb + 152, "4.3\0", 4);          /* scsiXPTversion */
    gput(pb + 156, "1.0\0", 4);          /* scsiSIMversion */
    gput(pb + 160, "1.0\0", 4);          /* scsiHBAversion */
    return noErr;
}

static int16_t xref_call(uint8_t fn, gaddr pb)
{
    uint32_t dev = rd32(pb + sh_device);
    int i = 0;
    while (i < nxrefs && xrefs[i].device != dev)
        i++;
    switch (fn) {
    case 0x85: /* SCSICreateRefNumXref */
        if (i == nxrefs) {
            if (nxrefs == MAX_XREFS)
                return -108; /* memFullErr */
            nxrefs++;
        }
        xrefs[i].device = dev;
        xrefs[i].refnum = (int16_t)rd16(pb + 36);
        xrefs[i].flags = rd16(pb + 38);
        return noErr;
    case 0x86: /* SCSILookupRefNumXref: and the one after it */
        /* A device on bus $FF asks for the first (Norton's Volume Recover
         * passes $00FF0000, others $FFFFFFFF); a next device on bus $FF
         * ends the list, and a device not in it has none after it, so a
         * scan stops either way. */
        if (rd8(pb + sh_device + 1) == 0xFF) {
            wr32(pb + 40, nxrefs ? xrefs[0].device : 0xFFFFFFFFu);
            return noErr;
        }
        if (i == nxrefs) {
            wr16(pb + 36, 0);
            wr32(pb + 40, 0xFFFFFFFFu);
            return scsiNoSuchXref;
        }
        wr16(pb + 36, (uint16_t)xrefs[i].refnum);
        wr16(pb + 38, xrefs[i].flags);
        wr32(pb + 40, i + 1 < nxrefs ? xrefs[i + 1].device : 0xFFFFFFFFu);
        return noErr;
    default: /* SCSIRemoveRefNumXref */
        if (i == nxrefs)
            return scsiNoSuchXref;
        xrefs[i] = xrefs[--nxrefs];
        return noErr;
    }
}

static int16_t action(gaddr pb)
{
    uint8_t fn = rd8(pb + 8);
    int16_t r;
    switch (fn) {
    case 0x00: r = noErr; break; /* SCSINop */
    case 0x01:                   /* SCSIExecIO */
        r = check_device(pb);
        if (r == noErr)
            r = scsiSelectTimeout;
        wr32(pb + 64, rd32(pb + 44)); /* scsiDataResidual: scsiDataLength, nothing moved */
        break;
    case 0x03: r = bus_inquiry(pb); break;
    case 0x04: r = rd8(pb + sh_device + 1) ? scsiBusInvalid : noErr; break; /* SCSIReleaseQ: nothing frozen */
    case 0x10: r = scsiUnableToAbort; break;                                /* SCSIAbortCommand: nothing queued */
    case 0x11: r = rd8(pb + sh_device + 1) ? scsiBusInvalid : noErr; break; /* SCSIResetBus */
    case 0x12: r = check_device(pb) ? check_device(pb) : scsiSelectTimeout; break; /* SCSIResetDevice */
    case 0x13: r = scsiUnableToTerminate; break;                            /* SCSITerminateIO */
    case 0x80: { /* SCSIGetVirtualIDInfo: an old-API ID is on bus 0, and nothing is there */
        uint16_t id = rd16(pb + 36);
        wr8(pb + sh_device + 1, 0);
        wr8(pb + sh_device + 2, (uint8_t)id);
        wr8(pb + sh_device + 3, 0);
        wr8(pb + 38, id == HOST_ID);
        r = id > 7 ? scsiTIDInvalid : noErr;
        break;
    }
    case 0x82: r = check_device(pb) ? check_device(pb) : scsiDeviceNotThere; break; /* SCSILoadDriver */
    case 0x85:
    case 0x86:
    case 0x87: r = xref_call(fn, pb); break;
    default: r = scsiFunctionNotAvailable; break;
    }
    wr16(pb + sh_result, (uint16_t)r);
    /* An I/O with a completion routine was asynchronous: the routine is
     * called with the PB (C convention), and the call itself accepted. */
    gaddr done = rd32(pb + sh_completion);
    if (done && fn == 0x01) {
        cpu_push32(g_cpu, pb);
        guest_call(done);
        g_cpu->a[7] += 4;
        return noErr;
    }
    return r;
}

int16_t tb_SCSIAction(gaddr parameterBlock) { return parameterBlock ? action(parameterBlock) : paramErr; }
int16_t tb_SCSIRegisterBus(gaddr parameterBlock) { return scsiFunctionNotAvailable; }
int16_t tb_SCSIDeregisterBus(gaddr parameterBlock) { return scsiBusInvalid; }
int16_t tb_SCSIReregisterBus(gaddr parameterBlock) { return scsiBusInvalid; }
int16_t tb_SCSIKillXPT(gaddr parameterBlock) { return scsiFunctionNotAvailable; }

/* ---- The ATA Manager: no ATA buses ---- */

enum { ATAPBInvalid = -9406 + 0x2F, ATAFuncNotSupported = -9406 + 0x30 };
/* ataPBHdr */
enum { ata_callback = 12, ata_result = 16, ata_function = 18, ata_size = 48 };

int16_t tb_ataManager(gaddr pb)
{
    if (!pb)
        return paramErr;
    int16_t r;
    switch (rd8(pb + ata_function)) {
    case 0x00: r = noErr; break; /* kATAMgrNOP */
    case 0x90:                   /* kATAMgrManagerInquiry */
        gmemset(pb + ata_size, 0, 16);
        wr32(pb + ata_size, 0x01008000); /* ataMgrVersion 1.0 */
        wr8(pb + ata_size + 4, 1);       /* ataMgrPBVers */
        r = noErr;                       /* ataBusCnt, ataDevCnt: 0 */
        break;
    case 0x01: case 0x03: case 0x04: case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x82: case 0x85: case 0x86: case 0x87: case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C:
    case 0x98:
        r = ATAPBInvalid; /* the bus the device ID names isn't there */
        break;
    default: r = ATAFuncNotSupported; break;
    }
    wr16(pb + ata_result, (uint16_t)r);
    gaddr done = rd32(pb + ata_callback);
    if (done) {
        cpu_push32(g_cpu, pb);
        guest_call(done);
        g_cpu->a[7] += 4;
    }
    return r;
}
