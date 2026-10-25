/*
 * CarbonLib's QuickDraw: the accessors that stand in for reaching into
 * ports, regions, pixel maps and the QuickDraw globals, ports made and
 * disposed of without a record of the program's own, regions flattened
 * into handles and taken apart into rectangles, and the port buffering
 * Mac OS X has (on Mac OS 9 nothing is buffered: drawing reaches the
 * screen as it is done, so there is never anything to flush).
 */
#include <stdlib.h>
#include <string.h>

#define CARBON_HI_EXPORTS
#include "carbon_hi.h"
#include "qd_internal.h"
#include "tb_internal.h"


static bool is_color(gaddr port) { return qd_port_is_color(port); }

/* A colour port's GrafVars (rgbOpColor, rgbHiliteColor ...), or 0. */
static gaddr graf_vars(gaddr port)
{
    gaddr h = is_color(port) ? rd32(port + CGrafPort_grafVars_off) : 0;
    return h && rd32(h) ? rd32(h) : 0;
}

/* Run a classic call on another port, the current one kept. */
#define ON_PORT(port, call)                                                                                            \
    do {                                                                                                               \
        gaddr saved_ = qd_port();                                                                                      \
        tb_SetPort(port);                                                                                              \
        call;                                                                                                          \
        tb_SetPort(saved_);                                                                                            \
    } while (0)

/* ---- Ports ---- */

/* CreateNewPort(): a colour port of its own, current. */
static void qd_CreateNewPort(ppc_cpu *c)
{
    gaddr p = tb_NewPtr(CGrafPort_SIZEOF, false, true);
    if (p) {
        gaddr saved = qd_port();
        tb_OpenCPort(p);
        tb_SetPort(saved ? saved : p);
    }
    RETURN(p);
}

static void qd_DisposePort(ppc_cpu *c)
{
    gaddr p = R(0);
    if (p) {
        if (qd_port() == p)
            tb_SetPort(rd32(LM_WMgrCPort) ? rd32(LM_WMgrCPort) : rd32(LM_WMgrPort));
        if (is_color(p))
            tb_CloseCPort(p);
        else
            tb_ClosePort(p);
        tb_DisposePtr(p);
    }
    DONE();
}

/* Whether a port is open: in the PortList (windows', GWorlds' and
 * every other port OpenPort or OpenCPort made). */
static bool port_open(gaddr p)
{
    gaddr h = rd32(LM_PortList);
    if (!p || !h || !rd32(h))
        return false;
    int n = (int16_t)rd16(rd32(h));
    for (int i = 0; i < n; i++)
        if (rd32(rd32(h) + 2 + (uint32_t)i * 4) == p)
            return true;
    return false;
}

static void qd_IsValidPort(ppc_cpu *c) { RETURN(port_open(R(0))); }
static void qd_IsPortColor(ppc_cpu *c) { RETURN(R(0) && is_color(R(0))); }

/* Offscreen: drawing into memory other than the screen's. */
static void qd_IsPortOffscreen(ppc_cpu *c)
{
    if (!R(0)) {
        RETURN(0);
        return;
    }
    gaddr base = qd_port_pixels(R(0)).base;
    RETURN(base != GA_SCREEN && base != rd32(LM_ScrnBase));
}

static void qd_IsPortPictureBeingDefined(ppc_cpu *c) { RETURN(R(0) && GrafPort_picSave(R(0))); }
static void qd_IsPortPolyBeingDefined(ppc_cpu *c) { RETURN(R(0) && GrafPort_polySave(R(0))); }
static void qd_IsPortRegionBeingDefined(ppc_cpu *c) { RETURN(R(0) && GrafPort_rgnSave(R(0))); }

/* GetPortBitMapForCopyBits(port): the port's portBits, which in a colour
 * port is its pixel map handle and version, as CopyBits takes them. */
static void qd_GetPortBitMapForCopyBits(ppc_cpu *c) { RETURN(R(0) ? R(0) + GrafPort_portBits_off : 0); }

/* GetPortPixMap(port): a colour port's pixel map; for an old port, one
 * kept describing its bitmap. */
static struct {
    gaddr port, pm;
} bw_pixmaps[16];

static gaddr bw_pixmap(gaddr port)
{
    int slot = -1;
    for (int i = 0; i < 16 && slot < 0; i++)
        if (bw_pixmaps[i].port == port)
            slot = i;
    for (int i = 0; i < 16 && slot < 0; i++)
        if (!bw_pixmaps[i].port || !port_open(bw_pixmaps[i].port))
            slot = i;
    if (slot < 0)
        slot = 0;
    if (bw_pixmaps[slot].port != port || !bw_pixmaps[slot].pm) {
        if (!bw_pixmaps[slot].pm)
            bw_pixmaps[slot].pm = tb_NewPixMap();
        bw_pixmaps[slot].port = port;
    }
    gaddr pmh = bw_pixmaps[slot].pm;
    if (!pmh || !rd32(pmh))
        return 0;
    pixdst d = qd_port_pixels(port);
    gaddr pm = rd32(pmh);
    PixMap_set_baseAddr(pm, d.base);
    PixMap_set_rowBytes(pm, (int16_t)(d.row_bytes | 0x8000));
    qrect_put(pm + PixMap_bounds_off, d.bounds);
    PixMap_set_pixelSize(pm, (int16_t)d.depth);
    PixMap_set_cmpSize(pm, (int16_t)d.depth);
    PixMap_set_cmpCount(pm, 1);
    return pmh;
}

static void qd_GetPortPixMap(ppc_cpu *c)
{
    gaddr p = R(0);
    RETURN(!p ? 0 : is_color(p) ? rd32(p + CGrafPort_portPixMap_off) : bw_pixmap(p));
}

static void qd_GetPortBounds(ppc_cpu *c)
{
    if (R(0) && R(1))
        gmemcpy(R(1), R(0) + GrafPort_portRect_off, 8);
    RETURN(R(1));
}

static void qd_SetPortBounds(ppc_cpu *c)
{
    if (R(0) && R(1))
        gmemcpy(R(0) + GrafPort_portRect_off, R(1), 8);
    DONE();
}

static void qd_GetPortVisibleRegion(ppc_cpu *c)
{
    if (R(0) && R(1))
        tb_CopyRgn(GrafPort_visRgn(R(0)), R(1));
    RETURN(R(1));
}

static void qd_SetPortVisibleRegion(ppc_cpu *c)
{
    if (R(0) && R(1))
        tb_CopyRgn(R(1), GrafPort_visRgn(R(0)));
    DONE();
}

static void qd_GetPortClipRegion(ppc_cpu *c)
{
    if (R(0) && R(1))
        tb_CopyRgn(GrafPort_clipRgn(R(0)), R(1));
    RETURN(R(1));
}

static void qd_SetPortClipRegion(ppc_cpu *c)
{
    if (R(0) && R(1))
        tb_CopyRgn(R(1), GrafPort_clipRgn(R(0)));
    DONE();
}

/* The colours: an old port's from its old-style colour numbers. */
static void qd_GetPortForeColor(ppc_cpu *c)
{
    if (R(0) && R(1))
        ON_PORT(R(0), tb_GetForeColor(R(1)));
    RETURN(R(1));
}

static void qd_GetPortBackColor(ppc_cpu *c)
{
    if (R(0) && R(1))
        ON_PORT(R(0), tb_GetBackColor(R(1)));
    RETURN(R(1));
}

static void qd_GetPortOpColor(ppc_cpu *c)
{
    if (R(0) && R(1)) {
        uint16_t rgb[3];
        qd_port_op_color(R(0), rgb);
        for (int i = 0; i < 3; i++)
            wr16(R(1) + (uint32_t)i * 2, rgb[i]);
    }
    RETURN(R(1));
}

static void qd_SetPortOpColor(ppc_cpu *c)
{
    if (R(0) && R(1))
        ON_PORT(R(0), tb_OpColor(R(1)));
    DONE();
}

/* The highlight colour: a colour port's GrafVars', else the system's. */
static void qd_GetPortHiliteColor(ppc_cpu *c)
{
    gaddr gv = R(0) ? graf_vars(R(0)) : 0;
    if (R(1))
        gmemcpy(R(1), gv ? gv + 6 : LM_HiliteRGB, 6);
    RETURN(R(1));
}

/* The pixel patterns, copied into the caller's: an old port's from its
 * eight-byte patterns. */
static gaddr port_pattern(gaddr port, uint32_t color_off, uint32_t bw_off, gaddr out)
{
    if (!port || !out)
        return out;
    if (is_color(port)) {
        gaddr pp = rd32(port + color_off);
        if (pp && rd32(pp))
            tb_CopyPixPat(pp, out);
    } else {
        uint8_t pat[8];
        gget(pat, port + bw_off, 8);
        gaddr pp = qd_pixpat_for(pat);
        if (pp) {
            tb_CopyPixPat(pp, out);
            tb_DisposeHandle(pp);
        }
    }
    return out;
}

static void qd_GetPortBackPixPat(ppc_cpu *c)
{
    RETURN(port_pattern(R(0), CGrafPort_bkPixPat_off, GrafPort_bkPat_off, R(1)));
}
static void qd_GetPortPenPixPat(ppc_cpu *c)
{
    RETURN(port_pattern(R(0), CGrafPort_pnPixPat_off, GrafPort_pnPat_off, R(1)));
}
static void qd_GetPortFillPixPat(ppc_cpu *c)
{
    RETURN(port_pattern(R(0), CGrafPort_fillPixPat_off, GrafPort_fillPat_off, R(1)));
}

static void qd_SetPortBackPixPat(ppc_cpu *c)
{
    if (R(0) && R(1))
        ON_PORT(R(0), tb_BackPixPat(R(1)));
    DONE();
}

static void qd_SetPortPenPixPat(ppc_cpu *c)
{
    if (R(0) && R(1))
        ON_PORT(R(0), tb_PenPixPat(R(1)));
    DONE();
}

/* The fill pattern (FillRect and the rest set it as they fill). */
static void qd_SetPortFillPixPat(ppc_cpu *c)
{
    gaddr port = R(0), pp = R(1);
    if (port && pp && rd32(pp)) {
        if (is_color(port) && rd32(port + CGrafPort_fillPixPat_off))
            tb_CopyPixPat(pp, rd32(port + CGrafPort_fillPixPat_off));
        else if (!is_color(port))
            gmemcpy(port + GrafPort_fillPat_off, rd32(pp) + PixPat_pat1Data_off, 8);
    }
    DONE();
}

/* The pen and text fields. */
static void qd_GetPortPenLocation(ppc_cpu *c)
{
    if (R(0) && R(1))
        wr32(R(1), rd32(R(0) + GrafPort_pnLoc_off));
    RETURN(R(1));
}

static void qd_GetPortPenSize(ppc_cpu *c)
{
    if (R(0) && R(1))
        wr32(R(1), rd32(R(0) + GrafPort_pnSize_off));
    RETURN(R(1));
}

static void qd_SetPortPenSize(ppc_cpu *c)
{
    if (R(0))
        wr32(R(0) + GrafPort_pnSize_off, R(1));
    DONE();
}

static void qd_GetPortPenMode(ppc_cpu *c) { RETURN(R(0) ? (int32_t)(int16_t)GrafPort_pnMode(R(0)) : 0); }

static void qd_SetPortPenMode(ppc_cpu *c)
{
    if (R(0))
        GrafPort_set_pnMode(R(0), (int16_t)R(1));
    DONE();
}

static void qd_GetPortPenVisibility(ppc_cpu *c) { RETURN(R(0) ? (int32_t)(int16_t)GrafPort_pnVis(R(0)) : 0); }

static void qd_GetPortFracHPenLocation(ppc_cpu *c)
{
    RETURN(R(0) && is_color(R(0)) ? (int32_t)(int16_t)rd16(R(0) + CGrafPort_pnLocHFrac_off) : 0);
}

static void qd_SetPortFracHPenLocation(ppc_cpu *c)
{
    if (R(0) && is_color(R(0)))
        wr16(R(0) + CGrafPort_pnLocHFrac_off, (uint16_t)R(1));
    DONE();
}

static void qd_GetPortTextFont(ppc_cpu *c) { RETURN(R(0) ? (int32_t)(int16_t)GrafPort_txFont(R(0)) : 0); }
static void qd_GetPortTextFace(ppc_cpu *c) { RETURN(R(0) ? rd8(R(0) + GrafPort_txFace_off) : 0); }
static void qd_GetPortTextMode(ppc_cpu *c) { RETURN(R(0) ? (int32_t)(int16_t)GrafPort_txMode(R(0)) : 0); }
static void qd_GetPortTextSize(ppc_cpu *c) { RETURN(R(0) ? (int32_t)(int16_t)GrafPort_txSize(R(0)) : 0); }
static void qd_GetPortSpExtra(ppc_cpu *c) { RETURN(R(0) ? rd32(R(0) + GrafPort_spExtra_off) : 0); }

static void qd_GetPortChExtra(ppc_cpu *c)
{
    RETURN(R(0) && is_color(R(0)) ? (int32_t)(int16_t)rd16(R(0) + CGrafPort_chExtra_off) : 0);
}

/* SetPortTextFont and the rest (CarbonLib 1.3). */
static void qd_SetPortTextFont(ppc_cpu *c)
{
    if (R(0))
        GrafPort_set_txFont(R(0), (int16_t)R(1));
    DONE();
}

static void qd_SetPortTextFace(ppc_cpu *c)
{
    if (R(0))
        wr8(R(0) + GrafPort_txFace_off, (uint8_t)R(1));
    DONE();
}

static void qd_SetPortTextMode(ppc_cpu *c)
{
    if (R(0))
        GrafPort_set_txMode(R(0), (int16_t)R(1));
    DONE();
}

static void qd_SetPortTextSize(ppc_cpu *c)
{
    if (R(0))
        GrafPort_set_txSize(R(0), (int16_t)R(1));
    DONE();
}

static void qd_GetPortGrafProcs(ppc_cpu *c) { RETURN(R(0) ? GrafPort_grafProcs(R(0)) : 0); }

static void qd_SetPortGrafProcs(ppc_cpu *c)
{
    if (R(0))
        GrafPort_set_grafProcs(R(0), R(1));
    DONE();
}

/* SwapPortPicSaveHandle(port, inPicSaveHdl): the old one back. */
static void qd_SwapPortPicSaveHandle(ppc_cpu *c)
{
    gaddr old = R(0) ? GrafPort_picSave(R(0)) : 0;
    if (R(0))
        GrafPort_set_picSave(R(0), R(1));
    RETURN(old);
}

/* QDSwapPort(inNewPort, outOldPort): whether the port changed. */
static void qd_QDSwapPort(ppc_cpu *c)
{
    gaddr old = qd_port();
    if (R(1))
        wr32(R(1), old);
    if (old != R(0))
        tb_SetPort(R(0));
    RETURN(old != R(0));
}

static void qd_IsPortVisibleRegionEmpty(ppc_cpu *c) { RETURN(!R(0) || tb_EmptyRgn(GrafPort_visRgn(R(0)))); }
static void qd_IsPortClipRegionEmpty(ppc_cpu *c) { RETURN(!R(0) || tb_EmptyRgn(GrafPort_clipRgn(R(0)))); }

/* SectRegionWithPortClipRegion(port, ioRegion) */
static void qd_SectRegionWithPortClipRegion(ppc_cpu *c)
{
    if (R(0) && R(1))
        tb_SectRgn(R(1), GrafPort_clipRgn(R(0)), R(1));
    DONE();
}

static void qd_SectRegionWithPortVisibleRegion(ppc_cpu *c)
{
    if (R(0) && R(1))
        tb_SectRgn(R(1), GrafPort_visRgn(R(0)), R(1));
    DONE();
}

/* LockPortBits / UnlockPortBits: an offscreen port's pixels held still. */
static void qd_LockPortBits(ppc_cpu *c)
{
    if (!R(0)) {
        RETURN((uint32_t)(int32_t)paramErr);
        return;
    }
    if (is_color(R(0)))
        tb_LockPixels(rd32(R(0) + CGrafPort_portPixMap_off));
    RETURN(noErr);
}

static void qd_UnlockPortBits(ppc_cpu *c)
{
    if (!R(0)) {
        RETURN((uint32_t)(int32_t)paramErr);
        return;
    }
    if (is_color(R(0)))
        tb_UnlockPixels(rd32(R(0) + CGrafPort_portPixMap_off));
    RETURN(noErr);
}

/* Buffering: none. */
static void qd_QDIsPortBuffered(ppc_cpu *c) { RETURN(0); }
static void qd_QDIsPortBufferDirty(ppc_cpu *c) { RETURN(0); }
static void qd_QDFlushPortBuffer(ppc_cpu *c) { DONE(); }

static void qd_QDGetDirtyRegion(ppc_cpu *c)
{
    if (R(1))
        tb_SetEmptyRgn(R(1));
    RETURN(R(0) ? noErr : (uint32_t)(int32_t)paramErr);
}

static void qd_dirty_noErr(ppc_cpu *c) { RETURN(R(0) ? noErr : (uint32_t)(int32_t)paramErr); }

/* ---- Pixel maps and regions ---- */

static void qd_GetPixBounds(ppc_cpu *c)
{
    if (R(0) && rd32(R(0)) && R(1))
        gmemcpy(R(1), rd32(R(0)) + PixMap_bounds_off, 8);
    RETURN(R(1));
}

static void qd_GetPixDepth(ppc_cpu *c) { RETURN(R(0) && rd32(R(0)) ? (int32_t)PixMap_pixelSize(rd32(R(0))) : 0); }

static void qd_GetRegionBounds(ppc_cpu *c)
{
    if (R(0) && rd32(R(0)) && R(1))
        gmemcpy(R(1), rd32(R(0)) + 2, 8);
    RETURN(R(1));
}

/* Rectangular: the bounding box alone (an empty region is one too). */
static void qd_IsRegionRectangular(ppc_cpu *c) { RETURN(R(0) && rd32(R(0)) && rd16(rd32(R(0))) == 10); }

/* RgnToHandle(region, flattenedRgnDataHdl) and HandleToRgn(oldRegion,
 * region): a region's data, as it is in its handle, in another handle. */
static void qd_RgnToHandle(ppc_cpu *c)
{
    gaddr rgn = R(0), h = R(1);
    if (rgn && rd32(rgn) && h) {
        uint32_t n = rd16(rd32(rgn));
        tb_SetHandleSize(h, (int32_t)n);
        if (tb_GetHandleSize(h) >= (int32_t)n)
            gmemcpy(rd32(h), rd32(rgn), n);
    }
    DONE();
}

static void qd_HandleToRgn(ppc_cpu *c)
{
    gaddr h = R(0), rgn = R(1);
    if (h && rd32(h) && rgn && tb_GetHandleSize(h) >= 10) {
        uint32_t n = rd16(rd32(h));
        if (n < 10 || (int32_t)n > tb_GetHandleSize(h))
            n = 10;
        tb_SetHandleSize(rgn, (int32_t)n);
        if (tb_GetHandleSize(rgn) >= (int32_t)n) {
            gmemcpy(rd32(rgn), rd32(h), n);
            wr16(rd32(rgn), (uint16_t)n);
        }
    }
    DONE();
}

/* QDRegionToRects(rgn, dir, proc, userData): the region's rectangles to
 * proc, kQDRegionToRectsMsgInit first and Terminate last, top to bottom
 * (or bottom to top) and left to right (or right to left). */
enum { kQDRegionToRectsMsgInit = 1, kQDRegionToRectsMsgParse, kQDRegionToRectsMsgTerminate };
enum { kQDParseRegionFromBottom = 1 << 1, kQDParseRegionFromRight = 1 << 3 };
enum { uppRegionToRectsProcInfo = 0x00003FB1 };

static void qd_QDRegionToRects(ppc_cpu *c)
{
    gaddr rgn = R(0), proc = R(2), ref = R(3);
    uint32_t dir = R(1);
    if (!rgn || !rd32(rgn) || !proc) {
        RETURN((uint32_t)(int32_t)paramErr);
        return;
    }
    gaddr r = tb_NewPtr(8, false, true);
    if (!r) {
        RETURN((uint32_t)(int32_t)memFullErr);
        return;
    }
    hrgn h;
    hrgn_init(&h);
    rgn_read(rgn, &h);
    uint32_t args[4] = { kQDRegionToRectsMsgInit, rgn, r, ref };
    int32_t err = (int32_t)mm_call(proc, uppRegionToRectsProcInfo, args, 4);
    bool up = dir & kQDParseRegionFromBottom, back = dir & kQDParseRegionFromRight;
    for (int b = 0; !err && b < h.nbands; b++) {
        rband *band = &h.bands[up ? h.nbands - 1 - b : b];
        int n = band->n / 2;
        for (int i = 0; !err && i < n; i++) {
            int k = back ? n - 1 - i : i;
            qrect_put(r, (qrect){ band->top, band->x[2 * k], band->bottom, band->x[2 * k + 1] });
            args[0] = kQDRegionToRectsMsgParse;
            err = (int32_t)mm_call(proc, uppRegionToRectsProcInfo, args, 4);
        }
    }
    args[0] = kQDRegionToRectsMsgTerminate;
    int32_t end = (int32_t)mm_call(proc, uppRegionToRectsProcInfo, args, 4);
    hrgn_free(&h);
    tb_DisposePtr(r);
    RETURN((uint32_t)(err ? err : end));
}

/* QDSaveRegionBits(region): the screen's pixels under it in a handle;
 * QDRestoreRegionBits(region, bits) puts them back inside the region;
 * QDDisposeRegionBits(bits). The handle: the bounds, depth and row
 * width, then the rows. */
enum { SAVED_HEADER = 12 };

static pixdst screen_pixels(void)
{
    gaddr wm = rd32(LM_WMgrCPort) ? rd32(LM_WMgrCPort) : rd32(LM_WMgrPort);
    return qd_port_pixels(wm);
}

static void qd_QDSaveRegionBits(ppc_cpu *c)
{
    if (!R(0) || !rd32(R(0))) {
        RETURN(0);
        return;
    }
    pixdst s = screen_pixels();
    qrect b = qrect_get(rd32(R(0)) + 2);
    if (b.top < s.bounds.top)
        b.top = s.bounds.top;
    if (b.left < s.bounds.left)
        b.left = s.bounds.left;
    if (b.bottom > s.bounds.bottom)
        b.bottom = s.bounds.bottom;
    if (b.right > s.bounds.right)
        b.right = s.bounds.right;
    if (qrect_empty(b))
        b = (qrect){ 0, 0, 0, 0 };
    int w = b.right - b.left, hgt = b.bottom - b.top;
    uint32_t row = (uint32_t)(w * s.depth + 7) / 8;
    gaddr h = tb_NewHandle((int32_t)(SAVED_HEADER + row * (uint32_t)hgt), false, true);
    if (!h) {
        RETURN(0);
        return;
    }
    gaddr p = rd32(h);
    qrect_put(p, b);
    wr16(p + 8, (uint16_t)s.depth);
    wr16(p + 10, (uint16_t)row);
    pixdst d = { p + SAVED_HEADER, row, b, s.depth, 0 };
    for (int y = 0; y < hgt; y++)
        for (int x = 0; x < w; x++)
            put_px(&d, x, y, get_px(&s, b.left - s.bounds.left + x, b.top - s.bounds.top + y));
    RETURN(h);
}

static void qd_QDRestoreRegionBits(ppc_cpu *c)
{
    gaddr rgn = R(0), h = R(1);
    if (!rgn || !rd32(rgn) || !h || !rd32(h) || tb_GetHandleSize(h) < SAVED_HEADER) {
        RETURN((uint32_t)(int32_t)paramErr);
        return;
    }
    pixdst s = screen_pixels();
    gaddr p = rd32(h);
    qrect b = qrect_get(p);
    uint32_t row = rd16(p + 10);
    int w = b.right - b.left, hgt = b.bottom - b.top;
    if ((int)rd16(p + 8) != s.depth || tb_GetHandleSize(h) < (int32_t)(SAVED_HEADER + row * (uint32_t)(hgt > 0 ? hgt : 0))) {
        RETURN((uint32_t)(int32_t)paramErr);
        return;
    }
    hrgn shape;
    hrgn_init(&shape);
    rgn_read(rgn, &shape);
    pixdst d = { p + SAVED_HEADER, row, b, s.depth, 0 };
    for (int y = 0; y < hgt; y++)
        for (int x = 0; x < w; x++)
            if (hrgn_contains(&shape, b.left + x, b.top + y))
                put_px(&s, b.left - s.bounds.left + x, b.top - s.bounds.top + y, get_px(&d, x, y));
    hrgn_free(&shape);
    display_note_screen_draw(b);
    RETURN(noErr);
}

static void qd_QDDisposeRegionBits(ppc_cpu *c)
{
    if (R(0))
        tb_DisposeHandle(R(0));
    RETURN(noErr);
}

/* ---- The QuickDraw globals ---- */

static gaddr qd_globals(void)
{
    gaddr a5 = rd32(LM_CurrentA5);
    gaddr g = a5 ? rd32(a5) : 0;
    return g ? g - QDGlobals_thePort_off : 0;
}

static void copy_global(ppc_cpu *c, uint32_t off, uint32_t size)
{
    gaddr g = qd_globals();
    if (g && R(0))
        gmemcpy(R(0), g + off, size);
    RETURN(R(0));
}

static void qd_GetQDGlobalsScreenBits(ppc_cpu *c) { copy_global(c, QDGlobals_screenBits_off, BitMap_SIZEOF); }
static void qd_GetQDGlobalsArrow(ppc_cpu *c) { copy_global(c, QDGlobals_arrow_off, Cursor_SIZEOF); }
static void qd_GetQDGlobalsDarkGray(ppc_cpu *c) { copy_global(c, QDGlobals_dkGray_off, 8); }
static void qd_GetQDGlobalsLightGray(ppc_cpu *c) { copy_global(c, QDGlobals_ltGray_off, 8); }
static void qd_GetQDGlobalsGray(ppc_cpu *c) { copy_global(c, QDGlobals_gray_off, 8); }
static void qd_GetQDGlobalsBlack(ppc_cpu *c) { copy_global(c, QDGlobals_black_off, 8); }
static void qd_GetQDGlobalsWhite(ppc_cpu *c) { copy_global(c, QDGlobals_white_off, 8); }
static void qd_GetQDGlobalsThePort(ppc_cpu *c) { RETURN(qd_port()); }

static void qd_SetQDGlobalsArrow(ppc_cpu *c)
{
    gaddr g = qd_globals();
    if (g && R(0))
        gmemcpy(g + QDGlobals_arrow_off, R(0), Cursor_SIZEOF);
    DONE();
}

static void qd_GetQDGlobalsRandomSeed(ppc_cpu *c)
{
    gaddr g = qd_globals();
    RETURN(g ? rd32(g + QDGlobals_randSeed_off) : 0);
}

static void qd_SetQDGlobalsRandomSeed(ppc_cpu *c)
{
    gaddr g = qd_globals();
    if (g)
        wr32(g + QDGlobals_randSeed_off, R(0));
    DONE();
}

/* SetQDError(err): what QDError answers next. */
static void qd_SetQDError(ppc_cpu *c)
{
    wr16(LM_QDErr_ADDR, (uint16_t)R(0));
    DONE();
}

/* QDGetPatternOrigin / QDSetPatternOrigin: patterns are aligned to the
 * port's own origin; the point is kept for those who ask. */
static uint32_t pattern_origin;

static void qd_QDGetPatternOrigin(ppc_cpu *c)
{
    if (R(0))
        wr32(R(0), pattern_origin);
    DONE();
}

static void qd_QDSetPatternOrigin(ppc_cpu *c)
{
    pattern_origin = R(0);
    DONE();
}

/* StandardGlyphs(dataStream, size): QuickDraw Text's glyph drawing for
 * ATSUI, which isn't here. */
static void qd_StandardGlyphs(ppc_cpu *c) { RETURN((uint32_t)(int32_t)-4); /* unimpErr */ }

void carbonqd_reset(void)
{
    memset(bw_pixmaps, 0, sizeof bw_pixmaps);
    pattern_origin = 0;
}

const ppc_export carbonqd_exports[] = {
    { "CreateNewPort", qd_CreateNewPort },
    { "DisposePort", qd_DisposePort },
    { "GetPixBounds", qd_GetPixBounds },
    { "GetPixDepth", qd_GetPixDepth },
    { "GetPortBackColor", qd_GetPortBackColor },
    { "GetPortBackPixPat", qd_GetPortBackPixPat },
    { "GetPortBitMapForCopyBits", qd_GetPortBitMapForCopyBits },
    { "GetPortBounds", qd_GetPortBounds },
    { "GetPortChExtra", qd_GetPortChExtra },
    { "GetPortClipRegion", qd_GetPortClipRegion },
    { "GetPortFillPixPat", qd_GetPortFillPixPat },
    { "GetPortForeColor", qd_GetPortForeColor },
    { "GetPortFracHPenLocation", qd_GetPortFracHPenLocation },
    { "GetPortGrafProcs", qd_GetPortGrafProcs },
    { "GetPortHiliteColor", qd_GetPortHiliteColor },
    { "GetPortOpColor", qd_GetPortOpColor },
    { "GetPortPenLocation", qd_GetPortPenLocation },
    { "GetPortPenMode", qd_GetPortPenMode },
    { "GetPortPenPixPat", qd_GetPortPenPixPat },
    { "GetPortPenSize", qd_GetPortPenSize },
    { "GetPortPenVisibility", qd_GetPortPenVisibility },
    { "GetPortPixMap", qd_GetPortPixMap },
    { "GetPortSpExtra", qd_GetPortSpExtra },
    { "GetPortTextFace", qd_GetPortTextFace },
    { "GetPortTextFont", qd_GetPortTextFont },
    { "GetPortTextMode", qd_GetPortTextMode },
    { "GetPortTextSize", qd_GetPortTextSize },
    { "GetPortVisibleRegion", qd_GetPortVisibleRegion },
    { "GetQDGlobalsArrow", qd_GetQDGlobalsArrow },
    { "GetQDGlobalsBlack", qd_GetQDGlobalsBlack },
    { "GetQDGlobalsDarkGray", qd_GetQDGlobalsDarkGray },
    { "GetQDGlobalsGray", qd_GetQDGlobalsGray },
    { "GetQDGlobalsLightGray", qd_GetQDGlobalsLightGray },
    { "GetQDGlobalsRandomSeed", qd_GetQDGlobalsRandomSeed },
    { "GetQDGlobalsScreenBits", qd_GetQDGlobalsScreenBits },
    { "GetQDGlobalsThePort", qd_GetQDGlobalsThePort },
    { "GetQDGlobalsWhite", qd_GetQDGlobalsWhite },
    { "GetRegionBounds", qd_GetRegionBounds },
    { "HandleToRgn", qd_HandleToRgn },
    { "IsPortClipRegionEmpty", qd_IsPortClipRegionEmpty },
    { "IsPortColor", qd_IsPortColor },
    { "IsPortOffscreen", qd_IsPortOffscreen },
    { "IsPortPictureBeingDefined", qd_IsPortPictureBeingDefined },
    { "IsPortPolyBeingDefined", qd_IsPortPolyBeingDefined },
    { "IsPortRegionBeingDefined", qd_IsPortRegionBeingDefined },
    { "IsPortVisibleRegionEmpty", qd_IsPortVisibleRegionEmpty },
    { "IsRegionRectangular", qd_IsRegionRectangular },
    { "IsValidPort", qd_IsValidPort },
    { "LockPortBits", qd_LockPortBits },
    { "QDAddRectToDirtyRegion", qd_dirty_noErr },
    { "QDAddRegionToDirtyRegion", qd_dirty_noErr },
    { "QDDisposeRegionBits", qd_QDDisposeRegionBits },
    { "QDFlushPortBuffer", qd_QDFlushPortBuffer },
    { "QDGetDirtyRegion", qd_QDGetDirtyRegion },
    { "QDGetPatternOrigin", qd_QDGetPatternOrigin },
    { "QDIsPortBufferDirty", qd_QDIsPortBufferDirty },
    { "QDIsPortBuffered", qd_QDIsPortBuffered },
    { "QDRegionToRects", qd_QDRegionToRects },
    { "QDRestoreRegionBits", qd_QDRestoreRegionBits },
    { "QDSaveRegionBits", qd_QDSaveRegionBits },
    { "QDSetDirtyRegion", qd_dirty_noErr },
    { "QDSetPatternOrigin", qd_QDSetPatternOrigin },
    { "QDSwapPort", qd_QDSwapPort },
    { "RgnToHandle", qd_RgnToHandle },
    { "SectRegionWithPortClipRegion", qd_SectRegionWithPortClipRegion },
    { "SectRegionWithPortVisibleRegion", qd_SectRegionWithPortVisibleRegion },
    { "SetPortBackPixPat", qd_SetPortBackPixPat },
    { "SetPortBounds", qd_SetPortBounds },
    { "SetPortClipRegion", qd_SetPortClipRegion },
    { "SetPortFillPixPat", qd_SetPortFillPixPat },
    { "SetPortFracHPenLocation", qd_SetPortFracHPenLocation },
    { "SetPortGrafProcs", qd_SetPortGrafProcs },
    { "SetPortOpColor", qd_SetPortOpColor },
    { "SetPortPenMode", qd_SetPortPenMode },
    { "SetPortPenPixPat", qd_SetPortPenPixPat },
    { "SetPortPenSize", qd_SetPortPenSize },
    { "SetPortTextFace", qd_SetPortTextFace },
    { "SetPortTextFont", qd_SetPortTextFont },
    { "SetPortTextMode", qd_SetPortTextMode },
    { "SetPortTextSize", qd_SetPortTextSize },
    { "SetPortVisibleRegion", qd_SetPortVisibleRegion },
    { "SetQDError", qd_SetQDError },
    { "SetQDGlobalsArrow", qd_SetQDGlobalsArrow },
    { "SetQDGlobalsRandomSeed", qd_SetQDGlobalsRandomSeed },
    { "StandardGlyphs", qd_StandardGlyphs },
    { "SwapPortPicSaveHandle", qd_SwapPortPicSaveHandle },
    { "UnlockPortBits", qd_UnlockPortBits },
};
const unsigned carbonqd_export_count = sizeof carbonqd_exports / sizeof carbonqd_exports[0];

/* StdOpcode(fromRect, toRect, opcode, version): DrawPicture reads past
 * the data of the opcodes it doesn't know itself; called by a program
 * (its own opcodeProc passing one on) there is nothing more to do. */
void tb_StdOpcode(gaddr fromRect, gaddr toRect, uint16_t opcode, int16_t version) {}
