/*
 * The Power Manager and the Start Manager's parameter RAM.
 *
 * The machine Blue Box presents is a desktop (a Quadra 800, or a G3 for
 * PowerPC programs) and Gestalt has no 'powr': there is no battery, no
 * modem to power, no wakeup timer.  _PowerDispatch ($A09E) is there, as
 * on the desktop Macs that had it, answering for a machine with nothing to
 * manage: no selectors beyond the count, no features, no batteries, no
 * sleep or dimming timeouts.  The calls that went to the PMU through
 * PMgrOp (BatteryStatus, ModemStatus, the wakeup time) answer unimpErr,
 * as PMgrOp is not implemented on a desktop; the serial port power calls
 * (AOn and the rest) do nothing, the ports being always powered.
 *
 * The Start Manager's defaults (startup device, operating system, video
 * and the SCSI timeout) live in parameter RAM, kept for the session.
 */
#include <string.h>

#include "legacy.h"
#include "tb_internal.h"

enum { unimpErr = -4 };

int16_t tb_PMSelectorCount(void) { return 0; }
uint32_t tb_PMFeatures(void) { return 0; }
uint8_t tb_GetSleepTimeout(void) { return 0; }
uint8_t tb_GetDimmingTimeout(void) { return 0; }
int16_t tb_BatteryCount(void) { return 0; }

int16_t tb_BatteryStatus(gaddr status, gaddr power) { return unimpErr; }
int16_t tb_ModemStatus(gaddr status) { return unimpErr; }
int16_t tb_GetWUTime(gaddr wuTime, gaddr wuFlag) { return unimpErr; }
int16_t tb_SetWUTime(int32_t wuTime) { return unimpErr; }
int16_t tb_DisableWUTime(void) { return unimpErr; }

void tb_AOn(void) {}
void tb_AOnIgnoreModem(void) {}
void tb_BOn(void) {}
void tb_AOff(void) {}
void tb_BOff(void) {}

/* The processor's speed in megahertz: the Quadra 800's 33, or for PowerPC
 * programs the 266 of the G3 its Gestalt describes. */
int32_t tb_GetCPUSpeed(void) { return gestalt_value(fourcc("sysa"), 1) == 2 ? 266 : 33; }

/* ---- The Start Manager ---- */

static uint8_t def_startup[4], def_os[2], def_video[2];
static int16_t scsi_timeout;

void startmgr_reset(void)
{
    memset(def_startup, 0, sizeof def_startup);
    memset(def_os, 0, sizeof def_os);
    def_os[1] = 1; /* DefOSRec's osType: Mac OS */
    memset(def_video, 0, sizeof def_video);
    scsi_timeout = 0;
}

void tb_GetDefaultStartup(gaddr pb) { gput(pb, def_startup, sizeof def_startup); }
void tb_SetDefaultStartup(gaddr pb) { gget(def_startup, pb, sizeof def_startup); }
void tb_GetOSDefault(gaddr pb) { gput(pb, def_os, sizeof def_os); }
void tb_SetOSDefault(gaddr pb) { gget(def_os, pb, sizeof def_os); }
void tb_SetVideoDefault(gaddr pb) { gget(def_video, pb, sizeof def_video); }
void startmgr_get_video(gaddr pb) { gput(pb, def_video, sizeof def_video); }
void tb_GetTimeout(gaddr count)
{
    if (count)
        wr16(count, (uint16_t)scsi_timeout);
}
void tb_SetTimeout(int16_t count) { scsi_timeout = count; }
