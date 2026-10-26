/*
 * The Power Manager's _PowerDispatch selectors PowerMgrLib exports beyond
 * power.c's.  The machine is a desktop with nothing to power manage: the
 * settings (timeouts, the sleep and dimming switches, the SCSI disk mode
 * address) are kept and read back, the disks are always spinning, the
 * processor runs at full speed, there are no batteries (their records come
 * back zero), and what only hardware can do (the startup timer, the sound
 * mixer byte) answers unimpErr, as PMgrOp does in power.c.  UpdateSystemActivity
 * keeps the time of each kind of activity for GetLastActivity.
 */
#include <string.h>

#include "legacy.h"
#include "os9svc.h"
#include "tb_internal.h"

enum { unimpErr = -4, qErr = -1 };
enum { kMaxActivity = 4 }; /* OverallAct, UsrActivity, NetActivity, HDActivity, IdleActivity */

static struct {
    uint8_t sleep_timeout, disk_timeout, dimming_timeout;
    bool auto_sleep_off, dimming_off, spindown_off, dim_suspend, cycling;
    int16_t scsi_address, modem_state;
    uint8_t wakeup[6];
    uint32_t activity[kMaxActivity + 1];
} pm;

#define MAX_QUEUED 16
static gaddr hd_queue[MAX_QUEUED], state_queue[MAX_QUEUED];

void os9svc_power_reset(void)
{
    memset(&pm, 0, sizeof pm);
    memset(hd_queue, 0, sizeof hd_queue);
    memset(state_queue, 0, sizeof state_queue);
}

uint8_t os9svc_sleep_timeout(void) { return pm.sleep_timeout; }
uint8_t os9svc_dimming_timeout(void) { return pm.dimming_timeout; }

void tb_SetSleepTimeout(uint8_t timeout) { pm.sleep_timeout = timeout; }
uint8_t tb_GetHardDiskTimeout(void) { return pm.disk_timeout; }
void tb_SetHardDiskTimeout(uint8_t timeout) { pm.disk_timeout = timeout; }
int8_t tb_HardDiskPowered(void) { return 1; }
void tb_SpinDownHardDisk(void) {}
int8_t tb_IsSpindownDisabled(void) { return pm.spindown_off; }
void tb_SetSpindownDisable(int8_t setDisable) { pm.spindown_off = setDisable != 0; }

/* The queues of elements to tell when a disk spins down or the Power
 * Manager's state changes: kept; nothing happens to tell them of. */
static int16_t queue_install(gaddr *q, gaddr e)
{
    if (!e)
        return paramErr;
    for (int i = 0; i < MAX_QUEUED; i++)
        if (q[i] == e)
            return noErr;
    for (int i = 0; i < MAX_QUEUED; i++)
        if (!q[i]) {
            q[i] = e;
            return noErr;
        }
    return memFullErr;
}

static int16_t queue_remove(gaddr *q, gaddr e)
{
    for (int i = 0; i < MAX_QUEUED; i++)
        if (e && q[i] == e) {
            q[i] = 0;
            return noErr;
        }
    return qErr;
}

int16_t tb_HardDiskQInstall(gaddr theElement) { return queue_install(hd_queue, theElement); }
int16_t tb_HardDiskQRemove(gaddr theElement) { return queue_remove(hd_queue, theElement); }
int16_t tb_PMgrStateQInstall(gaddr theElement) { return queue_install(state_queue, theElement); }
int16_t tb_PMgrStateQRemove(gaddr theElement) { return queue_remove(state_queue, theElement); }

/* No batteries: their records are zero (no battery installed). */
void tb_GetScaledBatteryInfo(int16_t whichBattery, uint32_t *theInfo)
{
    if (theInfo)
        *theInfo = 0; /* the glue stores it where the caller's A0 points */
}

int32_t tb_GetBatteryVoltage(int16_t whichBattery) { return 0; }

void tb_GetBatteryTimes(int16_t whichBattery, gaddr theTimes)
{
    for (int i = 0; theTimes && i < 4; i++)
        wr32(theTimes + 4u * (uint32_t)i, 0);
}

void tb_AutoSleepControl(int8_t enableSleep) { pm.auto_sleep_off = !enableSleep; }
int8_t tb_IsAutoSlpControlDisabled(void) { return pm.auto_sleep_off; }
void tb_SetDimmingTimeout(uint8_t timeout) { pm.dimming_timeout = timeout; }
void tb_DimmingControl(int8_t enableSleep) { pm.dimming_off = !enableSleep; }
int8_t tb_IsDimmingControlDisabled(void) { return pm.dimming_off; }
int8_t tb_GetDimSuspendState(void) { return pm.dim_suspend; }
void tb_SetDimSuspendState(int8_t dimSuspendState) { pm.dim_suspend = dimSuspendState != 0; }

/* No internal modem. */
uint32_t tb_GetIntModemInfo(void) { return 0; }
void tb_SetIntModemState(int16_t theState) { pm.modem_state = theState; }

/* The processor always runs flat out, at the speed Gestalt describes. */
int16_t tb_MaximumProcessorSpeed(void) { return (int16_t)tb_GetCPUSpeed(); }
int16_t tb_CurrentProcessorSpeed(void) { return (int16_t)tb_GetCPUSpeed(); }
int8_t tb_FullProcessorSpeed(void) { return 1; }
int8_t tb_SetProcessorSpeed(int8_t fullSpeed) { return 1; }
int8_t tb_IsProcessorCyclingEnabled(void) { return pm.cycling; }
void tb_EnableProcessorCycling(int8_t enable) { pm.cycling = enable != 0; }

int16_t tb_GetSCSIDiskModeAddress(void) { return pm.scsi_address; }
void tb_SetSCSIDiskModeAddress(int16_t scsiAddress) { pm.scsi_address = scsiAddress; }

/* WakeupTime: wakeTime, wakeEnabled, filler.  The machine cannot wake
 * itself, so a time set is only remembered. */
void tb_GetWakeupTimer(gaddr theTime)
{
    for (int i = 0; theTime && i < 6; i++)
        wr8(theTime + (uint32_t)i, pm.wakeup[i]);
}

void tb_SetWakeupTimer(gaddr theTime)
{
    for (int i = 0; theTime && i < 6; i++)
        pm.wakeup[i] = rd8(theTime + (uint32_t)i);
}

int16_t tb_GetStartupTimer(gaddr theTime) { return unimpErr; }
int16_t tb_SetStartupTimer(gaddr theTime) { return unimpErr; }
int16_t tb_GetSoundMixerState(gaddr theSoundMixerByte) { return unimpErr; }
int16_t tb_SetSoundMixerState(gaddr theSoundMixerByte) { return unimpErr; }

/* UpdateSystemActivity(activity): note the time (in ticks) of that kind of
 * activity (the overall one too); DelaySystemIdle keeps the machine from
 * idling a little longer, which it has no timer to do. */
int16_t tb_UpdateSystemActivity(uint8_t activity)
{
    if (activity > kMaxActivity)
        return paramErr;
    pm.activity[activity] = rd32(LM_Ticks);
    pm.activity[0] = pm.activity[activity];
    return noErr;
}

int16_t tb_DelaySystemIdle(void) { return noErr; }

/* GetLastActivity(info): ActivityType in, ActivityTime out. */
int16_t tb_GetLastActivity(gaddr theActivity)
{
    if (!theActivity)
        return paramErr;
    uint16_t type = rd16(theActivity);
    if (type > kMaxActivity)
        return paramErr;
    wr32(theActivity + 2, pm.activity[type]);
    return noErr;
}
