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
    &mactcp_driver, &serial_drivers[0], &serial_drivers[1], &serial_drivers[2], &serial_drivers[3],
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
        return true;
    }
    return false;
}

bool driver_refnum(int16_t refnum) { return driver_for(refnum) != NULL; }

int16_t driver_call(gaddr pb, bool async, int kind)
{
    const native_driver *d = driver_for((int16_t)rd16(pb + IOParam_ioRefNum_off));
    if (!d)
        return -51; /* rfNumErr */
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
