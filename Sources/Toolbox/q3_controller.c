/*
 * QuickDraw 3D's controllers and trackers (QD3DController.h): the list of
 * 3D input devices drivers and programs register (Blue Box itself has
 * none, so the list holds only what a program makes), their values,
 * buttons and channels (through the controller's own channel methods),
 * trackers following their position and orientation with thresholds,
 * serial numbers and a short history of events, controller states, and
 * the cursor tracker.  The Cr names are the library's own for the same
 * calls.
 */
#include <math.h>
#include <stdlib.h>

#include "q3_view.h"
#include "tb_internal.h"

#define TRACKER Q3T('t', 'r', 'k', 'r')
#define CSTATE Q3T('c', 't', 's', 't')

enum { HISTORY = 64 };

typedef struct {
    uint32_t time, buttons;
    float pos[3], orient[4];
} tevent;

typedef struct {
    gaddr notify;
    bool active;
    float pos_thresh, orient_thresh;
    uint32_t buttons;
    float pos[3], orient[4], dpos[3], dorient[4];
    uint32_t pos_serial, orient_serial;
    float notified_pos[3], notified_orient[4];
    tevent events[HISTORY];
    int nevents, next_event;
} tracker;

typedef struct ctl {
    gaddr g;
    char signature[256];
    uint32_t value_count, channel_count;
    gaddr get_method, set_method;
    bool active, decommissioned;
    gaddr tracker;
    uint32_t buttons;
    float *values;
    uint32_t value_serial;
    float pos[3], orient[4];
} ctl;

enum { MAX_CONTROLLERS = 64 };
static ctl *ctls[MAX_CONTROLLERS];
static int nctls;
static uint32_t list_serial = 1;

/* The cursor tracker. */
static gaddr cursor_notify;
static bool cursor_deltas;
static float cursor_depth;
static float cursor_orient[4] = { 1, 0, 0, 0 };
static bool cursor_has_orient;
static uint32_t cursor_serial;

static ctl *ctl_of(gaddr g)
{
    for (int i = 0; i < nctls; i++)
        if (ctls[i]->g == g)
            return ctls[i];
    q3_error(kQ3ErrorInvalidParameter);
    return NULL;
}

static tracker *tracker_of(gaddr g)
{
    q3obj *o = q3_of(g, TRACKER);
    return o ? o->data : NULL;
}

static void tracker_dispose(q3obj *o) { free(o->data); }
static bool tracker_duplicate(q3obj *to, const q3obj *from)
{
    tracker *t = malloc(sizeof *t);
    if (t)
        *t = *(tracker *)from->data;
    to->data = t;
    return t != NULL;
}

typedef struct {
    gaddr controller;
    bool active;
    gaddr tracker;
    uint32_t buttons;
    bool saved;
} cstate;

static void cstate_dispose(q3obj *o) { free(o->data); }
static bool cstate_duplicate(q3obj *to, const q3obj *from)
{
    cstate *s = malloc(sizeof *s);
    if (s)
        *s = *(cstate *)from->data;
    to->data = s;
    return s != NULL;
}

void q3_controller_classes(void);
void q3_controller_classes(void)
{
    q3_class_ops(TRACKER, tracker_dispose, tracker_duplicate, NULL, false);
    q3_class_ops(CSTATE, cstate_dispose, cstate_duplicate, NULL, false);
}

static void quat_mul(const float *a, const float *b, float *o)
{
    float w = a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3];
    float x = a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2];
    float y = a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1];
    float z = a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0];
    o[0] = w, o[1] = x, o[2] = y, o[3] = z;
}

static void get3(gaddr p, float *v)
{
    for (int k = 0; k < 3; k++)
        v[k] = rdf(p + 4 * (uint32_t)k);
}
static void put3(gaddr p, const float *v)
{
    for (int k = 0; k < 3; k++)
        wrf(p + 4 * (uint32_t)k, v[k]);
}
static void get4(gaddr p, float *v)
{
    for (int k = 0; k < 4; k++)
        v[k] = rdf(p + 4 * (uint32_t)k);
}
static void put4(gaddr p, const float *v)
{
    for (int k = 0; k < 4; k++)
        wrf(p + 4 * (uint32_t)k, v[k]);
}

/* A tracker moved: its notify function, once past a threshold. */
static void tracker_moved(gaddr tg, tracker *t, gaddr controller)
{
    if (!t->active || !t->notify)
        return;
    float dp = 0, dq = 0;
    for (int k = 0; k < 3; k++)
        dp += (t->pos[k] - t->notified_pos[k]) * (t->pos[k] - t->notified_pos[k]);
    for (int k = 0; k < 4; k++)
        dq += fabsf(t->orient[k] - t->notified_orient[k]);
    if (sqrtf(dp) < t->pos_thresh && dq < t->orient_thresh)
        return;
    memcpy(t->notified_pos, t->pos, sizeof t->pos);
    memcpy(t->notified_orient, t->orient, sizeof t->orient);
    uint32_t args[2] = { tg, controller };
    ppc_call(t->notify, args, 2);
}

/* ---------------------------------------------------------------------- */
/* Controllers */

static void Q3Controller_GetListChanged(ppc_cpu *c)
{
    gaddr changed = Q3R(0), serial = Q3R(1);
    if (!changed || !serial) {
        Q3RETURN(kQ3Failure);
        return;
    }
    wr32(changed, rd32(serial) != list_serial);
    wr32(serial, list_serial);
    Q3RETURN(kQ3Success);
}

static void Q3Controller_Next(ppc_cpu *c)
{
    gaddr cur = Q3R(0), out = Q3R(1);
    if (!out) {
        Q3RETURN(kQ3Failure);
        return;
    }
    int at = 0;
    if (cur) {
        at = -1;
        for (int i = 0; i < nctls; i++)
            if (ctls[i]->g == cur)
                at = i + 1;
        if (at < 0) {
            wr32(out, 0);
            Q3RETURN(kQ3Failure);
            return;
        }
    }
    while (at < nctls && ctls[at]->decommissioned)
        at++;
    wr32(out, at < nctls ? ctls[at]->g : 0);
    Q3RETURN(kQ3Success);
}

static void Q3Controller_New(ppc_cpu *c)
{
    gaddr d = Q3R(0);
    if (!d || nctls == MAX_CONTROLLERS) {
        Q3RETURN(0);
        return;
    }
    ctl *x = calloc(1, sizeof *x);
    if (!x) {
        Q3RETURN(0);
        return;
    }
    q3_gstr(rd32(d), x->signature, sizeof x->signature);
    x->value_count = rd32(d + 4);
    x->channel_count = rd32(d + 8);
    x->get_method = rd32(d + 12);
    x->set_method = rd32(d + 16);
    x->values = calloc(x->value_count + 1, sizeof *x->values);
    x->active = true;
    x->orient[0] = 1;
    x->g = q3_handle_new(Q3H_CLASS, x);
    if (!x->g || !x->values) {
        free(x->values);
        free(x);
        Q3RETURN(0);
        return;
    }
    ctls[nctls++] = x;
    list_serial++;
    Q3RETURN(x->g);
}

static void Q3Controller_Decommission(ppc_cpu *c)
{
    ctl *x = ctl_of(Q3R(0));
    if (x) {
        x->decommissioned = true;
        x->active = false;
        list_serial++;
    }
    Q3RETURN(x ? kQ3Success : kQ3Failure);
}

static void Q3Controller_SetActivation(ppc_cpu *c)
{
    ctl *x = ctl_of(Q3R(0));
    if (x && x->active != (Q3R(1) != 0)) {
        x->active = Q3R(1) != 0;
        list_serial++;
    }
    Q3RETURN(x ? kQ3Success : kQ3Failure);
}

static void Q3Controller_GetActivation(ppc_cpu *c)
{
    ctl *x = ctl_of(Q3R(0));
    if (x && Q3R(1))
        wr32(Q3R(1), x->active);
    Q3RETURN(x && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3Controller_GetSignature(ppc_cpu *c)
{
    ctl *x = ctl_of(Q3R(0));
    gaddr out = Q3R(1);
    uint32_t n = Q3R(2);
    if (!x || !out || !n) {
        Q3RETURN(kQ3Failure);
        return;
    }
    uint32_t len = (uint32_t)strlen(x->signature);
    if (len >= n)
        len = n - 1;
    gput(out, x->signature, len);
    wr8(out + len, 0);
    Q3RETURN(kQ3Success);
}

static void Q3Controller_SetChannel(ppc_cpu *c)
{
    ctl *x = ctl_of(Q3R(0));
    if (!x || Q3R(1) >= x->channel_count || !x->set_method) {
        Q3RETURN(kQ3Failure);
        return;
    }
    uint32_t args[4] = { x->g, Q3R(1), Q3R(2), Q3R(3) };
    Q3RETURN(ppc_call(x->set_method, args, 4));
}

static void Q3Controller_GetChannel(ppc_cpu *c)
{
    ctl *x = ctl_of(Q3R(0));
    if (!x || Q3R(1) >= x->channel_count || !x->get_method) {
        if (Q3R(3))
            wr32(Q3R(3), 0);
        Q3RETURN(kQ3Failure);
        return;
    }
    uint32_t args[4] = { x->g, Q3R(1), Q3R(2), Q3R(3) };
    Q3RETURN(ppc_call(x->get_method, args, 4));
}

static void Q3Controller_GetChannelCount(ppc_cpu *c)
{
    ctl *x = ctl_of(Q3R(0));
    if (x && Q3R(1))
        wr32(Q3R(1), x->channel_count);
    Q3RETURN(x && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3Controller_GetValueCount(ppc_cpu *c)
{
    ctl *x = ctl_of(Q3R(0));
    if (x && Q3R(1))
        wr32(Q3R(1), x->value_count);
    Q3RETURN(x && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3Controller_SetTracker(ppc_cpu *c)
{
    ctl *x = ctl_of(Q3R(0));
    if (!x || (Q3R(1) && !tracker_of(Q3R(1)))) {
        Q3RETURN(kQ3Failure);
        return;
    }
    q3_assign(&x->tracker, Q3R(1));
    list_serial++;
    Q3RETURN(kQ3Success);
}

static void Q3Controller_GetTracker(ppc_cpu *c)
{
    ctl *x = ctl_of(Q3R(0));
    if (x && Q3R(1))
        wr32(Q3R(1), x->tracker ? q3_ref(x->tracker) : 0);
    Q3RETURN(x && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3Controller_HasTracker(ppc_cpu *c)
{
    ctl *x = ctl_of(Q3R(0));
    if (x && Q3R(1))
        wr32(Q3R(1), x->tracker != 0);
    Q3RETURN(x && Q3R(1) ? kQ3Success : kQ3Failure);
}

/* Without a tracker of its own a controller moves the cursors. */
static void Q3Controller_Track2DCursor(ppc_cpu *c)
{
    ctl *x = ctl_of(Q3R(0));
    if (x && Q3R(1))
        wr32(Q3R(1), x->active && !x->tracker);
    Q3RETURN(x && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3Controller_Track3DCursor(ppc_cpu *c)
{
    ctl *x = ctl_of(Q3R(0));
    if (x && Q3R(1))
        wr32(Q3R(1), x->active && !x->tracker && cursor_deltas);
    Q3RETURN(x && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3Controller_GetButtons(ppc_cpu *c)
{
    ctl *x = ctl_of(Q3R(0));
    if (x && Q3R(1))
        wr32(Q3R(1), x->buttons);
    Q3RETURN(x && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3Controller_SetButtons(ppc_cpu *c)
{
    ctl *x = ctl_of(Q3R(0));
    if (!x) {
        Q3RETURN(kQ3Failure);
        return;
    }
    uint32_t changed = x->buttons ^ Q3R(1);
    x->buttons = Q3R(1);
    tracker *t = tracker_of(x->tracker);
    if (t && changed) {
        t->buttons = (t->buttons & ~changed) | (x->buttons & changed);
        tracker_moved(x->tracker, t, x->g);
    }
    Q3RETURN(kQ3Success);
}

/* Position and orientation: the tracker's, or the controller's own. */
static void ctl_pose(ppc_cpu *c, bool orient, int op /* 0 get, 1 set, 2 move */)
{
    ctl *x = ctl_of(Q3R(0));
    gaddr p = Q3R(1);
    if (!x || !p) {
        Q3RETURN(kQ3Failure);
        return;
    }
    tracker *t = tracker_of(x->tracker);
    float *pos = t ? t->pos : x->pos, *q = t ? t->orient : x->orient;
    if (op == 0) {
        if (orient)
            put4(p, q);
        else
            put3(p, pos);
    } else if (orient) {
        float in[4];
        get4(p, in);
        if (op == 1)
            memcpy(q, in, sizeof in);
        else
            quat_mul(q, in, q);
        if (t) {
            t->orient_serial++;
            tracker_moved(x->tracker, t, x->g);
        }
    } else {
        float in[3];
        get3(p, in);
        for (int k = 0; k < 3; k++)
            pos[k] = op == 1 ? in[k] : pos[k] + in[k];
        if (t) {
            t->pos_serial++;
            tracker_moved(x->tracker, t, x->g);
        }
    }
    Q3RETURN(kQ3Success);
}
static void Q3Controller_GetTrackerPosition(ppc_cpu *c) { ctl_pose(c, false, 0); }
static void Q3Controller_SetTrackerPosition(ppc_cpu *c) { ctl_pose(c, false, 1); }
static void Q3Controller_MoveTrackerPosition(ppc_cpu *c) { ctl_pose(c, false, 2); }
static void Q3Controller_GetTrackerOrientation(ppc_cpu *c) { ctl_pose(c, true, 0); }
static void Q3Controller_SetTrackerOrientation(ppc_cpu *c) { ctl_pose(c, true, 1); }
static void Q3Controller_MoveTrackerOrientation(ppc_cpu *c) { ctl_pose(c, true, 2); }

static void Q3Controller_GetValues(ppc_cpu *c)
{
    ctl *x = ctl_of(Q3R(0));
    uint32_t n = Q3R(1);
    gaddr values = Q3R(2), changed = Q3R(3), serial = Q3R(4);
    if (!x) {
        Q3RETURN(kQ3Failure);
        return;
    }
    for (uint32_t i = 0; values && i < n && i < x->value_count; i++)
        wrf(values + 4 * i, x->values[i]);
    if (changed)
        wr32(changed, !serial || rd32(serial) != x->value_serial);
    if (serial)
        wr32(serial, x->value_serial);
    Q3RETURN(kQ3Success);
}

static void Q3Controller_SetValues(ppc_cpu *c)
{
    ctl *x = ctl_of(Q3R(0));
    gaddr values = Q3R(1);
    uint32_t n = Q3R(2);
    if (!x || !values) {
        Q3RETURN(kQ3Failure);
        return;
    }
    for (uint32_t i = 0; i < n && i < x->value_count; i++)
        x->values[i] = rdf(values + 4 * i);
    x->value_serial++;
    Q3RETURN(kQ3Success);
}

/* ---------------------------------------------------------------------- */
/* Controller states: a controller's activation, tracker and buttons,
 * saved and reset (a program taking it over), then restored. */

static void Q3ControllerState_New(ppc_cpu *c)
{
    ctl *x = ctl_of(Q3R(0));
    cstate *s = x ? calloc(1, sizeof *s) : NULL;
    q3obj *o = s ? q3_new(CSTATE, s) : NULL;
    if (!o) {
        free(s);
        Q3RETURN(0);
        return;
    }
    s->controller = x->g;
    Q3RETURN(o->g);
}

static void Q3ControllerState_SaveAndReset(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), CSTATE);
    cstate *s = o ? o->data : NULL;
    ctl *x = s ? ctl_of(s->controller) : NULL;
    if (!x) {
        Q3RETURN(kQ3Failure);
        return;
    }
    s->active = x->active;
    s->tracker = x->tracker;
    s->buttons = x->buttons;
    s->saved = true;
    x->tracker = 0; /* the state keeps the reference */
    x->buttons = 0;
    x->active = true;
    list_serial++;
    Q3RETURN(kQ3Success);
}

static void Q3ControllerState_Restore(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), CSTATE);
    cstate *s = o ? o->data : NULL;
    ctl *x = s && s->saved ? ctl_of(s->controller) : NULL;
    if (!x) {
        Q3RETURN(kQ3Failure);
        return;
    }
    q3_assign(&x->tracker, 0);
    x->tracker = s->tracker;
    x->active = s->active;
    x->buttons = s->buttons;
    s->tracker = 0;
    s->saved = false;
    list_serial++;
    Q3RETURN(kQ3Success);
}

/* ---------------------------------------------------------------------- */
/* Trackers */

static void Q3Tracker_New(ppc_cpu *c)
{
    tracker *t = calloc(1, sizeof *t);
    q3obj *o = t ? q3_new(TRACKER, t) : NULL;
    if (!o) {
        free(t);
        Q3RETURN(0);
        return;
    }
    t->notify = Q3R(0);
    t->active = true;
    t->orient[0] = t->notified_orient[0] = 1;
    Q3RETURN(o->g);
}

static void Q3Tracker_SetNotifyThresholds(ppc_cpu *c)
{
    tracker *t = tracker_of(Q3R(0));
    if (t) {
        t->pos_thresh = q3_farg(c, 0);
        t->orient_thresh = q3_farg(c, 1);
    }
    Q3RETURN(t ? kQ3Success : kQ3Failure);
}

static void Q3Tracker_GetNotifyThresholds(ppc_cpu *c)
{
    tracker *t = tracker_of(Q3R(0));
    if (t && Q3R(1))
        wrf(Q3R(1), t->pos_thresh);
    if (t && Q3R(2))
        wrf(Q3R(2), t->orient_thresh);
    Q3RETURN(t ? kQ3Success : kQ3Failure);
}

static void Q3Tracker_SetActivation(ppc_cpu *c)
{
    tracker *t = tracker_of(Q3R(0));
    if (t)
        t->active = Q3R(1) != 0;
    Q3RETURN(t ? kQ3Success : kQ3Failure);
}

static void Q3Tracker_GetActivation(ppc_cpu *c)
{
    tracker *t = tracker_of(Q3R(0));
    if (t && Q3R(1))
        wr32(Q3R(1), t->active);
    Q3RETURN(t && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3Tracker_GetButtons(ppc_cpu *c)
{
    tracker *t = tracker_of(Q3R(0));
    if (t && Q3R(1))
        wr32(Q3R(1), t->buttons);
    Q3RETURN(t && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3Tracker_ChangeButtons(ppc_cpu *c)
{
    tracker *t = tracker_of(Q3R(0));
    if (!t) {
        Q3RETURN(kQ3Failure);
        return;
    }
    uint32_t mask = Q3R(3);
    t->buttons = (t->buttons & ~mask) | (Q3R(2) & mask);
    tracker_moved(Q3R(0), t, Q3R(1));
    Q3RETURN(kQ3Success);
}

/* What has moved since the caller's serial number. */
static void Q3Tracker_GetPosition(ppc_cpu *c)
{
    tracker *t = tracker_of(Q3R(0));
    gaddr pos = Q3R(1), delta = Q3R(2), changed = Q3R(3), serial = Q3R(4);
    if (!t) {
        Q3RETURN(kQ3Failure);
        return;
    }
    if (pos)
        put3(pos, t->pos);
    if (delta) {
        put3(delta, t->dpos);
        memset(t->dpos, 0, sizeof t->dpos);
    }
    if (changed)
        wr32(changed, !serial || rd32(serial) != t->pos_serial);
    if (serial)
        wr32(serial, t->pos_serial);
    Q3RETURN(kQ3Success);
}

static void Q3Tracker_GetOrientation(ppc_cpu *c)
{
    tracker *t = tracker_of(Q3R(0));
    gaddr q = Q3R(1), delta = Q3R(2), changed = Q3R(3), serial = Q3R(4);
    if (!t) {
        Q3RETURN(kQ3Failure);
        return;
    }
    if (q)
        put4(q, t->orient);
    if (delta) {
        put4(delta, t->dorient[0] || t->dorient[1] || t->dorient[2] || t->dorient[3] ? t->dorient
                                                                                     : (float[4]){ 1, 0, 0, 0 });
        memset(t->dorient, 0, sizeof t->dorient);
    }
    if (changed)
        wr32(changed, !serial || rd32(serial) != t->orient_serial);
    if (serial)
        wr32(serial, t->orient_serial);
    Q3RETURN(kQ3Success);
}

static void tracker_pose(ppc_cpu *c, bool orient, bool move)
{
    tracker *t = tracker_of(Q3R(0));
    gaddr p = Q3R(2);
    if (!t || !p) {
        Q3RETURN(kQ3Failure);
        return;
    }
    if (orient) {
        float in[4], old[4];
        get4(p, in);
        memcpy(old, t->orient, sizeof old);
        if (move) {
            quat_mul(t->orient, in, t->orient);
            memcpy(t->dorient, in, sizeof in);
        } else {
            memcpy(t->orient, in, sizeof in);
            float inv[4] = { old[0], -old[1], -old[2], -old[3] };
            quat_mul(inv, in, t->dorient);
        }
        t->orient_serial++;
    } else {
        float in[3];
        get3(p, in);
        for (int k = 0; k < 3; k++) {
            float was = t->pos[k];
            t->pos[k] = move ? was + in[k] : in[k];
            t->dpos[k] += t->pos[k] - was;
        }
        t->pos_serial++;
    }
    tracker_moved(Q3R(0), t, Q3R(1));
    Q3RETURN(kQ3Success);
}
static void Q3Tracker_SetPosition(ppc_cpu *c) { tracker_pose(c, false, false); }
static void Q3Tracker_MovePosition(ppc_cpu *c) { tracker_pose(c, false, true); }
static void Q3Tracker_SetOrientation(ppc_cpu *c) { tracker_pose(c, true, false); }
static void Q3Tracker_MoveOrientation(ppc_cpu *c) { tracker_pose(c, true, true); }

static void Q3Tracker_SetEventCoordinates(ppc_cpu *c)
{
    tracker *t = tracker_of(Q3R(0));
    if (!t) {
        Q3RETURN(kQ3Failure);
        return;
    }
    tevent *e = &t->events[t->next_event];
    t->next_event = (t->next_event + 1) % HISTORY;
    if (t->nevents < HISTORY)
        t->nevents++;
    e->time = Q3R(1);
    e->buttons = Q3R(2);
    if (Q3R(3))
        get3(Q3R(3), e->pos);
    else
        memcpy(e->pos, t->pos, sizeof e->pos);
    if (Q3R(4))
        get4(Q3R(4), e->orient);
    else
        memcpy(e->orient, t->orient, sizeof e->orient);
    Q3RETURN(kQ3Success);
}

/* The latest event at or before the time stamp. */
static void Q3Tracker_GetEventCoordinates(ppc_cpu *c)
{
    tracker *t = tracker_of(Q3R(0));
    uint32_t when = Q3R(1);
    const tevent *best = NULL;
    for (int i = 0; t && i < t->nevents; i++) {
        const tevent *e = &t->events[i];
        if (e->time <= when && (!best || e->time > best->time))
            best = e;
    }
    if (!best) {
        Q3RETURN(kQ3Failure);
        return;
    }
    if (Q3R(2))
        wr32(Q3R(2), best->buttons);
    if (Q3R(3))
        put3(Q3R(3), best->pos);
    if (Q3R(4))
        put4(Q3R(4), best->orient);
    Q3RETURN(kQ3Success);
}

/* ---------------------------------------------------------------------- */
/* The cursor tracker */

static void Q3CursorTracker_PrepareTracking(ppc_cpu *c)
{
    cursor_depth = 0;
    Q3RETURN(kQ3Success);
}

static void Q3CursorTracker_SetTrackDeltas(ppc_cpu *c)
{
    cursor_deltas = Q3R(0) != 0;
    Q3RETURN(kQ3Success);
}

static void Q3CursorTracker_GetAndClearDeltas(ppc_cpu *c)
{
    if (Q3R(0))
        wrf(Q3R(0), cursor_depth);
    if (Q3R(1))
        put4(Q3R(1), cursor_orient);
    if (Q3R(2))
        wr32(Q3R(2), cursor_has_orient);
    if (Q3R(3))
        wr32(Q3R(3), Q3R(4) && rd32(Q3R(4)) != cursor_serial);
    if (Q3R(4))
        wr32(Q3R(4), cursor_serial);
    cursor_depth = 0;
    cursor_orient[0] = 1;
    cursor_orient[1] = cursor_orient[2] = cursor_orient[3] = 0;
    cursor_has_orient = false;
    Q3RETURN(kQ3Success);
}

static void Q3CursorTracker_SetNotifyFunc(ppc_cpu *c)
{
    cursor_notify = Q3R(0);
    Q3RETURN(kQ3Success);
}

static void Q3CursorTracker_GetNotifyFunc(ppc_cpu *c)
{
    if (Q3R(0))
        wr32(Q3R(0), cursor_notify);
    Q3RETURN(Q3R(0) ? kQ3Success : kQ3Failure);
}

static void CrLibrary_Init(ppc_cpu *c) { Q3RETURN(kQ3Success); }

static void CrTracker_Delete(ppc_cpu *c)
{
    q3_release(Q3R(0));
    Q3RETURN(kQ3Success);
}

static void CrTracker_GetTrackerObject(ppc_cpu *c) { Q3RETURN(Q3R(0)); }

#define E(n) { #n, n }
#define BOTH(n) { "Q3" #n, Q3##n }, { "Cr" #n, Q3##n }
const ppc_export q3_controller_exports[] = {
    BOTH(Controller_GetListChanged), BOTH(Controller_Next), BOTH(Controller_New), BOTH(Controller_Decommission),
    BOTH(Controller_SetActivation), BOTH(Controller_GetActivation), BOTH(Controller_GetSignature),
    BOTH(Controller_SetChannel), BOTH(Controller_GetChannel), BOTH(Controller_GetValueCount),
    BOTH(Controller_SetTracker), BOTH(Controller_HasTracker), BOTH(Controller_Track2DCursor),
    BOTH(Controller_Track3DCursor), BOTH(Controller_GetButtons), BOTH(Controller_SetButtons),
    BOTH(Controller_GetTrackerPosition), BOTH(Controller_SetTrackerPosition), BOTH(Controller_MoveTrackerPosition),
    BOTH(Controller_GetTrackerOrientation), BOTH(Controller_SetTrackerOrientation),
    BOTH(Controller_MoveTrackerOrientation), BOTH(Controller_GetValues), BOTH(Controller_SetValues),
    { "CrController_GetChannelCount", Q3Controller_GetChannelCount },
    { "CrController_GetTracker", Q3Controller_GetTracker },
    E(Q3ControllerState_New), E(Q3ControllerState_SaveAndReset), E(Q3ControllerState_Restore),
    BOTH(Tracker_New), BOTH(Tracker_SetNotifyThresholds), BOTH(Tracker_GetNotifyThresholds),
    BOTH(Tracker_SetActivation), BOTH(Tracker_GetActivation), BOTH(Tracker_GetButtons), BOTH(Tracker_ChangeButtons),
    BOTH(Tracker_GetPosition), BOTH(Tracker_SetPosition), BOTH(Tracker_MovePosition), BOTH(Tracker_GetOrientation),
    BOTH(Tracker_SetOrientation), BOTH(Tracker_MoveOrientation), BOTH(Tracker_SetEventCoordinates),
    BOTH(Tracker_GetEventCoordinates), E(CrTracker_Delete), E(CrTracker_GetTrackerObject),
    BOTH(CursorTracker_PrepareTracking), BOTH(CursorTracker_SetTrackDeltas), BOTH(CursorTracker_GetAndClearDeltas),
    BOTH(CursorTracker_SetNotifyFunc), BOTH(CursorTracker_GetNotifyFunc),
    E(CrLibrary_Init), { "CrLibrary_Exit", CrLibrary_Init },
};
const unsigned q3_controller_export_count = sizeof q3_controller_exports / sizeof q3_controller_exports[0];

void q3_controller_reset(void);
void q3_controller_reset(void)
{
    for (int i = 0; i < nctls; i++) {
        free(ctls[i]->values);
        free(ctls[i]);
    }
    nctls = 0;
    list_serial = 1;
    cursor_notify = 0;
}
