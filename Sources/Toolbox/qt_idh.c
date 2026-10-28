/*
 * The FireWire DV isochronous data handler ('ihlr' 'dv  '), as QuickTime
 * 4's DV support registered it.  Blue Box has no FireWire bus, so it
 * answers as on a Mac with no DV device attached: the device list holds
 * no devices, configurations name none, opening and I/O fail with the
 * handler's documented errors, and notifications are kept but never come.
 */
#include <string.h>

#include "qt_atoms.h"
#include "qt_comp.h"
#include "qt_stream.h"
#include "tb_internal.h"

enum {
    kIDHErrInvalidDeviceID = -14102, kIDHErrDeviceNotOpened = -14105, kIDHErrDeviceNotConfigured = -14109,
    kIDHInterfaceVersion1 = 1, paramErrI = -50, MAX_NOTES = 16,
};

typedef struct {
    struct {
        uint32_t id, device, events;
        gaddr proc, refcon;
    } notes[MAX_NOTES];
    uint32_t next_note;
} idh;

/* A device list with no devices in it: 'dlst' holding the interface version. */
static gaddr device_list(void)
{
    gaddr cont = qt_atoms_new();
    if (!cont)
        return 0;
    int16_t err;
    uint32_t list = qt_atoms_insert(cont, 0, fourcc("dlst"), 1, 0, NULL, 0, &err);
    uint8_t v[4] = { 0, 0, 0, kIDHInterfaceVersion1 };
    if (list)
        qt_atoms_insert(cont, list, fourcc("iver"), 1, 0, v, 4, &err);
    return cont;
}

static int32_t idh_call(qtc_call *k)
{
    idh *d = k->self;
    if (!k->call) /* the standard selectors */
        return k->what == kQTCOpen || k->what == kQTCClose ? 0 : qtcBadSelector;
    const char *name = k->call->name;
    if (!strcmp(name, "IDHGetDeviceList") || !strcmp(name, "IDHUpdateDeviceList")) {
        if (!k->a[0])
            return paramErrI;
        gaddr list = device_list();
        if (!list)
            return -108;
        if (!strcmp(name, "IDHUpdateDeviceList") && rd32(k->a[0]))
            tb_DisposeHandle(rd32(k->a[0]));
        wr32(k->a[0], list);
        return 0;
    }
    if (!strcmp(name, "IDHGetDeviceConfiguration")) {
        if (k->a[0]) {
            wr32(k->a[0], 0);
            wr32(k->a[0] + 4, 0);
        }
        return kIDHErrDeviceNotConfigured;
    }
    if (!strcmp(name, "IDHSetDeviceConfiguration") || !strcmp(name, "IDHGetDeviceStatus"))
        return kIDHErrInvalidDeviceID; /* no configuration names a device here */
    if (!strcmp(name, "IDHGetDeviceClock") || !strcmp(name, "IDHGetDeviceControl")) {
        if (k->a[0])
            wr32(k->a[0], 0);
        return kIDHErrDeviceNotConfigured;
    }
    if (!strcmp(name, "IDHOpenDevice"))
        return kIDHErrDeviceNotConfigured;
    if (!strcmp(name, "IDHCloseDevice") || !strcmp(name, "IDHRead") || !strcmp(name, "IDHWrite") ||
        !strcmp(name, "IDHReleaseBuffer") || !strcmp(name, "IDHCancelPendingIO"))
        return kIDHErrDeviceNotOpened;
    if (!strcmp(name, "IDHNewNotification")) { /* deviceID, proc, userData, notificationID* */
        if (!k->a[3])
            return paramErrI;
        for (int i = 0; i < MAX_NOTES; i++)
            if (!d->notes[i].id) {
                d->notes[i].id = ++d->next_note;
                d->notes[i].device = k->a[0];
                d->notes[i].proc = k->a[1];
                d->notes[i].refcon = k->a[2];
                d->notes[i].events = 0;
                wr32(k->a[3], d->notes[i].id);
                return 0;
            }
        return -108;
    }
    if (!strcmp(name, "IDHNotifyMeWhen") || !strcmp(name, "IDHCancelNotification") ||
        !strcmp(name, "IDHDisposeNotification")) {
        for (int i = 0; i < MAX_NOTES; i++)
            if (d->notes[i].id && d->notes[i].id == k->a[0]) {
                if (!strcmp(name, "IDHNotifyMeWhen"))
                    d->notes[i].events = k->a[1];
                else if (!strcmp(name, "IDHCancelNotification"))
                    d->notes[i].events = 0;
                else
                    memset(&d->notes[i], 0, sizeof d->notes[i]);
                return 0;
            }
        return paramErrI;
    }
    return qtcBadSelector;
}

static qtc_class dv_handler = {
    0x69686C72 /* 'ihlr' */, 0x64762020 /* 'dv  ' */, 0x6170706C /* 'appl' */, 0, "FireWire DV", NULL,
    0x00010000, { "IDH" }, sizeof(idh), idh_call, 0,
};

void qt_idh_register(void) { qtc_register(&dv_handler); }
