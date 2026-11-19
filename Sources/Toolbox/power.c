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
 * The sleep queue (_Sleep, _SleepQInstall, _SleepQRemove: $A08A and its
 * flag variants) is kept as System 7 keeps it, and _Sleep asks and tells
 * its procedures as a portable's would; the machine itself never sleeps,
 * so a sleep that goes ahead wakes at once.
 *
 * The Start Manager's defaults (startup device, operating system, video
 * and the SCSI timeout) live in parameter RAM, kept for the session.
 */
#include <string.h>

#include "legacy.h"
#include "os9svc.h"
#include "tb_internal.h"
#include "trap.h"

enum { unimpErr = -4 };

int16_t tb_PMSelectorCount(void) { return 0; }
uint32_t tb_PMFeatures(void) { return 0; }
uint8_t tb_GetSleepTimeout(void) { return os9svc_sleep_timeout(); }
uint8_t tb_GetDimmingTimeout(void) { return os9svc_dimming_timeout(); }
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

/* ---- The sleep queue ---- */

enum {
    SleepQRec_sleepQLink = 0, SleepQRec_sleepQType = 4, SleepQRec_sleepQProc = 6, SleepQRec_sleepQFlags = 10,
    kSleepRequest = 1, kSleepDemand = 2, kSleepWakeUp = 3, kSleepRevoke = 4, kSleepNow = 6,
    noCalls = 1, noRequest = 2,
};

static gaddr sleep_queue; /* the first SleepQRec, linked through sleepQLink */

void tb_SleepQRemove(gaddr q)
{
    if (!q)
        return;
    if (sleep_queue == q) {
        sleep_queue = rd32(q + SleepQRec_sleepQLink);
        return;
    }
    for (gaddr e = sleep_queue; e; e = rd32(e + SleepQRec_sleepQLink))
        if (rd32(e + SleepQRec_sleepQLink) == q) {
            wr32(e + SleepQRec_sleepQLink, rd32(q + SleepQRec_sleepQLink));
            return;
        }
}

/* At the end of the queue: procedures are told in the order they came. */
void tb_SleepQInstall(gaddr q)
{
    if (!q)
        return;
    tb_SleepQRemove(q); /* never in the queue twice */
    wr32(q + SleepQRec_sleepQLink, 0);
    if (!sleep_queue) {
        sleep_queue = q;
        return;
    }
    gaddr e = sleep_queue;
    while (rd32(e + SleepQRec_sleepQLink))
        e = rd32(e + SleepQRec_sleepQLink);
    wr32(e + SleepQRec_sleepQLink, q);
}

/* A procedure called as the Power Manager calls it: D0 the message, A0 its
 * record, the answer in D0 (nonzero: it refuses a request). */
static int32_t sleep_tell(gaddr q, int32_t message)
{
    cpu68k *c = g_cpu;
    gaddr proc = rd32(q + SleepQRec_sleepQProc);
    if (!c || !proc)
        return 0;
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d);
    memcpy(a, c->a, sizeof a);
    c->d[0] = (uint32_t)message;
    c->a[0] = q;
    guest_call_regs(proc);
    int32_t answer = (int32_t)c->d[0];
    memcpy(c->d, d, sizeof d);
    memcpy(c->a, a, sizeof a);
    return answer;
}

static void sleep_tell_all(int32_t message)
{
    for (gaddr q = sleep_queue; q; q = rd32(q + SleepQRec_sleepQLink))
        if (!(rd16(q + SleepQRec_sleepQFlags) & noCalls))
            sleep_tell(q, message);
}

/* _Sleep: a request each procedure may refuse (those asked before it are
 * then told the request is revoked, and the refusal is the answer); a
 * demand, or a request all agreed to, is told to all, and as this machine
 * doesn't sleep, waking up follows at once. */
static int32_t sleep_call(int32_t message)
{
    switch (message) {
    case kSleepRequest:
        for (gaddr q = sleep_queue; q; q = rd32(q + SleepQRec_sleepQLink)) {
            uint16_t flags = rd16(q + SleepQRec_sleepQFlags);
            if (flags & (noCalls | noRequest))
                continue;
            int32_t no = sleep_tell(q, kSleepRequest);
            if (no) {
                for (gaddr r = sleep_queue; r != q; r = rd32(r + SleepQRec_sleepQLink))
                    if (!(rd16(r + SleepQRec_sleepQFlags) & (noCalls | noRequest)))
                        sleep_tell(r, kSleepRevoke);
                return no;
            }
        }
        /* fall through: all agreed */
    case kSleepDemand:
    case kSleepNow:
        sleep_tell_all(kSleepDemand);
        sleep_tell_all(kSleepWakeUp);
        return 0;
    default:
        return 0;
    }
}

/* $A08A _Sleep (D0 the message), $A28A _SleepQInstall and $A48A
 * _SleepQRemove (A0 the record); D0 is the answer. */
void raw_Sleep(cpu68k *c, uint16_t tw)
{
    switch (tw & 0x0600) {
    case 0x0000:
        c->d[0] = (uint32_t)sleep_call((int32_t)c->d[0]);
        return;
    case 0x0200:
        tb_SleepQInstall(c->a[0]);
        break;
    case 0x0400:
        tb_SleepQRemove(c->a[0]);
        break;
    }
    c->d[0] = 0;
}

void sleep_reset(void) { sleep_queue = 0; }


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
