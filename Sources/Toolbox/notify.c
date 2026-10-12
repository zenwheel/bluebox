/*
 * Notification Manager.
 *
 * A request's parts go to the host: its message becomes a notification,
 * its mark or icon a Dock bounce and badge, its sound is played here.  The
 * response procedure runs at the next safe point after the request was
 * shown, as it did when System 7 had shown it; nmResp of -1 removes the
 * request.
 */
#include <pthread.h>
#include <string.h>

#include "forks.h"
#include "machine.h"
#include "tb_internal.h"
#include "trap.h"

enum { nmType = 8, qErr = -1 };

#define MAX_REQUESTS 16
static gaddr requests[MAX_REQUESTS];
static gaddr respond[MAX_REQUESTS]; /* shown, response not yet run */
static machine_notify_fn notify_fn;
static void *notify_ctx;

void machine_set_notification_handler(machine_notify_fn fn, void *ctx)
{
    notify_fn = fn;
    notify_ctx = ctx;
}

int16_t tb_NMInstall(gaddr nm)
{
    if (!nm || (int16_t)rd16(nm + NMRec_qType_off) != nmType)
        return -299; /* nmTypErr */
    int slot = -1;
    for (int i = 0; i < MAX_REQUESTS; i++)
        if (!requests[i]) {
            slot = i;
            break;
        }
    if (slot < 0)
        return qErr;
    requests[slot] = nm;
    gaddr snd = rd32(nm + NMRec_nmSound_off);
    if (snd == 0xFFFFFFFFu)
        tb_SysBeep(1);
    else if (snd && rd32(snd))
        tb_SndPlay(0, snd, 0);
    char text[768] = "";
    gaddr str = rd32(nm + NMRec_nmStr_off);
    if (str) {
        uint8_t s[256];
        uint32_t n = rd8(str);
        gget(s, str + 1, n);
        macroman_to_utf8(s, n, text, sizeof text);
    }
    int flags = 0;
    if (rd16(nm + NMRec_nmMark_off))
        flags |= MACHINE_NOTIFY_MARK;
    if (rd32(nm + NMRec_nmIcon_off))
        flags |= MACHINE_NOTIFY_ICON;
    if (notify_fn)
        notify_fn((int)slot + 1, text, flags, notify_ctx);
    else if (text[0])
        LOGF(LOG_UNIMPL, "Notification: %s\n", text);
    respond[slot] = nm;
    return noErr;
}

int16_t tb_NMRemove(gaddr nm)
{
    for (int i = 0; i < MAX_REQUESTS; i++)
        if (requests[i] == nm) {
            requests[i] = 0;
            respond[i] = 0;
            if (notify_fn)
                notify_fn(-(i + 1), "", 0, notify_ctx); /* withdrawn */
            return noErr;
        }
    return qErr;
}

/* Run response procedures for requests that have been shown. */
void notify_pump(void)
{
    for (int i = 0; i < MAX_REQUESTS; i++) {
        gaddr nm = respond[i];
        if (!nm)
            continue;
        respond[i] = 0;
        gaddr resp = rd32(nm + NMRec_nmResp_off);
        if (resp == 0xFFFFFFFFu) {
            tb_NMRemove(nm);
        } else if (resp) {
            cpu_push32(g_cpu, nm);
            guest_call(resp);
        }
    }
}

void notify_reset(void)
{
    memset(requests, 0, sizeof requests);
    memset(respond, 0, sizeof respond);
}
