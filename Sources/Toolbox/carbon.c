/*
 * CarbonLib's additions that sit on what is here: the Window Manager 2.0
 * accessors (collapsing, regions, features), and the Control Manager's
 * activation and visibility calls.  (InterfaceLib 8.x and its companions
 * export some of the same names.)  Core Foundation is carbon_cf*.c's.
 */
#include <stdlib.h>
#include <string.h>

#include "forks.h"
#include "ppc_bridge.h"
#include "qd_internal.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

/* ---- Windows ---- */

enum { errWindowRegionCodeInvalid = -30593, errInvalidWindowRef = -5600 };

static void wm_IsWindowCollapsed(ppc_cpu *c) { RETURN(R(0) && wm_is_collapsed(R(0))); }

/* CollapseWindow(window, collapse) */
static void wm_CollapseWindow(ppc_cpu *c)
{
    if (!R(0)) {
        RETURN((uint32_t)errInvalidWindowRef);
        return;
    }
    RETURN(wm_set_collapsed(R(0), (R(1) & 0xFF) != 0) ? 0 : (uint32_t)(int32_t)-5602); /* errWindowDoesNotHaveProxy: no collapse box */
}

static void wm_GetWindowFeatures(ppc_cpu *c)
{
    if (R(1))
        wr32(R(1), R(0) ? wm_window_features(R(0)) : 0);
    RETURN(R(0) ? 0 : (uint32_t)errInvalidWindowRef);
}

/* GetWindowRegion(window, regionCode, rgn): the structure, content and
 * update regions, the port's bounds, and the title bar (its drag region
 * too) between the structure's top and the content's. */
static void wm_GetWindowRegion(ppc_cpu *c)
{
    gaddr w = R(0), rgn = R(2);
    uint16_t code = (uint16_t)R(1);
    if (!w || !rgn) {
        RETURN((uint32_t)errInvalidWindowRef);
        return;
    }
    gaddr struc = WindowRecord_strucRgn(w), cont = WindowRecord_contRgn(w);
    switch (code) {
    case 32: /* kWindowStructureRgn */
        tb_CopyRgn(struc, rgn);
        break;
    case 33: /* kWindowContentRgn: empty when collapsed */
        if (wm_is_collapsed(w))
            tb_SetEmptyRgn(rgn);
        else
            tb_CopyRgn(cont, rgn);
        break;
    case 34: /* kWindowUpdateRgn */
        tb_CopyRgn(WindowRecord_updateRgn(w), rgn);
        break;
    case 40: { /* kWindowGlobalPortRgn */
        qrect r = qrect_get(w + GrafPort_portRect_off);
        pixdst d = qd_port_pixels(w);
        tb_SetRectRgn(rgn, (int16_t)(r.left - d.bounds.left), (int16_t)(r.top - d.bounds.top),
                      (int16_t)(r.right - d.bounds.left), (int16_t)(r.bottom - d.bounds.top));
        break;
    }
    case 0: /* kWindowTitleBarRgn */
    case 5: { /* kWindowDragRgn */
        qrect s = qrect_get(rd32(struc) + 2), k = qrect_get(rd32(cont) + 2);
        if (!(wm_window_features(w) & 64) || k.top <= s.top)
            tb_SetEmptyRgn(rgn);
        else
            tb_SetRectRgn(rgn, s.left, s.top, s.right, k.top);
        break;
    }
    default:
        RETURN((uint32_t)errWindowRegionCodeInvalid);
        return;
    }
    RETURN(0);
}

/* ---- Controls ---- */

static void cm_ActivateControl(ppc_cpu *c)
{
    if (R(0))
        tb_HiliteControl(R(0), 0);
    RETURN(0);
}
static void cm_DeactivateControl(ppc_cpu *c)
{
    if (R(0))
        tb_HiliteControl(R(0), 255);
    RETURN(0);
}
static void cm_IsControlActive(ppc_cpu *c) { RETURN(R(0) && rd32(R(0)) && ControlRecord_contrlHilite(rd32(R(0))) != 255); }
static void cm_IsControlVisible(ppc_cpu *c) { RETURN(R(0) && rd32(R(0)) && ControlRecord_contrlVis(rd32(R(0))) != 0); }

/* SetControlVisibility(control, isVisible, doDraw): ShowControl and
 * HideControl when drawing, else the flag alone. */
static void cm_SetControlVisibility(ppc_cpu *c)
{
    gaddr h = R(0);
    bool vis = (R(1) & 0xFF) != 0, draw = (R(2) & 0xFF) != 0;
    if (h && rd32(h)) {
        if (draw)
            vis ? tb_ShowControl(h) : tb_HideControl(h);
        else
            wr8(rd32(h) + 16, vis ? 0xFF : 0); /* contrlVis */
    }
    RETURN(0);
}

void carbon_reset(void) { carbon_cf_reset(); }

const ppc_export carbon_exports[] = {
    { "CollapseWindow", wm_CollapseWindow },
    { "GetWindowFeatures", wm_GetWindowFeatures },
    { "GetWindowRegion", wm_GetWindowRegion },
    { "IsWindowCollapsed", wm_IsWindowCollapsed },
    { "ActivateControl", cm_ActivateControl },
    { "DeactivateControl", cm_DeactivateControl },
    { "IsControlActive", cm_IsControlActive },
    { "IsControlVisible", cm_IsControlVisible },
    { "SetControlVisibility", cm_SetControlVisibility },
};
const unsigned carbon_export_count = sizeof carbon_exports / sizeof carbon_exports[0];
