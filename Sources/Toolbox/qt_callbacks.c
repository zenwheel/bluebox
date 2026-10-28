/*
 * Time base callbacks (NewCallBack, CallMeWhen): a program's routine
 * called when a time base reaches a time going the way asked, when its
 * rate changes as asked, when its time jumps, or when it reaches its
 * start or stop.  They are checked whenever the program gives QuickTime
 * time (MoviesTask) and, for those asked for at interrupt time, as events
 * are fetched, which is as near as a run gets to QuickTime's interrupts.
 * A callback goes off once and is armed again by CallMeWhen.
 */
#include <stdlib.h>
#include <string.h>

#include "qt_errors.h"
#include "qt_movie_internal.h"
#include "tb_internal.h"

enum {
    callBackAtTime = 1, callBackAtRate = 2, callBackAtTimeJump = 3, callBackAtExtremes = 4,
    callBackAtTimeBaseDisposed = 5, callBackAtInterrupt = 0x8000, callBackAtDeferredTask = 0x4000,
    triggerTimeFwd = 1, triggerTimeBwd = 2,
    triggerRateLT = 4, triggerRateGT = 8, triggerRateEqual = 16, triggerRateNotEqual = 32,
    triggerAtStart = 1, triggerAtStop = 2,
};

#define MAX_CALLBACKS 256

typedef struct {
    gaddr token;          /* the QTCallBack */
    gaddr tb;
    int16_t type;
    gaddr proc;
    int32_t refcon, p1, p2, p3;
    bool armed;
    bool seen;            /* last time and rate known */
    int64_t last;         /* the time base's time at the last look, in p3's scale (AtTime) or its own */
    int32_t last_rate;
    bool in_call;
} callback;

static callback cbs[MAX_CALLBACKS];

static callback *cb_of(gaddr token)
{
    for (int i = 0; token && i < MAX_CALLBACKS; i++)
        if (cbs[i].token == token)
            return &cbs[i];
    return NULL;
}

/* pascal void (QTCallBack cb, long refCon) */
static void call(callback *c)
{
    if (!c->proc || c->in_call)
        return;
    c->in_call = true;
    gaddr token = c->token, proc = c->proc;
    int32_t refcon = c->refcon;
    cpu68k *cpu = g_cpu;
    cpu_push32(cpu, token);
    cpu_push32(cpu, (uint32_t)refcon);
    guest_call(proc);
    c = cb_of(token); /* (the routine may have disposed of it) */
    if (c)
        c->in_call = false;
}

int32_t tb_NewCallBack(int32_t tb, int16_t cbType)
{
    if (!tb) {
        qt_set_error(paramErr);
        return 0;
    }
    for (int i = 0; i < MAX_CALLBACKS; i++)
        if (!cbs[i].token) {
            gaddr h = tb_NewHandle(4, false, true);
            if (!h)
                break;
            wr32(rd32(h), 0x7163616C); /* 'qcal' */
            cbs[i] = (callback){ .token = h, .tb = (gaddr)tb, .type = cbType };
            qt_set_error(noErr);
            return (int32_t)h;
        }
    qt_set_error(memFullErr);
    return 0;
}

void tb_DisposeCallBack(int32_t cb)
{
    callback *c = cb_of((gaddr)cb);
    if (!c)
        return;
    tb_DisposeHandle(c->token);
    memset(c, 0, sizeof *c);
}

int16_t tb_GetCallBackType(int32_t cb)
{
    callback *c = cb_of((gaddr)cb);
    return c ? c->type : 0;
}

int32_t tb_GetCallBackTimeBase(int32_t cb)
{
    callback *c = cb_of((gaddr)cb);
    return c ? (int32_t)c->tb : 0;
}

/* The time base's time in a scale (0: its own) and rate now. */
static int64_t tb_time(gaddr tb, int32_t scale) { return tb_GetTimeBaseTime((int32_t)tb, scale, 0); }

int16_t tb_CallMeWhen(int32_t cb, int32_t callBackProc, int32_t refCon, int32_t param1, int32_t param2, int32_t param3)
{
    callback *c = cb_of((gaddr)cb);
    if (!c)
        return qt_set_error(paramErr);
    c->proc = (gaddr)callBackProc;
    c->refcon = refCon;
    c->p1 = param1;
    c->p2 = param2;
    c->p3 = param3;
    c->armed = true;
    c->last = tb_time(c->tb, (c->type & 0xFF) == callBackAtTime ? param3 : 0);
    c->last_rate = tb_GetTimeBaseEffectiveRate((int32_t)c->tb);
    c->seen = true;
    return noErr;
}

void tb_CancelCallBack(int32_t cb)
{
    callback *c = cb_of((gaddr)cb);
    if (c)
        c->armed = false;
}

/* For callback components: the Movie Toolbox arms and disarms them. */
int16_t tb_AddCallBackToTimeBase(int32_t cb)
{
    callback *c = cb_of((gaddr)cb);
    if (!c)
        return paramErr;
    c->armed = true;
    return noErr;
}

int16_t tb_RemoveCallBackFromTimeBase(int32_t cb)
{
    callback *c = cb_of((gaddr)cb);
    if (!c)
        return paramErr;
    c->armed = false;
    return noErr;
}

int32_t tb_GetFirstCallBack(int32_t tb)
{
    for (int i = 0; i < MAX_CALLBACKS; i++)
        if (cbs[i].token && cbs[i].tb == (gaddr)tb)
            return (int32_t)cbs[i].token;
    return 0;
}

int32_t tb_GetNextCallBack(int32_t cb)
{
    callback *c = cb_of((gaddr)cb);
    if (!c)
        return 0;
    for (int i = (int)(c - cbs) + 1; i < MAX_CALLBACKS; i++)
        if (cbs[i].token && cbs[i].tb == c->tb)
            return (int32_t)cbs[i].token;
    return 0;
}

void tb_ExecuteCallBack(int32_t cb)
{
    callback *c = cb_of((gaddr)cb);
    if (c)
        call(c);
}

static bool rate_matches(int32_t cond, int32_t rate, int32_t want)
{
    if (!(cond & (triggerRateLT | triggerRateGT | triggerRateEqual | triggerRateNotEqual)))
        return true; /* triggerRateChange: any change */
    return ((cond & triggerRateLT) && rate < want) || ((cond & triggerRateGT) && rate > want) ||
           ((cond & triggerRateEqual) && rate == want) || ((cond & triggerRateNotEqual) && rate != want);
}

/* Whether a callback is due now; it is disarmed when it goes off. */
static bool due(callback *c)
{
    int kind = c->type & 0xFF;
    switch (kind) {
    case callBackAtTime: {
        int64_t now = tb_time(c->tb, c->p3), was = c->last, at = c->p2;
        c->last = now;
        bool fwd = (c->p1 & triggerTimeFwd) && was < at && now >= at;
        bool bwd = (c->p1 & triggerTimeBwd) && was > at && now <= at;
        return fwd || bwd;
    }
    case callBackAtRate: {
        int32_t rate = tb_GetTimeBaseEffectiveRate((int32_t)c->tb), was = c->last_rate;
        c->last_rate = rate;
        return rate != was && rate_matches(c->p1, rate, c->p2);
    }
    case callBackAtExtremes: {
        int64_t now = tb_time(c->tb, 0), was = c->last;
        c->last = now;
        int64_t start = tb_GetTimeBaseStartTime((int32_t)c->tb, 0, 0), stop = tb_GetTimeBaseStopTime((int32_t)c->tb, 0, 0);
        bool at_stop = (c->p1 & triggerAtStop) && stop > start && was < stop && now >= stop;
        bool at_start = (c->p1 & triggerAtStart) && was > start && now <= start;
        return at_stop || at_start;
    }
    }
    return false;
}

static void poll(bool interrupt_only)
{
    for (int i = 0; i < MAX_CALLBACKS; i++) {
        callback *c = &cbs[i];
        if (!c->token || !c->armed || c->in_call)
            continue;
        if (interrupt_only && !(c->type & (callBackAtInterrupt | callBackAtDeferredTask)))
            continue;
        if (due(c)) {
            c->armed = false;
            call(c);
        }
    }
}

void qt_callbacks_poll(void) { poll(false); }
void qt_callbacks_periodic(void) { poll(true); }

/* A time base's time was set: those waiting for a jump go off; the rest
 * look from the new time on. */
void qt_callbacks_jumped(gaddr tb)
{
    for (int i = 0; i < MAX_CALLBACKS; i++) {
        callback *c = &cbs[i];
        if (!c->token || c->tb != tb)
            continue;
        c->last = tb_time(tb, (c->type & 0xFF) == callBackAtTime ? c->p3 : 0);
        if (c->armed && (c->type & 0xFF) == callBackAtTimeJump) {
            c->armed = false;
            call(c);
        }
    }
}

/* A time base going: its callbacks that asked to know are told, then all
 * of them are put out of use (the program still disposes of them). */
void qt_callbacks_timebase_gone(gaddr tb)
{
    for (int i = 0; i < MAX_CALLBACKS; i++) {
        callback *c = &cbs[i];
        if (!c->token || c->tb != tb)
            continue;
        if (c->armed && (c->type & 0xFF) == callBackAtTimeBaseDisposed)
            call(c);
        c->armed = false;
    }
}

void qt_callbacks_reset(void) { memset(cbs, 0, sizeof cbs); }
