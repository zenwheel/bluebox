/*
 * The Device Manager's native drivers: drivers of our own, opened by name
 * (".IPP") like the ROM's and the System's, with negative reference
 * numbers (-1 - unit) that PBControl, PBStatus, PBRead, PBWrite, PBClose
 * and PBKillIO route here.  A driver answers a call at once, or, when it
 * cannot yet and the call was asynchronous, leaves ioResult at 1 (in
 * progress) and completes the call itself later.
 */
#include <ctype.h>
#include <string.h>

#include "tb_internal.h"

static const native_driver *const drivers[] = {
    &sound_driver, &mactcp_driver, &mpp_driver, &atp_driver, &xpp_driver, &dsp_driver, &serial_drivers[0], &serial_drivers[1], &serial_drivers[2], &serial_drivers[3],
};
enum { NDRIVERS = sizeof drivers / sizeof drivers[0] };

static const native_driver *driver_for(int16_t refnum)
{
    if (refnum >= 0)
        return NULL;
    int16_t unit = (int16_t)(-1 - refnum);
    for (int i = 0; i < NDRIVERS; i++)
        if (drivers[i]->unit == unit)
            return drivers[i];
    return NULL;
}

/* The unit table (UTableBase, UnitNtryCnt): a device control entry for
 * each driver, whose dCtlDriver points at a driver header with the name.
 * Programs look there to see whether a driver is installed (MacTCP's
 * .IPP, the serial drivers) and GetDCtlEntry reads it; the calls
 * themselves still come here by reference number. */
enum { UNITS = 64, dOpened = 0x20 };
static gaddr dce[NDRIVERS];

void drivers_reset(void)
{
    gaddr table = tb_NewPtr(UNITS * 4, true, true);
    wr32(LM_UTableBase, table);
    wr16(LM_UnitNtryCnt, UNITS);
    for (int i = 0; i < NDRIVERS; i++) {
        const native_driver *d = drivers[i];
        size_t n = strlen(d->name);
        gaddr hdr = tb_NewPtr((int32_t)(18 + 1 + n), true, true);
        uint16_t flags = (uint16_t)((d->read ? 0x0100 : 0) | (d->write ? 0x0200 : 0) | (d->control ? 0x0400 : 0) |
                                    (d->status ? 0x0800 : 0));
        wr16(hdr, flags); /* drvrFlags; its routines' offsets stay 0: calls come to us */
        wr8(hdr + 18, (uint8_t)n);
        for (size_t k = 0; k < n; k++)
            wr8(hdr + 19 + (uint32_t)k, (uint8_t)d->name[k]);
        gaddr h = tb_NewHandle(DCtlEntry_SIZEOF, true, true);
        gaddr e = rd32(h);
        wr32(e + DCtlEntry_dCtlDriver_off, hdr);
        wr16(e + DCtlEntry_dCtlFlags_off, flags); /* a ROM-style driver: a pointer, not RAM-based */
        wr16(e + DCtlEntry_dCtlRefNum_off, (uint16_t)(-1 - d->unit));
        dce[i] = e;
        if (d->unit >= 0 && d->unit < UNITS)
            wr32(table + 4 * (uint32_t)d->unit, h);
    }
}

static void set_opened(const native_driver *d, bool open)
{
    for (int i = 0; i < NDRIVERS; i++)
        if (drivers[i] == d && dce[i]) {
            uint16_t f = rd16(dce[i] + DCtlEntry_dCtlFlags_off);
            wr16(dce[i] + DCtlEntry_dCtlFlags_off, (uint16_t)(open ? f | dOpened : f & ~dOpened));
        }
}

bool driver_open(gaddr pb, int16_t *err)
{
    gaddr namep = rd32(pb + IOParam_ioNamePtr_off);
    if (!namep)
        return false;
    int n = rd8(namep);
    for (int i = 0; i < NDRIVERS; i++) {
        const char *want = drivers[i]->name;
        if ((int)strlen(want) != n)
            continue;
        bool same = true;
        for (int k = 0; k < n && same; k++)
            same = tolower(rd8(namep + 1 + (uint32_t)k)) == tolower((unsigned char)want[k]);
        if (!same)
            continue;
        wr16(pb + IOParam_ioRefNum_off, (uint16_t)(-1 - drivers[i]->unit));
        *err = drivers[i]->open ? drivers[i]->open(pb) : noErr;
        if (*err == noErr)
            set_opened(drivers[i], true);
        return true;
    }
    return false;
}

bool driver_refnum(int16_t refnum) { return driver_for(refnum) != NULL; }

static int16_t call(const native_driver *d, gaddr pb, bool async, int kind)
{
    switch (kind) {
    case DRIVER_CONTROL: return d->control ? d->control(pb, async) : -17;  /* controlErr */
    case DRIVER_STATUS: return d->status ? d->status(pb, async) : -18;     /* statusErr */
    case DRIVER_READ: return d->read ? d->read(pb, async) : -19;           /* readErr */
    case DRIVER_WRITE: return d->write ? d->write(pb, async) : -20;        /* writErr */
    case DRIVER_CLOSE: return d->close ? d->close(pb) : noErr;
    case DRIVER_KILLIO: return d->kill_io ? d->kill_io(pb) : noErr;
    default: return -51;
    }
}

int16_t driver_call(gaddr pb, bool async, int kind)
{
    const native_driver *d = driver_for((int16_t)rd16(pb + IOParam_ioRefNum_off));
    if (!d)
        return -51; /* rfNumErr */
    int16_t r = call(d, pb, async, kind);
    if (kind == DRIVER_CLOSE && r == noErr)
        set_opened(d, false);
    if (kind == DRIVER_CONTROL || kind == DRIVER_STATUS)
        LOGF(LOG_TRAPS, "  %s csCode %d -> %d\n", d->name, (int16_t)rd16(pb + CntrlParam_csCode_off), r);
    return r;
}

/* DrvrInstall: a program's own driver gets a device control entry in the
 * unit table, so GetDCtlEntry and programs walking the table find it.
 * Its routines aren't called (the Device Manager answers only for ours),
 * and a unit one of ours has keeps ours (the installation is accepted, as
 * it was before there were entries). */
int16_t driver_install(gaddr drvr, int16_t refnum)
{
    int16_t unit = (int16_t)(-1 - refnum);
    gaddr table = rd32(LM_UTableBase);
    if (refnum >= 0 || !table || unit >= (int16_t)rd16(LM_UnitNtryCnt))
        return -21; /* badUnitErr */
    if (driver_for(refnum))
        return noErr;
    gaddr h = tb_NewHandle(DCtlEntry_SIZEOF, true, true);
    if (!h)
        return memFullErr;
    gaddr e = rd32(h);
    wr32(e + DCtlEntry_dCtlDriver_off, drvr);
    wr16(e + DCtlEntry_dCtlFlags_off, drvr ? rd16(drvr) : 0);
    wr16(e + DCtlEntry_dCtlRefNum_off, (uint16_t)refnum);
    wr32(table + 4 * (uint32_t)unit, h);
    return noErr;
}
