/*
 * The SCSI Manager, for a SCSI bus with the Mac (ID 7) and its hard disk
 * (ID 0) on it: the disk the Macintosh HD volume is on, whose driver is
 * .BlueBoxDisk at reference number -33, SCSI ID 0's (diskdriver.c).
 * Gestalt's 'hdwr' says the machine has a SCSI chip, and 'scsi' that SCSI
 * Manager 4.3 is there.
 *
 * The disk answers the commands a Mac's hard disk did: TEST UNIT READY,
 * REQUEST SENSE, INQUIRY, MODE SENSE (the block descriptor, and the
 * error-recovery, format, geometry and caching pages), READ CAPACITY,
 * READ(6) and READ(10), VERIFY, START STOP UNIT, PREVENT ALLOW, SYNCHRONIZE
 * CACHE.  Its blocks are those of the volume's drive: the files jLg2Phys
 * placed there read through the driver, the rest is zeros.  Commands that
 * would change it (WRITE, FORMAT UNIT, MODE SELECT) end in CHECK CONDITION
 * with DATA PROTECT: the volume is a host folder, and a raw write or a
 * format must not reach its files.  Disk utilities and tape and archive
 * programs that ask the startup disk for its block size and geometry
 * (suntar) get a disk's answers; formatters are told it is protected.
 *
 * The original SCSI Manager (_SCSIDispatch, $A815, the selector on the
 * stack): SCSIGet arbitrates for the free bus; SCSISelect of ID 0 connects
 * to the disk, of any other target times out (scCommErr) and leaves the bus
 * free.  SCSICmd sends the command; SCSIRead, SCSIRBlind, SCSIWrite and
 * SCSIWBlind move its data as a transfer instruction block says; SCSIComplete
 * gives the status and message bytes and frees the bus.  Calls out of
 * sequence say so.
 *
 * SCSI Manager 4.3 (_SCSIAtomic, $A089, the selector in D0): the
 * transport answers for one bus, 0; SCSIExecIO runs a command on the disk
 * (to a buffer, a TIB or a scatter/gather list, with autosense), and an I/O
 * to any other target times out in selection.  Drivers' reference-number
 * cross-references are kept, the disk's own among them.  A third party's
 * SIM can't register a bus: there is no hardware for it.
 *
 * The ATA Manager (PowerPC only, as on a G3) has no ATA buses either.
 */
#include <stdlib.h>
#include <string.h>

#include "legacy.h"
#include "tb_internal.h"

/* ---- The disk at SCSI ID 0 ---- */

enum { DISK_ID = 0, BLOCK = 512 };
/* 2 GB, as the drive queue gives the drive (filemgr.c); 64 blocks a track,
 * 16 heads (the geometry pages). */
enum { DISK_BLOCKS = 0x400000, SECTORS = 64, HEADS = 16 };
enum { GOOD = 0x00, CHECK_CONDITION = 0x02 };
enum { NO_SENSE = 0, NOT_READY = 2, ILLEGAL_REQUEST = 5, DATA_PROTECT = 7 };

static struct {
    uint8_t key, asc; /* the last command's sense */
} sense;

/* A command's data: the bytes it gives (in), or how many it takes (out). */
static struct {
    uint8_t *in;
    uint32_t len, pos;
    bool out;
    uint8_t status;
} xfer;

static gaddr io_pb, io_buf; /* a parameter block and a block for reading the drive */

static void xfer_clear(void)
{
    free(xfer.in);
    memset(&xfer, 0, sizeof xfer);
}

static uint8_t check(uint8_t key, uint8_t asc)
{
    sense.key = key;
    sense.asc = asc;
    return CHECK_CONDITION;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

/* Blocks of the drive, through its driver's read: those of files placed
 * on it, zeros elsewhere. */
static void read_blocks(uint32_t block, uint32_t count, uint8_t *out)
{
    if (!io_pb) {
        io_pb = mm_new_ptr_in(rd32(LM_SysZone), IOParam_SIZEOF, true);
        io_buf = mm_new_ptr_in(rd32(LM_SysZone), BLOCK, true);
    }
    for (uint32_t i = 0; i < count; i++, out += BLOCK) {
        memset(out, 0, BLOCK);
        if (!io_pb || !io_buf)
            continue;
        gmemset(io_pb, 0, IOParam_SIZEOF);
        wr32(io_pb + IOParam_ioBuffer_off, io_buf);
        wr32(io_pb + IOParam_ioReqCount_off, BLOCK);
        wr16(io_pb + IOParam_ioPosMode_off, 1); /* fsFromStart */
        wr32(io_pb + IOParam_ioPosOffset_off, (block + i) * BLOCK);
        if (fm_block_io(io_pb, false) == noErr)
            gget(out, io_buf, BLOCK);
    }
}

/* The mode pages asked for (page 0x3F: all of them) after the header and
 * block descriptor; false for a page the disk doesn't have. */
static bool mode_pages(uint8_t page, uint8_t *p, uint32_t *n)
{
    bool all = page == 0x3F, any = false;
    if (all || page == 0x01) { /* read-write error recovery */
        static const uint8_t pg[] = { 0x01, 0x0A, 0xC0, 8, 0, 0, 0, 0, 8, 0, 0xFF, 0xFF };
        memcpy(p + *n, pg, sizeof pg), *n += sizeof pg, any = true;
    }
    if (all || page == 0x03) { /* format: tracks, sectors and their size */
        uint8_t pg[24] = { 0x03, 0x16 };
        pg[3] = HEADS;                          /* tracks per zone */
        pg[10] = SECTORS >> 8, pg[11] = SECTORS; /* sectors per track */
        pg[12] = BLOCK >> 8, pg[13] = BLOCK & 0xFF;
        pg[20] = 0x40;                          /* hard-sectored */
        memcpy(p + *n, pg, sizeof pg), *n += sizeof pg, any = true;
    }
    if (all || page == 0x04) { /* rigid disk geometry: cylinders and heads */
        uint32_t cyl = DISK_BLOCKS / (SECTORS * HEADS);
        uint8_t pg[24] = { 0x04, 0x16, (uint8_t)(cyl >> 16), (uint8_t)(cyl >> 8), (uint8_t)cyl, HEADS };
        pg[20] = 0x1C, pg[21] = 0x20; /* 7200 rpm */
        memcpy(p + *n, pg, sizeof pg), *n += sizeof pg, any = true;
    }
    if (all || page == 0x08) { /* caching: read cache on, no write cache */
        static const uint8_t pg[] = { 0x08, 0x0A, 0x00, 0, 0xFF, 0xFF, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF };
        memcpy(p + *n, pg, sizeof pg), *n += sizeof pg, any = true;
    }
    return any || page == 0x00;
}

/* MODE SENSE(6) or (10): header, one block descriptor (unless DBD), pages. */
static uint8_t mode_sense(const uint8_t *cdb, bool ten)
{
    uint8_t page = cdb[2] & 0x3F, pc = cdb[2] >> 6;
    bool dbd = cdb[1] & 0x08;
    uint32_t alloc = ten ? (uint32_t)cdb[7] << 8 | cdb[8] : cdb[4];
    uint8_t buf[256] = { 0 };
    uint32_t hdr = ten ? 8 : 4, n = hdr;
    if (pc == 3)
        return check(ILLEGAL_REQUEST, 0x39); /* saved values: not supported */
    if (!dbd) {
        put32(buf + n, DISK_BLOCKS);       /* density 0, number of blocks */
        put32(buf + n + 4, BLOCK);         /* reserved, block length */
        n += 8;
    }
    if (!mode_pages(page, buf, &n))
        return check(ILLEGAL_REQUEST, 0x24); /* invalid field in CDB */
    if (pc == 1) /* changeable values: none */
        memset(buf + hdr + (dbd ? 0 : 8) + 2, 0, n - hdr - (dbd ? 0 : 8) - 2);
    if (ten) {
        buf[0] = (uint8_t)((n - 2) >> 8), buf[1] = (uint8_t)(n - 2);
        buf[3] = 0x80;                     /* write-protected */
        buf[7] = dbd ? 0 : 8;
    } else {
        buf[0] = (uint8_t)(n - 1);
        buf[2] = 0x80;
        buf[3] = dbd ? 0 : 8;
    }
    xfer.len = n < alloc ? n : alloc;
    xfer.in = malloc(xfer.len ? xfer.len : 1);
    memcpy(xfer.in, buf, xfer.len);
    return GOOD;
}

/* Run a command (its CDB, for logical unit lun): its status, and its data
 * in xfer. */
static uint8_t disk_command(const uint8_t *cdb, uint32_t cdblen, uint8_t lun)
{
    xfer_clear();
    uint8_t op = cdb[0];
    uint32_t need = op < 0x20 ? 6 : op < 0x60 ? 10 : 12;
    if (cdblen < need)
        return check(ILLEGAL_REQUEST, 0x20);
    if (op == 0x12) { /* INQUIRY, whichever unit */
        uint8_t inq[36] = { 0 };
        if (lun) {
            inq[0] = 0x7F; /* no device on this unit */
        } else {
            inq[2] = 2, inq[3] = 2, inq[4] = 31; /* SCSI-2, its format, 31 more bytes */
            memcpy(inq + 8, "BLUEBOX Macintosh HD    1.0 ", 28);
        }
        uint32_t alloc = cdb[4];
        xfer.len = alloc < sizeof inq ? alloc : sizeof inq;
        xfer.in = malloc(sizeof inq);
        memcpy(xfer.in, inq, sizeof inq);
        sense.key = NO_SENSE, sense.asc = 0;
        return GOOD;
    }
    if (op == 0x03) { /* REQUEST SENSE: the last command's, then cleared */
        uint8_t sd[18] = { 0x70, 0, sense.key, 0, 0, 0, 0, 10 };
        if (lun)
            sd[2] = ILLEGAL_REQUEST, sd[12] = 0x25;
        else
            sd[12] = sense.asc;
        uint32_t alloc = cdb[4] ? cdb[4] : 4;
        xfer.len = alloc < sizeof sd ? alloc : sizeof sd;
        xfer.in = malloc(sizeof sd);
        memcpy(xfer.in, sd, sizeof sd);
        sense.key = NO_SENSE, sense.asc = 0;
        return GOOD;
    }
    if (lun)
        return check(ILLEGAL_REQUEST, 0x25); /* logical unit not supported */
    sense.key = NO_SENSE, sense.asc = 0;
    switch (op) {
    case 0x00: /* TEST UNIT READY */
    case 0x1B: /* START STOP UNIT */
    case 0x1E: /* PREVENT ALLOW MEDIUM REMOVAL */
    case 0x2B: /* SEEK(10) */
    case 0x0B: /* SEEK(6) */
    case 0x35: /* SYNCHRONIZE CACHE */
        return GOOD;
    case 0x2F: { /* VERIFY(10): the blocks are there */
        uint32_t lba = (uint32_t)cdb[2] << 24 | (uint32_t)cdb[3] << 16 | (uint32_t)cdb[4] << 8 | cdb[5];
        uint32_t n = (uint32_t)cdb[7] << 8 | cdb[8];
        return (uint64_t)lba + n > DISK_BLOCKS ? check(ILLEGAL_REQUEST, 0x21) : GOOD;
    }
    case 0x1A: return mode_sense(cdb, false);
    case 0x5A: return mode_sense(cdb, true);
    case 0x25: /* READ CAPACITY: the last block and the block length */
        xfer.len = 8;
        xfer.in = malloc(8);
        put32(xfer.in, DISK_BLOCKS - 1);
        put32(xfer.in + 4, BLOCK);
        return GOOD;
    case 0x08: /* READ(6) */
    case 0x28: { /* READ(10) */
        uint32_t lba, n;
        if (op == 0x08) {
            lba = (uint32_t)(cdb[1] & 0x1F) << 16 | (uint32_t)cdb[2] << 8 | cdb[3];
            n = cdb[4] ? cdb[4] : 256;
        } else {
            lba = (uint32_t)cdb[2] << 24 | (uint32_t)cdb[3] << 16 | (uint32_t)cdb[4] << 8 | cdb[5];
            n = (uint32_t)cdb[7] << 8 | cdb[8];
        }
        if ((uint64_t)lba + n > DISK_BLOCKS)
            return check(ILLEGAL_REQUEST, 0x21); /* logical block address out of range */
        xfer.len = n * BLOCK;
        xfer.in = malloc(xfer.len ? xfer.len : 1);
        read_blocks(lba, n, xfer.in);
        return GOOD;
    }
    case 0x0A: case 0x2A: case 0x2E: /* WRITE(6), WRITE(10), WRITE AND VERIFY */
    case 0x04: case 0x07:            /* FORMAT UNIT, REASSIGN BLOCKS */
    case 0x15: case 0x55:            /* MODE SELECT */
        return check(DATA_PROTECT, 0x27); /* write protected */
    }
    return check(ILLEGAL_REQUEST, 0x20); /* invalid command operation code */
}

/* ---- The original SCSI Manager ---- */

enum {
    scCommErr = 2, scBadParmsErr = 4, scPhaseErr = 5, scCompareErr = 6, scMgrBusyErr = 7, scSequenceErr = 8,
};
/* Transfer instruction block opcodes. */
enum { scInc = 1, scNoInc = 2, scAdd = 3, scMove = 4, scLoop = 5, scNop = 6, scStop = 7, scComp = 8 };

static bool arbitrated; /* SCSIGet has the bus */
static enum { FREE, COMMAND, DATA, STATUS } phase; /* the disk, once selected */

int16_t tb_SCSIReset(void)
{
    arbitrated = false;
    phase = FREE;
    xfer_clear();
    return noErr;
}

int16_t tb_SCSIGet(void)
{
    if (arbitrated || phase != FREE)
        return scMgrBusyErr;
    arbitrated = true;
    return noErr;
}

/* The disk answers selection; no other target does, and the bus goes free
 * again. */
static int16_t select_target(int16_t id)
{
    if (!arbitrated)
        return scSequenceErr;
    arbitrated = false;
    if (id < 0 || id > 7)
        return scBadParmsErr;
    if (id != DISK_ID)
        return scCommErr;
    phase = COMMAND;
    return noErr;
}

int16_t tb_SCSISelect(int16_t targetID) { return select_target(targetID); }
int16_t tb_SCSISelAtn(int16_t targetID) { return select_target(targetID); }

int16_t tb_SCSICmd(gaddr buffer, int16_t count)
{
    if (phase != COMMAND)
        return phase == FREE ? scSequenceErr : scPhaseErr;
    uint8_t cdb[16] = { 0 };
    uint32_t n = count > 0 && count <= 16 ? (uint32_t)count : 0;
    if (n)
        gget(cdb, buffer, n);
    xfer.status = disk_command(cdb, n, cdb[1] >> 5); /* the LUN in byte 1's top bits */
    if (xfer.status == GOOD && (cdb[0] == 0x0A || cdb[0] == 0x2A))
        xfer.out = true;
    phase = xfer.status == GOOD && xfer.len ? DATA : STATUS;
    return noErr;
}

/* Move the command's data as a TIB says, into memory (in) or from it. */
static int16_t run_tib(gaddr tib, bool in)
{
    for (unsigned steps = 0; steps < 10000000; steps++) {
        uint16_t op = rd16(tib);
        uint32_t p1 = rd32(tib + 2), p2 = rd32(tib + 6);
        switch (op) {
        case scInc:
        case scNoInc:
        case scComp: {
            uint32_t left = xfer.len - xfer.pos, n = p2 < left ? p2 : left;
            if (in && op == scComp) {
                uint8_t tmp[256];
                for (uint32_t k = 0; k < n; k += sizeof tmp) {
                    uint32_t m = n - k < sizeof tmp ? n - k : sizeof tmp;
                    gget(tmp, p1 + k, m);
                    if (memcmp(tmp, xfer.in + xfer.pos + k, m))
                        return scCompareErr;
                }
            } else if (in) {
                gput(p1, xfer.in + xfer.pos, n);
            }
            xfer.pos += n;
            if (op == scInc)
                wr32(tib + 2, p1 + n);
            if (xfer.pos == xfer.len)
                phase = STATUS;
            if (n < p2)
                return scPhaseErr; /* the target went on to its status */
            break;
        }
        case scAdd: wr32(p1, rd32(p1) + p2); break;
        case scMove: wr32(p2, rd32(p1)); break;
        case scLoop:
            wr32(tib + 6, p2 - 1);
            if (p2 > 1) {
                tib += (int32_t)p1;
                continue;
            }
            break;
        case scNop: break;
        case scStop: return noErr;
        default: return scBadParmsErr;
        }
        tib += 10;
    }
    return scBadParmsErr;
}

static int16_t data_phase(gaddr tib, bool in)
{
    if (phase == FREE || phase == COMMAND)
        return phase == FREE ? scSequenceErr : scPhaseErr;
    if (phase != DATA || xfer.out == in)
        return scPhaseErr;
    return run_tib(tib, in);
}

int16_t tb_SCSIRead(gaddr tibPtr) { return data_phase(tibPtr, true); }
int16_t tb_SCSIRBlind(gaddr tibPtr) { return data_phase(tibPtr, true); }
int16_t tb_SCSIWrite(gaddr tibPtr) { return data_phase(tibPtr, false); }
int16_t tb_SCSIWBlind(gaddr tibPtr) { return data_phase(tibPtr, false); }

/* The message after the status: COMMAND COMPLETE. */
int16_t tb_SCSIMsgIn(gaddr message)
{
    if (phase == FREE)
        return scSequenceErr;
    if (message)
        wr16(message, 0);
    return noErr;
}

int16_t tb_SCSIMsgOut(int16_t message) { return phase == FREE ? scSequenceErr : noErr; }

/* The status and message bytes; the bus is free after. */
int16_t tb_SCSIComplete(gaddr stat, gaddr message, uint32_t wait)
{
    arbitrated = false;
    if (phase == FREE)
        return scSequenceErr;
    if (stat)
        wr16(stat, phase == COMMAND ? check(ILLEGAL_REQUEST, 0) : xfer.status);
    if (message)
        wr16(message, 0); /* COMMAND COMPLETE */
    phase = FREE;
    xfer_clear();
    return noErr;
}

/* The bus's lines: BSY and REQ while the disk is connected, with C/D, I/O
 * and MSG for its phase; none when the bus is free. */
int16_t tb_SCSIStat(void)
{
    enum { IO = 1 << 2, CD = 1 << 3, BSY = 1 << 6, REQ = 1 << 5 };
    switch (phase) {
    case COMMAND: return BSY | REQ | CD;
    case DATA: return BSY | REQ | (xfer.out ? 0 : IO);
    case STATUS: return BSY | REQ | CD | IO;
    default: return 0;
    }
}

/* ---- SCSI Manager 4.3 ---- */

enum {
    scsiRequestAborted = -7934, scsiUnableToAbort = -7933, scsiNonZeroStatus = -7932, scsiUnableToTerminate = -7927,
    scsiDataRunError = -7918,
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
    phase = FREE;
    xfer_clear();
    sense.key = NO_SENSE, sense.asc = 0;
    io_pb = io_buf = 0;
    /* The disk's driver has its cross-reference, as Apple's did. */
    xrefs[0].device = DISK_ID << 8;
    xrefs[0].refnum = (int16_t)(-1 - DISK_DRIVER_UNIT);
    xrefs[0].flags = 0;
    nxrefs = 1;
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
    put_name(pb + 136, "Blue Box");      /* scsiControllerType */
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

/* SCSI_IO: scsiFlags 20, scsiResultFlags 36, scsiDataPtr 40, scsiDataLength
 * 44, scsiSensePtr 48, scsiSenseLength 52, scsiCDBLength 53,
 * scsiSGListCount 54, scsiSCSIstatus 60, scsiSenseResidual 61,
 * scsiDataResidual 64, scsiCDB 68, scsiDataType 102. */
enum {
    io_flags = 20, io_result_flags = 36, io_data = 40, io_length = 44, io_sense = 48, io_sense_len = 52,
    io_cdb_len = 53, io_sg_count = 54, io_status = 60, io_sense_resid = 61, io_resid = 64, io_cdb = 68,
    io_data_type = 102,
};
enum { scsiDirectionMask = 0xC0000000u, scsiDirectionIn = 0x40000000u, scsiDisableAutosense = 0x20000000u,
       scsiCDBIsPointer = 0x01000000u };
enum { scsiAutosenseValid = 0x0002 };
enum { scsiDataBuffer = 0, scsiDataTIB = 1, scsiDataSG = 2 };

/* SCSIExecIO on the disk: the command, its data to or from the buffer,
 * TIB or scatter/gather list, the status, and the sense when it failed. */
static int16_t exec_io(gaddr pb)
{
    uint32_t flags = rd32(pb + io_flags), len = rd32(pb + io_length);
    uint32_t cdblen = rd8(pb + io_cdb_len);
    uint8_t cdb[16] = { 0 };
    if (cdblen > 16)
        cdblen = 16;
    gget(cdb, flags & scsiCDBIsPointer ? rd32(pb + io_cdb) : pb + io_cdb, cdblen);
    uint8_t status = disk_command(cdb, cdblen, rd8(pb + sh_device + 3));
    bool in = (flags & scsiDirectionMask) == scsiDirectionIn;
    uint32_t moved = 0;
    int16_t r = noErr;
    if (status == GOOD && in && xfer.len) {
        gaddr data = rd32(pb + io_data);
        switch (rd8(pb + io_data_type)) {
        case scsiDataTIB:
            xfer.pos = 0;
            r = run_tib(data, true);
            moved = xfer.pos;
            if (r == scPhaseErr)
                r = noErr; /* the target had less than the TIB asked for: a residual */
            else if (r)
                r = scsiDataRunError;
            break;
        case scsiDataSG:
            for (uint32_t i = 0, n = rd16(pb + io_sg_count); i < n && moved < xfer.len && moved < len; i++) {
                uint32_t a = rd32(data + i * 8), l = rd32(data + i * 8 + 4), m = xfer.len - moved;
                if (l > m)
                    l = m;
                gput(a, xfer.in + moved, l);
                moved += l;
            }
            break;
        default:
            moved = xfer.len < len ? xfer.len : len;
            gput(data, xfer.in, moved);
            break;
        }
        if (xfer.len > len && rd8(pb + io_data_type) != scsiDataTIB)
            r = scsiDataRunError; /* more than the buffer holds */
    } else if (status == GOOD && xfer.len && !in) {
        r = scsiDataRunError; /* data the caller didn't ask for */
    }
    wr32(pb + io_resid, len - (moved < len ? moved : len));
    wr8(pb + io_status, status);
    xfer_clear();
    if (status == GOOD)
        return r;
    if (!(flags & scsiDisableAutosense) && rd32(pb + io_sense)) {
        uint8_t sd[18] = { 0x70, 0, sense.key, 0, 0, 0, 0, 10 };
        sd[12] = sense.asc;
        uint32_t want = rd8(pb + io_sense_len), n = want < sizeof sd ? want : sizeof sd;
        gput(rd32(pb + io_sense), sd, n);
        wr8(pb + io_sense_resid, (uint8_t)(want - n));
        wr16(pb + io_result_flags, rd16(pb + io_result_flags) | scsiAutosenseValid);
        sense.key = NO_SENSE, sense.asc = 0;
    }
    return scsiNonZeroStatus;
}

static int16_t action(gaddr pb)
{
    uint8_t fn = rd8(pb + 8);
    int16_t r;
    switch (fn) {
    case 0x00: r = noErr; break; /* SCSINop */
    case 0x01: /* SCSIExecIO */
        r = check_device(pb);
        if (r == noErr && rd8(pb + sh_device + 2) == DISK_ID) {
            r = exec_io(pb);
            break;
        }
        if (r == noErr)
            r = scsiSelectTimeout;
        wr32(pb + 64, rd32(pb + 44)); /* scsiDataResidual: scsiDataLength, nothing moved */
        break;
    case 0x03: r = bus_inquiry(pb); break;
    case 0x04: r = rd8(pb + sh_device + 1) ? scsiBusInvalid : noErr; break; /* SCSIReleaseQ: nothing frozen */
    case 0x10: r = scsiUnableToAbort; break;                                /* SCSIAbortCommand: nothing queued */
    case 0x11: r = rd8(pb + sh_device + 1) ? scsiBusInvalid : noErr; break; /* SCSIResetBus */
    case 0x12: /* SCSIResetDevice */
        r = check_device(pb) ? check_device(pb) : rd8(pb + sh_device + 2) == DISK_ID ? noErr : scsiSelectTimeout;
        break;
    case 0x13: r = scsiUnableToTerminate; break;                            /* SCSITerminateIO */
    case 0x80: { /* SCSIGetVirtualIDInfo: an old-API ID is on bus 0, and nothing is there */
        uint16_t id = rd16(pb + 36);
        wr8(pb + sh_device + 1, 0);
        wr8(pb + sh_device + 2, (uint8_t)id);
        wr8(pb + sh_device + 3, 0);
        wr8(pb + 38, id == HOST_ID || id == DISK_ID); /* scsiExists */
        r = id > 7 ? scsiTIDInvalid : noErr;
        break;
    }
    case 0x82: /* SCSILoadDriver: the disk's is loaded already */
        r = check_device(pb) ? check_device(pb) : rd8(pb + sh_device + 2) == DISK_ID ? noErr : scsiDeviceNotThere;
        break;
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
