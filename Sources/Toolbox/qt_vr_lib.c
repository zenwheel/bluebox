/*
 * QuickTimeVRLib's calls for PowerPC programs, on the VR engine (qt_vr.c).
 *
 * Angles come and go in the program's units (radians unless it asked for
 * degrees); floats are in floating-point registers and take a word of the
 * parameter area each, as the PowerPC calling convention has them.  The
 * calls a program can intercept (QTVRInstallInterceptProc) go through its
 * routine with an intercept record holding their parameters, and do their
 * own work after it unless it cancels.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "qd_internal.h"
#include "qt_atoms.h"
#include "qt_comp.h"
#include "qt_movie_internal.h"
#include "qt_vr_internal.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define PI 3.14159265358979323846

static void ret(ppc_cpu *c, uint32_t v)
{
    c->gpr[3] = v;
    c->pc = c->lr;
}
static void retf(ppc_cpu *c, float f)
{
    ppc_set_fpr(c, 1, f);
    c->pc = c->lr;
}
static float arg_f(const ppc_cpu *c, int n) { return (float)ppc_fpr(c, n); }

static float getf(gaddr p)
{
    uint32_t u = rd32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static void putf(gaddr p, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    if (p)
        wr32(p, u);
}

/* Degrees from the program's units and back. */
static float in_deg(const vr *v, float x) { return v->units ? (float)(x * 180.0 / PI) : x; }
static float out_units(const vr *v, float d) { return v->units ? (float)(d * PI / 180.0) : d; }

/* ---- The program's routines ---- */

int16_t vr_enter_proc(vr *v, uint32_t id)
{
    if (!v->entering.proc)
        return noErr;
    uint32_t a[] = { v->token, id, (uint32_t)v->entering.refcon };
    return (int16_t)mm_call(v->entering.proc, 0x00000FE0, a, 3);
}

bool vr_leave_proc(vr *v, uint32_t from, uint32_t to)
{
    if (!v->leaving.proc)
        return false;
    gaddr cancel = tb_NewPtr(2, false, true);
    uint32_t a[] = { v->token, from, to, cancel, (uint32_t)v->leaving.refcon };
    mm_call(v->leaving.proc, 0x0000FFE0, a, 5);
    bool r = rd8(cancel) != 0;
    tb_DisposePtr(cancel);
    return r;
}

void vr_mouse_over(vr *v, uint32_t id, uint32_t flags)
{
    if (!v->mouse_over.proc)
        return;
    uint32_t a[] = { v->token, id, flags, (uint32_t)v->mouse_over.refcon };
    mm_call(v->mouse_over.proc, 0x00003FE0, a, 4);
}

void vr_prescreen(vr *v)
{
    if (!v->prescreen.proc)
        return;
    uint32_t a[] = { v->token, (uint32_t)v->prescreen.refcon };
    mm_call(v->prescreen.proc, 0x000003E0, a, 2);
}

/* ---- Intercepted calls ---- */

static int kind_of(uint32_t sel) { return sel == SEL_PAN ? VR_PAN : sel == SEL_TILT ? VR_TILT : VR_FOV; }

static uint32_t hot_and_report(vr *v, uint32_t pt, gaddr id_out)
{
    uint32_t id = vr_hot_at(v, pt_h(pt), pt_v(pt));
    if (id_out)
        wr32(id_out, id);
    return id;
}

/* The mouse moving over hot spots: leave the old, enter the new. */
static void track_over(vr *v, uint32_t id, gaddr window)
{
    if (!v->over_tracking)
        return;
    if (id != v->over_id) {
        if (v->over_id) {
            uint32_t p[6] = { v->over_id, window };
            vr_call(v, SEL_LEAVE, p, 2);
        }
        v->over_id = id;
        if (id) {
            uint32_t p[6] = { id, window };
            vr_call(v, SEL_ENTER, p, 2);
        }
    } else if (id) {
        uint32_t p[6] = { id, window };
        vr_call(v, SEL_WITHIN, p, 2);
    }
}

/* The controller's idle: the hot spot under the mouse entered, stayed in
 * or left, as QuickTime VR's controller told the program's routines. */
void vr_idle_mouse(vr *v, uint32_t pt)
{
    if (!v->tracking_down)
        track_over(v, vr_hot_at(v, pt_h(pt), pt_v(pt)), 0);
}

static int16_t set_param(vr *v, uint32_t param, gaddr value, uint32_t flags)
{
    if (!value && param != 0x0200)
        return paramErr;
    if (param == 0x0200) { /* kQTVRHotSpotsVisible */
        v->hots_visible = value && (rd8(value) != 0);
        vr_update(v, false);
        return noErr;
    }
    if (param == 0x0104) { /* kQTVRViewCenter */
        float h = getf(value), vv = getf(value + 4);
        if (flags & 1) {
            h += v->vch;
            vv += v->vcv;
        }
        vr_set_center(v, h, vv);
        return noErr;
    }
    int kind = param == 0x0100 ? VR_PAN : param == 0x0101 ? VR_TILT : param == 0x0103 ? VR_FOV : -1;
    if (kind < 0)
        return paramErr;
    float d = in_deg(v, getf(value));
    if (flags & 2) { /* kQTVRValueIsRate */
        v->rate[kind] = d;
        return noErr;
    }
    float *cur = kind == VR_PAN ? &v->pan : kind == VR_TILT ? &v->tilt : &v->fov;
    vr_set_angle(v, kind, flags & 1 ? *cur + d : d);
    return noErr;
}

static int16_t get_param(vr *v, uint32_t param, gaddr value, uint32_t flags, gaddr flags_out)
{
    if (flags_out)
        wr32(flags_out, 0);
    if (!value)
        return paramErr;
    switch (param) {
    case 0x0100: putf(value, out_units(v, flags & 2 ? v->rate[VR_PAN] : v->pan)); break;
    case 0x0101: putf(value, out_units(v, flags & 2 ? v->rate[VR_TILT] : v->tilt)); break;
    case 0x0103: putf(value, out_units(v, flags & 2 ? v->rate[VR_FOV] : v->fov)); break;
    case 0x0104:
        putf(value, v->vch);
        putf(value + 4, v->vcv);
        break;
    case 0x0200: wr8(value, v->hots_visible); break;
    default: return paramErr;
    }
    return noErr;
}

/* A click begun, continued and ended: a panorama turns towards the mouse
 * as long as it is held away from where it went down; an object turns with
 * the mouse as it is dragged.  A click that stays on a hot spot triggers
 * it. */
static void mouse_down(vr *v, uint32_t pt, uint32_t when, gaddr id_out)
{
    v->down_pt = v->last_pt = pt;
    v->last_ticks = when ? when : tb_TickCount();
    v->moved = false;
    v->tracking_down = true;
    v->down_id = v->down_tracking ? hot_and_report(v, pt, id_out) : 0;
    if (!v->down_tracking && id_out)
        wr32(id_out, 0);
    if (v->object && v->down_state && v->down_state != v->cur_state) {
        v->cur_state = v->down_state;
        vr_update(v, false);
    }
}

static void mouse_still_down(vr *v, uint32_t pt, gaddr id_out)
{
    int w = v->box.right - v->box.left, h = v->box.bottom - v->box.top;
    int hyst = (int)(v->interaction[1] ? v->interaction[1] : 2);
    int dh = pt_h(pt) - pt_h(v->down_pt), dv = pt_v(pt) - pt_v(v->down_pt);
    if (abs(dh) > hyst || abs(dv) > hyst)
        v->moved = true;
    uint32_t now = tb_TickCount();
    uint32_t ticks = now - v->last_ticks;
    v->last_ticks = now;
    if (w > 0 && h > 0 && v->moved) {
        if (v->object) {
            float scale = v->mouse_motion_scale > 0 ? v->mouse_motion_scale : v->motion_scale;
            int mh = pt_h(pt) - pt_h(v->last_pt), mv = pt_v(pt) - pt_v(v->last_pt);
            if (v->control & 32) { /* kQTVRObjectSwapHVControlOn */
                int t = mh;
                mh = mv;
                mv = t;
            }
            float dp = (float)mh / w * scale, dt = (float)mv / h * scale;
            if (v->control & 8)
                dp = -dp;
            if (v->control & 16)
                dt = -dt;
            if (dp != 0)
                vr_set_angle(v, VR_PAN, v->pan - dp);
            if (dt != 0)
                vr_set_angle(v, VR_TILT, v->tilt + dt);
        } else {
            /* at the window's edge, a field of view a second (at speed 5) */
            float speed = (float)(v->interaction[3] ? v->interaction[3] : 5) / 5;
            float secs = (ticks ? ticks : 1) / 60.0f;
            float dp = (float)dh / (w / 2.0f) * v->fov * speed * secs, dt = (float)-dv / (h / 2.0f) * v->fov * speed * secs;
            if (dp != 0) {
                float a = v->pan - dp;
                uint32_t p[6] = { 0 };
                gaddr f = tb_NewPtr(4, false, true);
                putf(f, out_units(v, a));
                p[0] = f;
                vr_call(v, SEL_PAN, p, 1);
                tb_DisposePtr(f);
            }
            if (dt != 0) {
                gaddr f = tb_NewPtr(4, false, true);
                putf(f, out_units(v, v->tilt + dt));
                uint32_t p[6] = { f };
                vr_call(v, SEL_TILT, p, 1);
                tb_DisposePtr(f);
            }
        }
    }
    v->last_pt = pt;
    hot_and_report(v, pt, id_out);
}

static void mouse_up(vr *v, uint32_t pt, gaddr id_out)
{
    uint32_t id = hot_and_report(v, pt, id_out);
    bool click = v->tracking_down && !v->moved && id && id == v->down_id;
    v->tracking_down = false;
    if (v->object && v->cur_state != v->def_state && v->def_state) {
        v->cur_state = v->def_state;
        vr_update(v, false);
    }
    v->down_id = 0;
    if (click) {
        uint32_t p[6] = { id, 0, 0 };
        vr_call(v, SEL_TRIGGER, p, 3);
    }
}

/* A container handle's bytes (malloc'd). */
static uint8_t *container_bytes(gaddr h, uint32_t *len)
{
    if (!h || !rd32(h))
        return NULL;
    *len = (uint32_t)tb_GetHandleSize(h);
    uint8_t *b = malloc(*len ? *len : 1);
    gget(b, rd32(h), *len);
    return b;
}

int16_t vr_original(vr *v, uint32_t sel, const uint32_t *p)
{
    switch (sel) {
    case SEL_PAN:
    case SEL_TILT:
    case SEL_FOV:
        if (!p[0])
            return paramErr;
        vr_set_angle(v, kind_of(sel), in_deg(v, getf(p[0])));
        return noErr;
    case SEL_CENTER:
        if (!v->object)
            return callNotSupportedByNodeErr;
        if (!p[0])
            return paramErr;
        vr_set_center(v, getf(p[0]), getf(p[0] + 4));
        return noErr;
    case SEL_ENTER: vr_mouse_over(v, p[0], 0); return noErr;  /* kQTVRHotSpotEnter */
    case SEL_WITHIN: vr_mouse_over(v, p[0], 1); return noErr;
    case SEL_LEAVE: vr_mouse_over(v, p[0], 2); return noErr;
    case SEL_DOWN: mouse_down(v, p[0], p[1], p[3]); return noErr;
    case SEL_STILLDOWN: mouse_still_down(v, p[0], p[1]); return noErr;
    case SEL_UP: mouse_up(v, p[0], p[1]); return noErr;
    case SEL_TRIGGER: {
        uint32_t len = 0;
        uint8_t *b = container_bytes(p[1], &len);
        int16_t err = vr_trigger(v, p[0], b, len, b ? p[2] : 0);
        free(b);
        return err;
    }
    case SEL_HOTTYPE:
        if (!vr_hot_known(v, p[0]))
            return invalidHotSpotIDErr;
        if (p[1])
            wr32(p[1], vr_hot_type(v, p[0]));
        return noErr;
    case SEL_SETPARAM: return set_param(v, p[0], p[1], p[2]);
    case SEL_GETPARAM: return get_param(v, p[0], p[1], p[2], p[3]);
    }
    return selectorNotSupportedByNodeErr;
}

int16_t vr_call(vr *v, uint32_t sel, uint32_t *p, int n)
{
    if (sel < SEL_PAN || sel > SEL_LAST)
        return paramErr;
    int k = (int)(sel - SEL_PAN);
    vr_proc *ip = &v->intercepts[k];
    if (!ip->proc || v->in_intercept[k])
        return vr_original(v, sel, p);
    /* QTVRInterceptRecord: reserved1, selector, reserved2, reserved3,
     * paramCount, parameter[6] */
    gaddr msg = tb_NewPtr(44, false, true), cancel = tb_NewPtr(2, false, true);
    if (!msg || !cancel)
        return memFullErr;
    wr32(msg + 4, sel);
    wr32(msg + 16, (uint32_t)n);
    for (int i = 0; i < 6; i++)
        wr32(msg + 20 + 4 * (uint32_t)i, p[i]);
    v->in_intercept[k] = true;
    uint32_t a[] = { v->token, msg, (uint32_t)ip->refcon, cancel };
    mm_call(ip->proc, 0x00003FC0, a, 4);
    v->in_intercept[k] = false;
    int16_t err = noErr;
    if (!rd8(cancel)) {
        uint32_t q[6];
        for (int i = 0; i < 6; i++)
            q[i] = rd32(msg + 20 + 4 * (uint32_t)i);
        err = vr_original(v, sel, q);
    }
    tb_DisposePtr(msg);
    tb_DisposePtr(cancel);
    return err;
}

/* An angle set through the intercepts. */
static int16_t set_angle_call(vr *v, uint32_t sel, float x_units)
{
    gaddr f = tb_NewPtr(4, false, true);
    putf(f, x_units);
    uint32_t p[6] = { f };
    int16_t err = vr_call(v, sel, p, 1);
    tb_DisposePtr(f);
    return err;
}

/* ---- The exports ---- */

#define VR_OR(err) \
    vr *v = vr_for_token(R(0)); \
    if (!v) { \
        ret(c, (uint32_t)(int32_t)(err)); \
        return; \
    }

static void QTVRGetQTVRTrack(ppc_cpu *c) { ret(c, vr_find_track(R(0), (int32_t)R(1))); }

static void QTVRGetQTVRInstance(ppc_cpu *c)
{
    gaddr out = R(0);
    if (out)
        wr32(out, 0);
    vr *v = vr_instance(R(1), R(2));
    if (!v) {
        ret(c, (uint32_t)(int32_t)notAQTVRMovieErr);
        return;
    }
    if (out)
        wr32(out, v->token);
    ret(c, noErr);
}

static void QTVRSetViewParameter(ppc_cpu *c)
{
    VR_OR(paramErr);
    uint32_t p[6] = { R(1), R(2), R(3) };
    ret(c, (uint32_t)(int32_t)vr_call(v, SEL_SETPARAM, p, 3));
}

static void QTVRGetViewParameter(ppc_cpu *c)
{
    VR_OR(paramErr);
    uint32_t p[6] = { R(1), R(2), R(3), R(4) };
    ret(c, (uint32_t)(int32_t)vr_call(v, SEL_GETPARAM, p, 4));
}

static void QTVRSetPanAngle(ppc_cpu *c)
{
    VR_OR(paramErr);
    ret(c, (uint32_t)(int32_t)set_angle_call(v, SEL_PAN, arg_f(c, 1)));
}
static void QTVRSetTiltAngle(ppc_cpu *c)
{
    VR_OR(paramErr);
    ret(c, (uint32_t)(int32_t)set_angle_call(v, SEL_TILT, arg_f(c, 1)));
}
static void QTVRSetFieldOfView(ppc_cpu *c)
{
    VR_OR(paramErr);
    ret(c, (uint32_t)(int32_t)set_angle_call(v, SEL_FOV, arg_f(c, 1)));
}

static void QTVRGetPanAngle(ppc_cpu *c)
{
    vr *v = vr_for_token(R(0));
    retf(c, v ? out_units(v, v->pan) : 0);
}
static void QTVRGetTiltAngle(ppc_cpu *c)
{
    vr *v = vr_for_token(R(0));
    retf(c, v ? out_units(v, v->tilt) : 0);
}
static void QTVRGetFieldOfView(ppc_cpu *c)
{
    vr *v = vr_for_token(R(0));
    retf(c, v ? out_units(v, v->fov) : 0);
}

static void QTVRShowDefaultView(ppc_cpu *c)
{
    VR_OR(paramErr);
    vr_show_default(v);
    ret(c, noErr);
}

static void QTVRSetViewCenter(ppc_cpu *c)
{
    VR_OR(paramErr);
    uint32_t p[6] = { R(1) };
    ret(c, (uint32_t)(int32_t)vr_call(v, SEL_CENTER, p, 1));
}

static void QTVRGetViewCenter(ppc_cpu *c)
{
    VR_OR(paramErr);
    if (!v->object) {
        ret(c, (uint32_t)(int32_t)callNotSupportedByNodeErr);
        return;
    }
    putf(R(1), v->vch);
    if (R(1))
        putf(R(1) + 4, v->vcv);
    ret(c, noErr);
}

static void QTVRNudge(ppc_cpu *c)
{
    VR_OR(paramErr);
    uint32_t before = vr_constraint_status(v);
    float p = v->pan, t = v->tilt;
    vr_nudge(v, (int)R(1));
    ret(c, p == v->pan && t == v->tilt && before ? (uint32_t)(int32_t)constraintReachedErr : noErr);
}

static void QTVRInteractionNudge(ppc_cpu *c) { QTVRNudge(c); }

static gaddr new_container(const uint8_t *b, uint32_t len)
{
    if (!b || len < QTA_HEADER + QTA_ATOM)
        return qt_atoms_new();
    gaddr h = tb_NewHandle((int32_t)len, false, false);
    if (h && rd32(h))
        gput(rd32(h), b, len);
    return h;
}

static void QTVRGetVRWorld(ppc_cpu *c)
{
    VR_OR(paramErr);
    if (R(1))
        wr32(R(1), new_container(v->world, v->world_len));
    ret(c, noErr);
}

static int node_for(const vr *v, uint32_t id)
{
    if (id == kQTVRCurrentNode)
        return v->cur;
    return vr_node_index(v, id);
}

static void QTVRGetNodeInfo(ppc_cpu *c)
{
    VR_OR(paramErr);
    int i = node_for(v, R(1));
    if (R(2))
        wr32(R(2), 0);
    if (i < 0) {
        ret(c, (uint32_t)(int32_t)invalidNodeIDErr);
        return;
    }
    if (R(2))
        wr32(R(2), new_container(v->nodes[i].info, v->nodes[i].info_len));
    ret(c, noErr);
}

static void QTVRGoToNodeID(ppc_cpu *c)
{
    VR_OR(paramErr);
    int16_t err = vr_go_to_node(v, (int32_t)R(1));
    vr_update(v, true);
    ret(c, (uint32_t)(int32_t)err);
}

static void QTVRGetCurrentNodeID(ppc_cpu *c)
{
    vr *v = vr_for_token(R(0));
    ret(c, v && v->cur >= 0 ? v->nodes[v->cur].id : 0);
}

static void QTVRGetNodeType(ppc_cpu *c)
{
    vr *v = vr_for_token(R(0));
    int i = v ? node_for(v, R(1)) : -1;
    ret(c, i >= 0 ? v->nodes[i].type : 0);
}

static void QTVRPtToHotSpotID(ppc_cpu *c)
{
    VR_OR(paramErr);
    hot_and_report(v, R(1), R(2));
    ret(c, noErr);
}

static void QTVRGetHotSpotType(ppc_cpu *c)
{
    VR_OR(paramErr);
    uint32_t p[6] = { R(1), R(2) };
    ret(c, (uint32_t)(int32_t)vr_call(v, SEL_HOTTYPE, p, 2));
}

static void QTVRTriggerHotSpot(ppc_cpu *c)
{
    VR_OR(paramErr);
    uint32_t p[6] = { R(1), R(2), R(3) };
    ret(c, (uint32_t)(int32_t)vr_call(v, SEL_TRIGGER, p, 3));
}

static void set_proc(vr_proc *p, ppc_cpu *c)
{
    *p = (vr_proc){ R(1), (int32_t)R(2), R(3) };
}

static void QTVRSetMouseOverHotSpotProc(ppc_cpu *c)
{
    VR_OR(paramErr);
    set_proc(&v->mouse_over, c);
    ret(c, noErr);
}

static void QTVREnableHotSpot(ppc_cpu *c)
{
    VR_OR(paramErr);
    uint32_t how = R(1), value = R(2);
    bool on = (R(3) & 0xFF) != 0;
    if (how == 2) { /* kQTVRAllHotSpots */
        v->all_hots_off = !on;
        if (on)
            v->noff_ids = v->noff_types = 0;
    } else {
        uint32_t *list = how == 0 ? v->off_ids : v->off_types;
        int *n = how == 0 ? &v->noff_ids : &v->noff_types;
        if (how > 1) {
            ret(c, (uint32_t)(int32_t)paramErr);
            return;
        }
        int k = 0;
        while (k < *n && list[k] != value)
            k++;
        if (on && k < *n)
            list[k] = list[--*n];
        else if (!on && k == *n && *n < VR_MAX_DISABLED)
            list[(*n)++] = value;
    }
    vr_update(v, false);
    ret(c, noErr);
}

static void QTVRGetVisibleHotSpots(ppc_cpu *c)
{
    vr *v = vr_for_token(R(0));
    if (!v) {
        ret(c, 0);
        return;
    }
    uint32_t ids[256];
    int n = vr_visible_hots(v, ids, 256);
    gaddr h = R(1);
    if (h) {
        tb_SetHandleSize(h, n * 4);
        if (rd32(h))
            for (int i = 0; i < n && i < 256; i++)
                wr32(rd32(h) + 4 * (uint32_t)i, ids[i]);
    }
    ret(c, (uint32_t)n);
}

static void QTVRGetHotSpotRegion(ppc_cpu *c)
{
    VR_OR(paramErr);
    if (!R(2)) {
        ret(c, (uint32_t)(int32_t)paramErr);
        return;
    }
    vr_hot_region(v, R(1), R(2));
    ret(c, noErr);
}

static void QTVRSetMouseOverTracking(ppc_cpu *c)
{
    VR_OR(paramErr);
    v->over_tracking = (R(1) & 0xFF) != 0;
    ret(c, noErr);
}
static void QTVRGetMouseOverTracking(ppc_cpu *c)
{
    vr *v = vr_for_token(R(0));
    ret(c, v && v->over_tracking);
}
static void QTVRSetMouseDownTracking(ppc_cpu *c)
{
    VR_OR(paramErr);
    v->down_tracking = (R(1) & 0xFF) != 0;
    ret(c, noErr);
}
static void QTVRGetMouseDownTracking(ppc_cpu *c)
{
    vr *v = vr_for_token(R(0));
    ret(c, v && v->down_tracking);
}

static void QTVRMouseEnter(ppc_cpu *c)
{
    VR_OR(paramErr);
    uint32_t id = hot_and_report(v, R(1), R(2));
    track_over(v, id, R(3));
    ret(c, noErr);
}

static void QTVRMouseWithin(ppc_cpu *c) { QTVRMouseEnter(c); }

static void QTVRMouseLeave(ppc_cpu *c)
{
    VR_OR(paramErr);
    track_over(v, 0, R(2));
    ret(c, noErr);
}

static void QTVRMouseDown(ppc_cpu *c)
{
    VR_OR(paramErr);
    uint32_t p[6] = { R(1), R(2), R(3) & 0xFFFF, R(4), R(5) };
    ret(c, (uint32_t)(int32_t)vr_call(v, SEL_DOWN, p, 5));
}

static void QTVRMouseStillDown(ppc_cpu *c)
{
    VR_OR(paramErr);
    uint32_t p[6] = { R(1), R(2), R(3) };
    int16_t err = vr_call(v, SEL_STILLDOWN, p, 3);
    vr_update(v, v->dirty);
    ret(c, (uint32_t)(int32_t)err);
}

static void QTVRMouseUp(ppc_cpu *c)
{
    VR_OR(paramErr);
    uint32_t p[6] = { R(1), R(2), R(3) };
    int16_t err = vr_call(v, SEL_UP, p, 3);
    vr_update(v, v->dirty);
    ret(c, (uint32_t)(int32_t)err);
}

static void QTVRMouseStillDownExtended(ppc_cpu *c) { QTVRMouseStillDown(c); }
static void QTVRMouseUpExtended(ppc_cpu *c) { QTVRMouseUp(c); }

static void QTVRInstallInterceptProc(ppc_cpu *c)
{
    VR_OR(paramErr);
    uint32_t sel = R(1);
    if (sel < SEL_PAN || sel > SEL_LAST) {
        ret(c, (uint32_t)(int32_t)selectorNotSupportedByNodeErr);
        return;
    }
    v->intercepts[sel - SEL_PAN] = (vr_proc){ R(2), (int32_t)R(3), R(4) };
    ret(c, noErr);
}

static void QTVRCallInterceptedProc(ppc_cpu *c)
{
    VR_OR(paramErr);
    gaddr msg = R(1);
    if (!msg) {
        ret(c, (uint32_t)(int32_t)paramErr);
        return;
    }
    uint32_t p[6];
    for (int i = 0; i < 6; i++)
        p[i] = rd32(msg + 20 + 4 * (uint32_t)i);
    ret(c, (uint32_t)(int32_t)vr_original(v, rd32(msg + 4), p));
}

static void QTVRGetCurrentMouseMode(ppc_cpu *c)
{
    vr *v = vr_for_token(R(0));
    uint32_t m = 0;
    if (v) {
        m = 1; /* kQTVRPanning */
        if (!v->object || (v->control & 4))
            m |= 4; /* kQTVRZooming */
        if (v->object && v->translate_on_down)
            m |= 2;
    }
    ret(c, m);
}

static void QTVRSetFrameRate(ppc_cpu *c)
{
    VR_OR(paramErr);
    if (!v->object) {
        ret(c, (uint32_t)(int32_t)callNotSupportedByNodeErr);
        return;
    }
    v->frame_rate = arg_f(c, 1);
    ret(c, noErr);
}
static void QTVRGetFrameRate(ppc_cpu *c)
{
    vr *v = vr_for_token(R(0));
    retf(c, v && v->object ? v->frame_rate : 0);
}
static void QTVRSetViewRate(ppc_cpu *c)
{
    VR_OR(paramErr);
    if (!v->object) {
        ret(c, (uint32_t)(int32_t)callNotSupportedByNodeErr);
        return;
    }
    v->view_rate = arg_f(c, 1);
    ret(c, noErr);
}
static void QTVRGetViewRate(ppc_cpu *c)
{
    vr *v = vr_for_token(R(0));
    retf(c, v && v->object ? v->view_rate : 0);
}

static void QTVRSetViewCurrentTime(ppc_cpu *c)
{
    VR_OR(paramErr);
    int32_t t = (int32_t)R(1);
    if (!v->object) {
        ret(c, (uint32_t)(int32_t)callNotSupportedByNodeErr);
        return;
    }
    if (t < 0 || (uint32_t)t >= v->view_duration) {
        ret(c, (uint32_t)(int32_t)timeNotInViewErr);
        return;
    }
    v->view_time = t;
    vr_update(v, false);
    ret(c, noErr);
}
static void QTVRGetViewCurrentTime(ppc_cpu *c)
{
    vr *v = vr_for_token(R(0));
    ret(c, v && v->object ? (uint32_t)v->view_time : 0);
}
static void QTVRGetCurrentViewDuration(ppc_cpu *c)
{
    vr *v = vr_for_token(R(0));
    ret(c, v && v->object ? v->view_duration : 0);
}

static void QTVRSetViewState(ppc_cpu *c)
{
    VR_OR(paramErr);
    uint32_t type = R(1), state = R(2) & 0xFFFF;
    if (!v->object) {
        ret(c, (uint32_t)(int32_t)callNotSupportedByNodeErr);
        return;
    }
    if (state < 1 || state > v->view_states) {
        ret(c, (uint32_t)(int32_t)invalidViewStateErr);
        return;
    }
    if (type == 0)
        v->def_state = state;
    else if (type == 2)
        v->cur_state = state;
    else if (type == 3)
        v->down_state = state;
    else {
        ret(c, (uint32_t)(int32_t)paramErr);
        return;
    }
    vr_update(v, false);
    ret(c, noErr);
}

static void QTVRGetViewState(ppc_cpu *c)
{
    VR_OR(paramErr);
    uint32_t type = R(1);
    if (!v->object) {
        ret(c, (uint32_t)(int32_t)callNotSupportedByNodeErr);
        return;
    }
    uint32_t s = type == 0 ? v->def_state : type == 2 ? v->cur_state : type == 3 ? v->down_state : 0;
    if (!s) {
        ret(c, (uint32_t)(int32_t)paramErr);
        return;
    }
    if (R(2))
        wr16(R(2), (uint16_t)s);
    ret(c, noErr);
}

static void QTVRGetViewStateCount(ppc_cpu *c)
{
    vr *v = vr_for_token(R(0));
    ret(c, v && v->object ? v->view_states : 0);
}

/* The animation settings' bits in the object's animationSettings. */
static uint32_t anim_bit(uint32_t setting)
{
    switch (setting) {
    case 1: return 1 << 1;  /* kQTVRPalindromeViewFrames */
    case 2: return 1 << 2;  /* kQTVRStartFirstViewFrame */
    case 3: return 1 << 6;  /* kQTVRDontLoopViewFrames */
    case 4: return 1 << 7;  /* kQTVRPlayEveryViewFrame */
    case 16: return 1 << 5; /* kQTVRSyncViewToFrameRate */
    case 17: return 1 << 4; /* kQTVRPalindromeViews */
    case 18: return 1 << 8; /* kQTVRPlayStreamingViews */
    }
    return 0;
}

static void QTVRSetAnimationSetting(ppc_cpu *c)
{
    VR_OR(paramErr);
    uint32_t b = anim_bit(R(1));
    if (!v->object || !b) {
        ret(c, (uint32_t)(int32_t)(v->object ? settingNotSupportedByNodeErr : callNotSupportedByNodeErr));
        return;
    }
    v->anim = R(2) & 0xFF ? v->anim | b : v->anim & ~b;
    ret(c, noErr);
}

static void QTVRGetAnimationSetting(ppc_cpu *c)
{
    VR_OR(paramErr);
    uint32_t b = anim_bit(R(1));
    if (!v->object || !b) {
        ret(c, (uint32_t)(int32_t)(v->object ? settingNotSupportedByNodeErr : callNotSupportedByNodeErr));
        return;
    }
    if (R(2))
        wr8(R(2), (v->anim & b) != 0);
    ret(c, noErr);
}

static void QTVRSetControlSetting(ppc_cpu *c)
{
    VR_OR(paramErr);
    uint32_t s = R(1);
    if (!v->object || s < 1 || s > 7) {
        ret(c, (uint32_t)(int32_t)(v->object ? settingNotSupportedByNodeErr : callNotSupportedByNodeErr));
        return;
    }
    uint32_t b = 1u << (s - 1);
    v->control = R(2) & 0xFF ? v->control | b : v->control & ~b;
    if (s == 1)
        v->wraps = (v->control & 1) != 0;
    ret(c, noErr);
}

static void QTVRGetControlSetting(ppc_cpu *c)
{
    VR_OR(paramErr);
    uint32_t s = R(1);
    if (!v->object || s < 1 || s > 7) {
        ret(c, (uint32_t)(int32_t)(v->object ? settingNotSupportedByNodeErr : callNotSupportedByNodeErr));
        return;
    }
    if (R(2))
        wr8(R(2), (v->control >> (s - 1)) & 1);
    ret(c, noErr);
}

static void QTVREnableFrameAnimation(ppc_cpu *c)
{
    VR_OR(paramErr);
    if (!v->object) {
        ret(c, (uint32_t)(int32_t)callNotSupportedByNodeErr);
        return;
    }
    v->frame_anim = (R(1) & 0xFF) != 0;
    ret(c, noErr);
}
static void QTVRGetFrameAnimation(ppc_cpu *c)
{
    vr *v = vr_for_token(R(0));
    ret(c, v && v->frame_anim);
}
static void QTVREnableViewAnimation(ppc_cpu *c)
{
    VR_OR(paramErr);
    if (!v->object) {
        ret(c, (uint32_t)(int32_t)callNotSupportedByNodeErr);
        return;
    }
    v->view_anim = (R(1) & 0xFF) != 0;
    ret(c, noErr);
}
static void QTVRGetViewAnimation(ppc_cpu *c)
{
    vr *v = vr_for_token(R(0));
    ret(c, v && v->view_anim);
}

static void QTVRSetVisible(ppc_cpu *c)
{
    VR_OR(paramErr);
    v->visible = (R(1) & 0xFF) != 0;
    vr_update(v, v->visible);
    ret(c, noErr);
}
static void QTVRGetVisible(ppc_cpu *c)
{
    vr *v = vr_for_token(R(0));
    ret(c, v && v->visible);
}

static void QTVRSetImagingProperty(ppc_cpu *c)
{
    VR_OR(paramErr);
    uint32_t mode = R(1), prop = R(2);
    if (prop < 1 || prop > 7 || (mode != 1 && mode != 2 && mode != 100)) {
        ret(c, (uint32_t)(int32_t)(prop == 100 ? paramErr : propertyNotSupportedByNodeErr));
        return;
    }
    for (int m = 1; m <= 2; m++)
        if (mode == 100 || (uint32_t)m == mode)
            v->imaging[m][prop] = (int32_t)R(3);
    vr_update(v, false);
    ret(c, noErr);
}

static void QTVRGetImagingProperty(ppc_cpu *c)
{
    VR_OR(paramErr);
    uint32_t mode = R(1), prop = R(2);
    int32_t val;
    if (prop == 100) /* kQTVRImagingCurrentMode */
        val = v->mode;
    else if (prop >= 1 && prop <= 7 && (mode == 1 || mode == 2))
        val = v->imaging[mode][prop];
    else {
        ret(c, (uint32_t)(int32_t)propertyNotSupportedByNodeErr);
        return;
    }
    if (R(3))
        wr32(R(3), (uint32_t)val);
    ret(c, noErr);
}

static void QTVRUpdate(ppc_cpu *c)
{
    VR_OR(paramErr);
    uint32_t mode = R(1);
    if (mode == 1 || mode == 2)
        v->mode = (int)mode;
    vr_update(v, true);
    ret(c, noErr);
}

static void QTVRBeginUpdateStream(ppc_cpu *c)
{
    VR_OR(paramErr);
    v->streaming = true;
    if (R(1) == 1 || R(1) == 2)
        v->mode = (int)R(1);
    ret(c, noErr);
}

static void QTVREndUpdateStream(ppc_cpu *c)
{
    VR_OR(paramErr);
    v->streaming = false;
    v->mode = kQTVRStatic;
    vr_update(v, true);
    ret(c, noErr);
}

static void QTVRSetTransitionProperty(ppc_cpu *c)
{
    VR_OR(paramErr);
    if (R(1) != 1) { /* kQTVRTransitionSwing */
        ret(c, (uint32_t)(int32_t)paramErr);
        return;
    }
    v->transition_type = R(1);
    if (R(2) == 1)
        v->transition_speed = R(3);
    else if (R(2) == 2)
        v->transition_dir = R(3);
    else {
        ret(c, (uint32_t)(int32_t)paramErr);
        return;
    }
    ret(c, noErr);
}

static void QTVREnableTransition(ppc_cpu *c)
{
    VR_OR(paramErr);
    if (R(1) != 1) {
        ret(c, (uint32_t)(int32_t)paramErr);
        return;
    }
    v->transition_on = (R(2) & 0xFF) != 0;
    ret(c, noErr);
}

static void QTVRSetAngularUnits(ppc_cpu *c)
{
    VR_OR(paramErr);
    if (R(1) > 1) {
        ret(c, (uint32_t)(int32_t)paramErr);
        return;
    }
    v->units = (int)R(1);
    ret(c, noErr);
}
static void QTVRGetAngularUnits(ppc_cpu *c)
{
    vr *v = vr_for_token(R(0));
    ret(c, v ? (uint32_t)v->units : 1);
}

static void QTVRPtToAngles(ppc_cpu *c)
{
    VR_OR(paramErr);
    float p, t;
    if (!vr_pt_to_angles(v, pt_h(R(1)), pt_v(R(1)), &p, &t)) {
        ret(c, (uint32_t)(int32_t)paramErr);
        return;
    }
    putf(R(2), out_units(v, p));
    putf(R(3), out_units(v, t));
    ret(c, noErr);
}

static void QTVRCoordToAngles(ppc_cpu *c)
{
    VR_OR(paramErr);
    if (!R(1)) {
        ret(c, (uint32_t)(int32_t)paramErr);
        return;
    }
    float p, t;
    vr_angles_from_coord(v, getf(R(1)), getf(R(1) + 4), &p, &t);
    putf(R(2), out_units(v, p));
    putf(R(3), out_units(v, t));
    ret(c, noErr);
}

static void QTVRAnglesToCoord(ppc_cpu *c)
{
    VR_OR(paramErr);
    float x, y;
    vr_coord_from_angles(v, in_deg(v, arg_f(c, 1)), in_deg(v, arg_f(c, 2)), &x, &y);
    if (R(3)) {
        putf(R(3), x);
        putf(R(3) + 4, y);
    }
    ret(c, noErr);
}

static void QTVRPanToColumn(ppc_cpu *c)
{
    vr *v = vr_for_token(R(0));
    ret(c, v ? (uint32_t)(int32_t)(int16_t)vr_pan_column(v, in_deg(v, arg_f(c, 1))) : 0);
}
static void QTVRColumnToPan(ppc_cpu *c)
{
    vr *v = vr_for_token(R(0));
    retf(c, v ? out_units(v, vr_column_pan(v, (int16_t)R(1))) : 0);
}
static void QTVRTiltToRow(ppc_cpu *c)
{
    vr *v = vr_for_token(R(0));
    ret(c, v ? (uint32_t)(int32_t)(int16_t)vr_tilt_row(v, in_deg(v, arg_f(c, 1))) : 0);
}
static void QTVRRowToTilt(ppc_cpu *c)
{
    vr *v = vr_for_token(R(0));
    retf(c, v ? out_units(v, vr_row_tilt(v, (int16_t)R(1))) : 0);
}

/* kind: kQTVRPan, kQTVRTilt, kQTVRFieldOfView, or the view center's parts. */
static bool valid_kind(int16_t k) { return k == VR_PAN || k == VR_TILT || k == VR_FOV || k == VR_VCH || k == VR_VCV; }

static void QTVRWrapAndConstrain(ppc_cpu *c)
{
    VR_OR(paramErr);
    int16_t kind = (int16_t)R(1);
    float val = arg_f(c, 1);
    if (!valid_kind(kind)) {
        ret(c, (uint32_t)(int32_t)paramErr);
        return;
    }
    float r;
    if (kind == VR_VCH || kind == VR_VCV)
        r = vr_wrap_constrain(v, kind, val);
    else
        r = out_units(v, vr_wrap_constrain(v, kind, in_deg(v, val)));
    putf(R(3), r);
    ret(c, noErr);
}

static void QTVRSetEnteringNodeProc(ppc_cpu *c)
{
    VR_OR(paramErr);
    set_proc(&v->entering, c);
    ret(c, noErr);
}
static void QTVRSetLeavingNodeProc(ppc_cpu *c)
{
    VR_OR(paramErr);
    set_proc(&v->leaving, c);
    ret(c, noErr);
}

/* Interaction properties: a value passed as itself, but the mouse motion
 * scale (a pointer to a float). */
static void QTVRSetInteractionProperty(ppc_cpu *c)
{
    VR_OR(paramErr);
    uint32_t prop = R(1), val = R(2);
    bool dflt = val == 0x80000000u; /* kQTVRInteractionDefaultValue */
    switch (prop) {
    case 1: v->interaction[1] = dflt ? 2 : val & 0xFFFF; break;
    case 2: v->interaction[2] = dflt ? 30 : val; break;
    case 3:
    case 4:
        if (!dflt && (val < 1 || val > 10)) {
            ret(c, (uint32_t)(int32_t)paramErr);
            return;
        }
        v->interaction[prop] = dflt ? 5 : val;
        break;
    case 101: v->translate_on_down = !dflt && (val & 0xFF); break;
    case 102: v->mouse_motion_scale = dflt || !val ? 180 : getf(val); break;
    case 103: v->nudge_mode = dflt ? 0 : val; break;
    default:
        ret(c, (uint32_t)(int32_t)propertyNotSupportedByNodeErr);
        return;
    }
    ret(c, noErr);
}

static void QTVRGetInteractionProperty(ppc_cpu *c)
{
    VR_OR(paramErr);
    uint32_t prop = R(1);
    gaddr p = R(2);
    if (!p) {
        ret(c, (uint32_t)(int32_t)paramErr);
        return;
    }
    switch (prop) {
    case 1: wr16(p, (uint16_t)v->interaction[1]); break;
    case 2:
    case 3:
    case 4: wr32(p, v->interaction[prop]); break;
    case 101: wr8(p, v->translate_on_down); break;
    case 102: putf(p, v->mouse_motion_scale); break;
    case 103: wr32(p, v->nudge_mode); break;
    default:
        ret(c, (uint32_t)(int32_t)propertyNotSupportedByNodeErr);
        return;
    }
    ret(c, noErr);
}

/* QTVRCursorRecord: type, resource ID, handle; the cursor kept for when
 * the mouse is over the view. */
static void QTVRReplaceCursor(ppc_cpu *c)
{
    VR_OR(paramErr);
    gaddr r = R(1);
    if (!r) {
        ret(c, (uint32_t)(int32_t)paramErr);
        return;
    }
    v->cursor_type = rd16(r);
    v->cursor_id = (int16_t)rd16(r + 2);
    v->cursor_handle = rd32(r + 4);
    ret(c, noErr);
}

static void QTVRGetViewingLimits(ppc_cpu *c)
{
    VR_OR(paramErr);
    int16_t kind = (int16_t)R(1);
    float lo, hi;
    if (kind == VR_VCH || kind == VR_VCV) {
        vr_effective_limits(v, kind, &lo, &hi);
        putf(R(2), lo);
        putf(R(3), hi);
    } else if (kind >= 0 && kind <= 2) {
        lo = v->lim_min[kind] < v->lim_max[kind] ? v->lim_min[kind] : v->lim_max[kind];
        hi = v->lim_min[kind] < v->lim_max[kind] ? v->lim_max[kind] : v->lim_min[kind];
        putf(R(2), out_units(v, lo));
        putf(R(3), out_units(v, hi));
    } else {
        ret(c, (uint32_t)(int32_t)paramErr);
        return;
    }
    ret(c, noErr);
}

static void QTVRGetConstraintStatus(ppc_cpu *c)
{
    vr *v = vr_for_token(R(0));
    ret(c, v ? vr_constraint_status(v) : 0);
}

static void QTVRGetConstraints(ppc_cpu *c)
{
    VR_OR(paramErr);
    int16_t kind = (int16_t)R(1);
    if (kind < 0 || kind > 2) {
        ret(c, (uint32_t)(int32_t)paramErr);
        return;
    }
    putf(R(2), out_units(v, v->con_min[kind]));
    putf(R(3), out_units(v, v->con_max[kind]));
    ret(c, noErr);
}

static void QTVRSetConstraints(ppc_cpu *c)
{
    VR_OR(paramErr);
    int16_t kind = (int16_t)R(1);
    if (kind < 0 || kind > 2) {
        ret(c, (uint32_t)(int32_t)paramErr);
        return;
    }
    float lo = in_deg(v, arg_f(c, 1)), hi = in_deg(v, arg_f(c, 2));
    float l0 = v->lim_min[kind] < v->lim_max[kind] ? v->lim_min[kind] : v->lim_max[kind];
    float l1 = v->lim_min[kind] < v->lim_max[kind] ? v->lim_max[kind] : v->lim_min[kind];
    if (lo > hi) {
        float t = lo;
        lo = hi;
        hi = t;
    }
    /* within what the file allows */
    int16_t err = noErr;
    if (lo < l0 - 1e-4f || hi > l1 + 1e-4f)
        err = limitReachedErr;
    v->con_min[kind] = lo < l0 ? l0 : lo;
    v->con_max[kind] = hi > l1 ? l1 : hi;
    if (kind == VR_PAN)
        v->wraps = !v->object ? v->con_max[kind] - v->con_min[kind] >= 359.9f : v->wraps;
    vr_set_angle(v, VR_FOV, v->fov);
    vr_set_angle(v, VR_PAN, v->pan);
    vr_set_angle(v, VR_TILT, v->tilt);
    vr_update(v, false);
    ret(c, (uint32_t)(int32_t)err);
}

static void QTVRGetAvailableResolutions(ppc_cpu *c)
{
    VR_OR(paramErr);
    if (R(1))
        wr16(R(1), 1); /* kQTVRFullRes */
    ret(c, noErr);
}

static int32_t buffer_bytes(const vr *v, uint32_t format, uint16_t res)
{
    int bpp = format == 16 ? 2 : format == 8 ? 1 : 4;
    int shrink = res & 4 ? 4 : res & 2 ? 2 : 1;
    return (int32_t)((int64_t)(v->iw / shrink) * (v->ih / shrink) * bpp);
}

static void QTVRGetBackBufferMemInfo(ppc_cpu *c)
{
    VR_OR(paramErr);
    if (v->object) {
        ret(c, (uint32_t)(int32_t)callNotSupportedByNodeErr);
        return;
    }
    int32_t full = buffer_bytes(v, R(3), (uint16_t)R(2));
    if (R(4))
        wr32(R(4), (uint32_t)(full / 4));
    if (R(5))
        wr32(R(5), (uint32_t)full);
    if (R(6))
        wr32(R(6), (uint32_t)full);
    ret(c, noErr);
}

static void QTVRGetBackBufferSettings(ppc_cpu *c)
{
    VR_OR(paramErr);
    if (v->object) {
        ret(c, (uint32_t)(int32_t)callNotSupportedByNodeErr);
        return;
    }
    if (R(1))
        wr32(R(1), v->horizontal ? fourcc("hcyl") : v->bb_geometry);
    if (R(2))
        wr16(R(2), v->bb_resolution);
    if (R(3))
        wr32(R(3), v->bb_format);
    if (R(4))
        wr16(R(4), (uint16_t)v->bb_cache);
    ret(c, noErr);
}

static void QTVRSetBackBufferPrefs(ppc_cpu *c)
{
    VR_OR(paramErr);
    if (v->object) {
        ret(c, (uint32_t)(int32_t)callNotSupportedByNodeErr);
        return;
    }
    if (R(1))
        v->bb_geometry = R(1);
    if (R(2) & 0xFFFF)
        v->bb_resolution = (uint16_t)R(2);
    if (R(3))
        v->bb_format = R(3);
    v->bb_cache = (int16_t)R(4);
    ret(c, noErr);
}

static void QTVRSetPrescreenImagingCompleteProc(ppc_cpu *c)
{
    VR_OR(paramErr);
    set_proc(&v->prescreen, c);
    ret(c, noErr);
}

static void QTVRSetBackBufferImagingProc(ppc_cpu *c)
{
    VR_OR(paramErr);
    if (v->object) {
        ret(c, (uint32_t)(int32_t)callNotSupportedByNodeErr);
        return;
    }
    uint16_t n = (uint16_t)R(2);
    gaddr areas = R(3);
    free(v->areas);
    v->areas = NULL;
    v->nareas = 0;
    v->backbuf = (vr_proc){ R(1), (int32_t)R(4), 0 };
    if (v->backbuf.proc && n && areas) {
        v->areas = calloc(n, sizeof *v->areas);
        for (uint16_t i = 0; i < n; i++) {
            gaddr a = areas + 20u * i;
            v->areas[i] = (vr_area){ in_deg(v, getf(a)), in_deg(v, getf(a + 4)), in_deg(v, getf(a + 8)),
                                     in_deg(v, getf(a + 12)), rd32(a + 16) };
        }
        v->nareas = n;
    }
    vr_refresh_back_buffer(v);
    vr_update(v, true);
    ret(c, noErr);
}

static void QTVRRefreshBackBuffer(ppc_cpu *c)
{
    VR_OR(paramErr);
    if (v->object) {
        ret(c, (uint32_t)(int32_t)callNotSupportedByNodeErr);
        return;
    }
    vr_refresh_back_buffer(v);
    vr_update(v, true);
    ret(c, noErr);
}

/* QuickTime VR 2.0's names for the back buffer calls. */
static void QTVRGetCacheMemInfo(ppc_cpu *c)
{
    VR_OR(paramErr);
    if (v->object) {
        ret(c, (uint32_t)(int32_t)callNotSupportedByNodeErr);
        return;
    }
    int16_t depth = (int16_t)R(2);
    int32_t full = buffer_bytes(v, depth ? (uint32_t)depth : 32, (uint16_t)R(1));
    if (R(3))
        wr32(R(3), (uint32_t)(full / 4));
    if (R(4))
        wr32(R(4), (uint32_t)full);
    if (R(5))
        wr32(R(5), (uint32_t)full);
    ret(c, noErr);
}

static void QTVRGetCacheSettings(ppc_cpu *c)
{
    VR_OR(paramErr);
    if (R(1))
        wr16(R(1), v->bb_resolution);
    if (R(2))
        wr16(R(2), (uint16_t)v->bb_format);
    if (R(3))
        wr16(R(3), (uint16_t)v->bb_cache);
    ret(c, noErr);
}

static void QTVRSetCachePrefs(ppc_cpu *c)
{
    VR_OR(paramErr);
    if (R(1) & 0xFFFF)
        v->bb_resolution = (uint16_t)R(1);
    if ((int16_t)R(2) > 0)
        v->bb_format = (uint32_t)(int16_t)R(2);
    v->bb_cache = (int16_t)R(3);
    ret(c, noErr);
}

static void InitializeQTVR(ppc_cpu *c) { ret(c, noErr); }
static void TerminateQTVR(ppc_cpu *c) { ret(c, noErr); }

static const ppc_export vr_exports[] = {
#define E(n) { #n, n }
    E(InitializeQTVR), E(QTVRAnglesToCoord), E(QTVRBeginUpdateStream), E(QTVRCallInterceptedProc),
    E(QTVRColumnToPan), E(QTVRCoordToAngles), E(QTVREnableFrameAnimation), E(QTVREnableHotSpot),
    E(QTVREnableTransition), E(QTVREnableViewAnimation), E(QTVREndUpdateStream), E(QTVRGetAngularUnits),
    E(QTVRGetAnimationSetting), E(QTVRGetAvailableResolutions), E(QTVRGetBackBufferMemInfo),
    E(QTVRGetBackBufferSettings), E(QTVRGetCacheMemInfo), E(QTVRGetCacheSettings), E(QTVRGetConstraintStatus),
    E(QTVRGetConstraints), E(QTVRGetControlSetting), E(QTVRGetCurrentMouseMode), E(QTVRGetCurrentNodeID),
    E(QTVRGetCurrentViewDuration), E(QTVRGetFieldOfView), E(QTVRGetFrameAnimation), E(QTVRGetFrameRate),
    E(QTVRGetHotSpotRegion), E(QTVRGetHotSpotType), E(QTVRGetImagingProperty), E(QTVRGetInteractionProperty),
    E(QTVRGetMouseDownTracking), E(QTVRGetMouseOverTracking), E(QTVRGetNodeInfo), E(QTVRGetNodeType),
    E(QTVRGetPanAngle), E(QTVRGetQTVRInstance), E(QTVRGetQTVRTrack), E(QTVRGetTiltAngle), E(QTVRGetVRWorld),
    E(QTVRGetViewAnimation), E(QTVRGetViewCenter), E(QTVRGetViewCurrentTime), E(QTVRGetViewParameter),
    E(QTVRGetViewRate), E(QTVRGetViewState), E(QTVRGetViewStateCount), E(QTVRGetViewingLimits), E(QTVRGetVisible),
    E(QTVRGetVisibleHotSpots), E(QTVRGoToNodeID), E(QTVRInstallInterceptProc), E(QTVRInteractionNudge),
    E(QTVRMouseDown), E(QTVRMouseEnter), E(QTVRMouseLeave), E(QTVRMouseStillDown), E(QTVRMouseStillDownExtended),
    E(QTVRMouseUp), E(QTVRMouseUpExtended), E(QTVRMouseWithin), E(QTVRNudge), E(QTVRPanToColumn), E(QTVRPtToAngles),
    E(QTVRPtToHotSpotID), E(QTVRRefreshBackBuffer), E(QTVRReplaceCursor), E(QTVRRowToTilt), E(QTVRSetAngularUnits),
    E(QTVRSetAnimationSetting), E(QTVRSetBackBufferImagingProc), E(QTVRSetBackBufferPrefs), E(QTVRSetCachePrefs),
    E(QTVRSetConstraints), E(QTVRSetControlSetting), E(QTVRSetEnteringNodeProc), E(QTVRSetFieldOfView),
    E(QTVRSetFrameRate), E(QTVRSetImagingProperty), E(QTVRSetInteractionProperty), E(QTVRSetLeavingNodeProc),
    E(QTVRSetMouseDownTracking), E(QTVRSetMouseOverHotSpotProc), E(QTVRSetMouseOverTracking), E(QTVRSetPanAngle),
    E(QTVRSetPrescreenImagingCompleteProc), E(QTVRSetTiltAngle), E(QTVRSetTransitionProperty),
    E(QTVRSetViewCenter), E(QTVRSetViewCurrentTime), E(QTVRSetViewParameter), E(QTVRSetViewRate),
    E(QTVRSetViewState), E(QTVRSetVisible), E(QTVRShowDefaultView), E(QTVRTiltToRow), E(QTVRTriggerHotSpot),
    E(QTVRUpdate), E(QTVRWrapAndConstrain), E(TerminateQTVR),
#undef E
};

void (*qt_vr_export_fn(const char *name))(ppc_cpu *)
{
    if (strncmp(name, "QTVR", 4) && strcmp(name, "InitializeQTVR") && strcmp(name, "TerminateQTVR"))
        return NULL;
    for (unsigned i = 0; i < sizeof vr_exports / sizeof vr_exports[0]; i++)
        if (!strcmp(vr_exports[i].name, name))
            return vr_exports[i].fn;
    return NULL;
}
