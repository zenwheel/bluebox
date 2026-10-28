/*
 * The main volume's disk driver (reference number -33, SCSI ID 0's, the drive the
 * Macintosh HD volume is on): what the Finder asks a disk's driver for to
 * draw it (the drive and media icons, the drive's kind) and the drive's
 * status.  The mounted volumes' drives are the CD driver's; it answers
 * the same icon calls through disk_icon_control.
 */
#include "tb_internal.h"

enum { kDriveIcon = 21, kMediaIcon = 22, kDriveInfo = 23, drvStsCode = 8 };
enum { controlErr_ = -17, statusErr_ = -18, nsDrvErr_ = -56 };
enum { DISK_UNIT = DISK_DRIVER_UNIT };

/* The icon (ICN#, data and mask) a driver hands back: a pointer to its 256
 * bytes, which stay put.  The System's hard disk icon. */
static gaddr hd_icon;

static gaddr icon_bytes(void)
{
    if (!hd_icon) {
        gaddr h = tb_GetResource((int32_t)fourcc("ICN#"), -3995);
        if (!h || !rd32(h) || tb_GetHandleSize(h) < 256)
            return 0;
        hd_icon = mm_new_ptr_in(rd32(LM_SysZone), 256, false);
        if (hd_icon)
            gmemcpy(hd_icon, rd32(h), 256);
    }
    return hd_icon;
}

void disk_driver_reset(void) { hd_icon = 0; }

/* kDriveIcon, kMediaIcon and kDriveInfo for a drive; -1 when the call is
 * another. */
int16_t disk_icon_control(gaddr pb, bool removable)
{
    gaddr cs = pb + CntrlParam_csParam_off;
    switch ((int16_t)rd16(pb + CntrlParam_csCode_off)) {
    case kDriveIcon:
    case kMediaIcon: {
        gaddr p = icon_bytes();
        if (!p)
            return controlErr_;
        wr32(cs, p);
        return noErr;
    }
    case kDriveInfo:
        /* The drive's kind in the low byte: 1 a SCSI hard disk, 7 a CD-ROM
         * drive; internal (the location byte 0) and fixed unless removable. */
        wr32(cs, removable ? 0x00000207u : 0x00000001u);
        return noErr;
    }
    return -1;
}

/* The drive's status (DrvSts) from its drive queue element. */
int16_t disk_drive_status(gaddr pb)
{
    if ((int16_t)rd16(pb + CntrlParam_csCode_off) != drvStsCode)
        return statusErr_;
    int16_t drive = (int16_t)rd16(pb + IOParam_ioVRefNum_off);
    for (gaddr e = rd32(LM_DrvQHdr + 2); e; e = rd32(e)) {
        if ((int16_t)rd16(e + DrvQEl_dQDrive_off) != drive)
            continue;
        gaddr st = pb + CntrlParam_csParam_off;
        gmemset(st, 0, DrvSts_SIZEOF);
        for (uint32_t i = 0; i < 4; i++) /* writeProt, diskInPlace, installed, sides */
            wr8(st + DrvSts_writeProt_off + i, rd8(e - 4 + i));
        gmemcpy(st + DrvSts_qLink_off, e, DrvSts_twoSideFmt_off - DrvSts_qLink_off);
        return noErr;
    }
    return nsDrvErr_;
}

static int16_t disk_control(gaddr pb, bool async)
{
    int16_t r = disk_icon_control(pb, false);
    return r == -1 ? controlErr_ : r;
}

static int16_t disk_status(gaddr pb, bool async) { return disk_drive_status(pb); }

const native_driver disk_driver = { ".BlueBoxDisk", DISK_UNIT, NULL, NULL, disk_control, disk_status, NULL, NULL, NULL };
