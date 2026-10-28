/*
 * The Sony driver (.Sony, reference number -5) for a raw floppy disk image.
 *
 * Key-disk copy protection reads the original floppy's sectors itself: it
 * asks the Sony driver for blocks by offset, for the 12 tag bytes that go
 * with each (SetTagBuffer, csCode 8), and for the drive's status.  An image
 * given to the run (a plain sector dump of 400K, 720K, 800K or 1440K, or a
 * Disk Copy 4.2 image with its tags) is what drive 1 holds: the same disk
 * answers reads at drive number 1 and at the drive of its own in the drive
 * queue, so a program that looks the floppy drive up or just takes drive 1
 * finds it.  Writes change the image in memory and nothing else.
 *
 * Without an image the driver is the drive of the volumes Blue Box mounts:
 * nothing to read raw, so its reads say the drive is offline.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tb_internal.h"

enum { SECTOR = 512, TAG = 12 };
enum { offLinErr = -65, nsDrvErr = -56, controlErr = -17, statusErr = -18, wPrErr = -44 };

static struct {
    uint8_t *data;
    size_t len;
    uint8_t *tags; /* TAG bytes a sector, or NULL: all zero */
    bool present;  /* the disk is in the drive */
    bool locked;
    gaddr tag_buffer;
    uint32_t mark;
    int16_t drive; /* the drive number of the queue element made for it */
} disk;

static uint32_t be32_at(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

bool machine_set_floppy(const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "cannot open %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = n > 0 ? malloc((size_t)n) : NULL;
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        snprintf(err, errlen, "cannot read %s", path);
        return false;
    }
    fclose(f);
    free(disk.data);
    free(disk.tags);
    memset(&disk, 0, sizeof disk);
    /* Disk Copy 4.2: a 84-byte header (name, data size, tag size, checksums,
     * disk and format bytes, 0x0100), then the data and the tags. */
    if (n > 84 && buf[0] < 64 && buf[82] == 0x01 && buf[83] == 0x00 && be32_at(buf + 64) % SECTOR == 0 &&
        (size_t)84 + be32_at(buf + 64) + be32_at(buf + 68) <= (size_t)n) {
        uint32_t dsize = be32_at(buf + 64), tsize = be32_at(buf + 68);
        disk.data = malloc(dsize);
        memcpy(disk.data, buf + 84, dsize);
        disk.len = dsize;
        if (tsize >= (dsize / SECTOR) * TAG) {
            disk.tags = malloc(tsize);
            memcpy(disk.tags, buf + 84 + dsize, tsize);
        }
        free(buf);
    } else if (n % SECTOR == 0 && n >= 80 * SECTOR) {
        disk.data = buf;
        disk.len = (size_t)n;
    } else {
        free(buf);
        snprintf(err, errlen, "%s is not a floppy disk image", path);
        return false;
    }
    disk.present = true;
    return true;
}

bool sony_has_disk(void) { return disk.data != NULL; }
uint32_t sony_disk_size(void) { return (uint32_t)disk.len; }
/* The tag bytes of a sector as the driver gives them (tests). */
bool sony_sector_tags(uint32_t sector, uint8_t out[TAG])
{
    if (!disk.data || (size_t)(sector + 1) * SECTOR > disk.len)
        return false;
    if (disk.tags)
        memcpy(out, disk.tags + (size_t)sector * TAG, TAG);
    else
        memset(out, 0, TAG);
    return true;
}

void sony_reset(void)
{
    free(disk.data);
    free(disk.tags);
    memset(&disk, 0, sizeof disk);
}

/* A drive queue element for the disk, after the volumes' (filemgr.c). */
void sony_install_drive(gaddr zone, int16_t drive)
{
    if (!disk.data)
        return;
    gaddr dq = mm_new_ptr_in(zone, 4 + DrvQEl_SIZEOF, true);
    if (!dq)
        return;
    disk.drive = drive;
    wr8(dq, disk.locked ? 0xFF : 0);                /* write-protected */
    wr8(dq + 1, 1);                                 /* a disk in place */
    wr8(dq + 2, 1);                                 /* installed */
    wr8(dq + 3, disk.len > 409600 ? 2 : 1);         /* sides */
    gaddr e = dq + 4;
    wr16(e + DrvQEl_qType_off, 0);
    wr16(e + DrvQEl_dQDrive_off, (uint16_t)drive);
    wr16(e + DrvQEl_dQRefNum_off, (uint16_t)-5);
    wr16(e + DrvQEl_dQFSID_off, 0);
    uint32_t blocks = (uint32_t)(disk.len / SECTOR);
    wr16(e + DrvQEl_dQDrvSz_off, (uint16_t)blocks);
    wr16(e + DrvQEl_dQDrvSz2_off, (uint16_t)(blocks >> 16));
    if (rd32(LM_DrvQHdr + 6))
        wr32(rd32(LM_DrvQHdr + 6), e);
    else
        wr32(LM_DrvQHdr + 2, e);
    wr32(LM_DrvQHdr + 6, e);
}

/* Is the call for the disk: drive 1, or the drive queue element made for it. */
static bool ours(gaddr pb)
{
    int16_t v = (int16_t)rd16(pb + IOParam_ioVRefNum_off);
    return disk.data && (v == 1 || (disk.drive && v == disk.drive));
}

static int16_t sony_rw(gaddr pb, bool write)
{
    if (!ours(pb))
        return disk.data ? nsDrvErr : offLinErr;
    if (!disk.present)
        return offLinErr;
    if (write && disk.locked)
        return wPrErr;
    gaddr buf = rd32(pb + IOParam_ioBuffer_off);
    uint32_t count = rd32(pb + IOParam_ioReqCount_off), off;
    int mode = rd16(pb + IOParam_ioPosMode_off) & 3;
    off = mode == 0 ? disk.mark : rd32(pb + IOParam_ioPosOffset_off); /* fsAtMark, or from the start */
    if (mode == 3) /* fsFromLEOF */
        off = (uint32_t)disk.len + rd32(pb + IOParam_ioPosOffset_off);
    if (count % SECTOR || off % SECTOR)
        return paramErr;
    if ((uint64_t)off + count > disk.len) {
        wr32(pb + IOParam_ioActCount_off, 0);
        return paramErr;
    }
    LOGF(LOG_TRAPS, "  .Sony %s %u bytes at %u\n", write ? "write" : "read", count, off);
    if (write) {
        gget(disk.data + off, buf, count);
    } else {
        gput(buf, disk.data + off, count);
    }
    /* the tag bytes of the last sector go to the tag buffer, and from it on a write */
    uint32_t last = (off + count) / SECTOR - 1;
    if (count && disk.tag_buffer) {
        if (write) {
            if (!disk.tags)
                disk.tags = calloc(disk.len / SECTOR, TAG);
            gget(disk.tags + (size_t)last * TAG, disk.tag_buffer, TAG);
        } else if (disk.tags) {
            gput(disk.tag_buffer, disk.tags + (size_t)last * TAG, TAG);
        } else {
            gmemset(disk.tag_buffer, 0, TAG);
        }
    }
    disk.mark = off + count;
    wr32(pb + IOParam_ioActCount_off, count);
    wr32(pb + IOParam_ioPosOffset_off, disk.mark);
    return noErr;
}

static int16_t sony_read(gaddr pb, bool async) { return sony_rw(pb, false); }
static int16_t sony_write(gaddr pb, bool async) { return sony_rw(pb, true); }

enum { csVerify = 5, csFormat = 6, csEject = 7, csSetTag = 8, csTrackCache = 9, csDriveIcon = 21, csMediaIcon = 22,
       csDriveInfo = 23 };

static int16_t sony_control(gaddr pb, bool async)
{
    int16_t cs = (int16_t)rd16(pb + CntrlParam_csCode_off);
    if (cs == csSetTag) { /* the buffer for the tag bytes, whichever drive */
        disk.tag_buffer = rd32(pb + CntrlParam_csParam_off);
        return noErr;
    }
    if (!ours(pb))
        return disk.data ? nsDrvErr : offLinErr;
    switch (cs) {
    case csVerify: return disk.present ? noErr : offLinErr;
    case csFormat: return disk.locked ? wPrErr : controlErr; /* a disk image is not formatted again */
    case csEject: /* the disk leaves the drive */
        disk.present = false;
        return noErr;
    case csTrackCache: return noErr;
    case csDriveInfo: /* internal floppy, 800K-capable when the disk is double-sided */
        wr32(pb + CntrlParam_csParam_off, disk.len > 409600 ? 0x00000001u : 0);
        return noErr;
    }
    return controlErr;
}

int16_t driver_drive_status(gaddr pb);

static int16_t sony_status(gaddr pb, bool async)
{
    int16_t cs = (int16_t)rd16(pb + CntrlParam_csCode_off);
    if (!ours(pb))
        return cs == 8 ? driver_drive_status(pb) : statusErr; /* the volumes' drives, as the drive queue has them */
    switch (cs) {
    case 6: { /* the formats the drive writes: a list of (capacity in blocks, flags) */
        gaddr p = pb + CntrlParam_csParam_off;
        wr16(p, 1);                                                  /* one format: the disk's */
        wr32(p + 2, (uint32_t)(disk.len / SECTOR));                  /* blocks */
        wr16(p + 6, disk.len > 409600 ? 0x0002 : 0x0001);            /* sides flag */
        return noErr;
    }
    case 8: { /* drive status */
        gaddr st = pb + CntrlParam_csParam_off;
        gmemset(st, 0, DrvSts_SIZEOF);
        wr8(st + DrvSts_writeProt_off, disk.locked ? 0xFF : 0);
        wr8(st + DrvSts_diskInPlace_off, disk.present ? 1 : 0);
        wr8(st + DrvSts_installed_off, 1);
        wr8(st + DrvSts_sides_off, disk.len > 409600 ? 0xFF : 0);   /* two-sided */
        wr16(st + DrvSts_dQDrive_off, (uint16_t)(disk.drive ? disk.drive : 1));
        wr16(st + DrvSts_dQRefNum_off, (uint16_t)-5);
        wr8(st + DrvSts_twoSideFmt_off, disk.len > 409600 ? 0xFF : 0);
        return noErr;
    }
    }
    return statusErr;
}

static int16_t sony_kill(gaddr pb) { return noErr; }

const native_driver sony_driver = { ".Sony", 4, NULL, NULL, sony_control, sony_status, sony_read, sony_write, sony_kill };
