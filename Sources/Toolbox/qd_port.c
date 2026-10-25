/* QuickDraw globals, graphics ports, pen state, colours and the cursor. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "qd_internal.h"
#include "tb_internal.h"
#include "machine.h"
#include "sysres_gen.h"
#include "trap.h"

const uint8_t qd_pat_black[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
const uint8_t qd_pat_white[8] = { 0 };
const uint8_t qd_pat_gray[8] = { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 };
const uint8_t qd_pat_ltgray[8] = { 0x88, 0x22, 0x88, 0x22, 0x88, 0x22, 0x88, 0x22 };
const uint8_t qd_pat_dkgray[8] = { 0x77, 0xDD, 0x77, 0xDD, 0x77, 0xDD, 0x77, 0xDD };

static const uint16_t arrow_data[16] = { 0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7C00, 0x7E00, 0x7F00,
                                         0x7F80, 0x7C00, 0x6C00, 0x4600, 0x0600, 0x0300, 0x0300, 0x0000 };
static const uint16_t arrow_mask[16] = { 0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00, 0xFF80,
                                         0xFFC0, 0xFFE0, 0xFE00, 0xEF00, 0xCF00, 0x8780, 0x0780, 0x0380 };

enum { blackColor = 33, whiteColor = 30 };
enum { patCopy = 8, srcOr = 1 };

/* ---- QuickDraw globals ---- */

/* Address of the app's thePort variable (QuickDraw globals end there). */
static gaddr qd_globals_ptr(void)
{
    gaddr a5 = rd32(LM_CurrentA5);
    return a5 ? rd32(a5) : 0;
}

gaddr qd_port(void)
{
    gaddr g = qd_globals_ptr();
    return g ? rd32(g) : 0;
}

static gaddr qd_global(uint32_t off)
{
    return qd_globals_ptr() - QDGlobals_thePort_off + off;
}

gaddr qd_screen_bits(void) { return qd_global(QDGlobals_screenBits_off); }

uint32_t qd_screen_row_bytes(void);

void tb_InitGraf(gaddr gp)
{
    wr32(rd32(LM_CurrentA5), gp);
    gaddr base = gp - QDGlobals_thePort_off;
    wr32(gp, 0); /* thePort */
    gput(base + QDGlobals_white_off, qd_pat_white, 8);
    gput(base + QDGlobals_black_off, qd_pat_black, 8);
    gput(base + QDGlobals_gray_off, qd_pat_gray, 8);
    gput(base + QDGlobals_ltGray_off, qd_pat_ltgray, 8);
    gput(base + QDGlobals_dkGray_off, qd_pat_dkgray, 8);
    gaddr arrow = base + QDGlobals_arrow_off;
    for (int i = 0; i < 16; i++) {
        wr16(arrow + (uint32_t)i * 2, arrow_data[i]);
        wr16(arrow + 32 + (uint32_t)i * 2, arrow_mask[i]);
    }
    wr16(arrow + 64, 1);
    wr16(arrow + 66, 1);
    gaddr sb = base + QDGlobals_screenBits_off;
    BitMap_set_baseAddr(sb, rd32(LM_ScrnBase));
    BitMap_set_rowBytes(sb, (int16_t)qd_screen_row_bytes());
    qrect_put(sb + BitMap_bounds_off, (qrect){ 0, 0, (int16_t)g_screen_height, (int16_t)g_screen_width });
    wr32(base + QDGlobals_randSeed_off, 1);
    wr8(LM_QDExist, 0);
}

/* ---- Ports ---- */

bool qd_port_is_color(gaddr port) { return port && (rd16(port + GrafPort_portBits_off + 4) & 0xC000) == 0xC000; }

/* A PixMap's pixels.  A GWorld's whose pixels are unlocked (pmVersion 2)
 * holds their handle in baseAddr, as QuickDraw's did (programs move the
 * pixels by it: HunterDeer disposes of the handle and puts its own in);
 * drawing locks it so the pixels stay put while they are drawn into. */
gaddr qd_pixmap_base(gaddr pm, bool lock)
{
    gaddr base = PixMap_baseAddr(pm);
    if (!(PixMap_rowBytes(pm) & 0x8000) || !(PixMap_pmVersion(pm) & 2) || !base)
        return base;
    if (lock && rd32(base))
        tb_HLock(base);
    return rd32(base);
}

pixdst qd_port_pixels(gaddr port)
{
    pixdst d;
    d.ctab = 0;
    if (qd_port_is_color(port)) {
        gaddr pm = rd32(rd32(port + CGrafPort_portPixMap_off));
        d.base = qd_pixmap_base(pm, true);
        d.row_bytes = (uint16_t)PixMap_rowBytes(pm) & 0x3FFF;
        d.bounds = qrect_get(pm + PixMap_bounds_off);
        d.depth = PixMap_pixelSize(pm);
        d.ctab = PixMap_pmTable(pm);
    } else {
        gaddr bm = port + GrafPort_portBits_off;
        d.base = BitMap_baseAddr(bm);
        d.row_bytes = (uint16_t)BitMap_rowBytes(bm) & 0x3FFF;
        d.bounds = qrect_get(bm + BitMap_bounds_off);
        d.depth = 1;
        /* An old port on a deep screen draws at the screen's depth. */
        if (d.base == GA_SCREEN) {
            d.depth = qd_screen_depth();
            d.ctab = qd_screen_ctab();
        }
    }
    if (d.depth < 1)
        d.depth = 1;
    pixdst_clamp(&d);
    return d;
}

void qd_port_clip(gaddr port, hrgn *out)
{
    hrgn clip;
    hrgn_init(&clip);
    rgn_read(GrafPort_visRgn(port), out);
    rgn_read(GrafPort_clipRgn(port), &clip);
    hrgn_op(out, out, &clip, RGN_SECT);
    pixdst d = qd_port_pixels(port);
    hrgn_rect(&clip, d.bounds);
    hrgn_op(out, out, &clip, RGN_SECT);
    hrgn_free(&clip);
}

static void set_pattern(gaddr dst, const uint8_t pat[8]) { gput(dst, pat, 8); }

/* A PixPat handle for an old-style pattern (patType 0). */
gaddr qd_pixpat_for(const uint8_t pat[8])
{
    gaddr h = tb_NewHandle(PixPat_SIZEOF, false, true);
    gaddr p = rd32(h);
    PixPat_set_patType(p, 0);
    gput(p + PixPat_pat1Data_off, pat, 8);
    return h;
}

void tb_InitPort(gaddr p)
{
    bool color = qd_port_is_color(p);
    gaddr sb = qd_screen_bits();
    if (!color) {
        gmemcpy(p + GrafPort_portBits_off, sb, BitMap_SIZEOF);
    }
    GrafPort_set_device(p, 0);
    qrect_put(p + GrafPort_portRect_off, qrect_get(sb + BitMap_bounds_off));
    tb_RectRgn(GrafPort_visRgn(p), sb + BitMap_bounds_off);
    tb_SetRectRgn(GrafPort_clipRgn(p), -32767, -32767, 32767, 32767);
    if (!color) {
        set_pattern(p + GrafPort_bkPat_off, qd_pat_white);
        set_pattern(p + GrafPort_fillPat_off, qd_pat_black);
        set_pattern(p + GrafPort_pnPat_off, qd_pat_black);
        GrafPort_set_fgColor(p, blackColor);
        GrafPort_set_bkColor(p, whiteColor);
    }
    wr32(p + GrafPort_pnLoc_off, 0);
    wr32(p + GrafPort_pnSize_off, make_pt(1, 1));
    GrafPort_set_pnMode(p, patCopy);
    GrafPort_set_pnVis(p, 0);
    GrafPort_set_txFont(p, 0);
    GrafPort_set_txFace(p, 0);
    GrafPort_set_txMode(p, srcOr);
    GrafPort_set_txSize(p, 0);
    GrafPort_set_spExtra(p, 0);
    GrafPort_set_colrBit(p, 0);
    GrafPort_set_patStretch(p, 0);
    GrafPort_set_picSave(p, 0);
    GrafPort_set_rgnSave(p, 0);
    GrafPort_set_polySave(p, 0);
    GrafPort_set_grafProcs(p, 0);
    qd_screen_port_opened(p);
    tb_SetPort(p);
}

/* PortList: a handle of a count (word) and then every open port. */
static void port_list_add(gaddr p)
{
    gaddr h = rd32(LM_PortList);
    if (!h || !rd32(h)) {
        h = tb_NewHandle(2, true, true);
        if (!h)
            return;
        wr32(LM_PortList, h);
    }
    int n = (int16_t)rd16(rd32(h));
    for (int i = 0; i < n; i++)
        if (rd32(rd32(h) + 2 + (uint32_t)i * 4) == p)
            return;
    tb_SetHandleSize(h, 2 + (n + 1) * 4);
    if (tb_GetHandleSize(h) < 2 + (n + 1) * 4)
        return;
    wr32(rd32(h) + 2 + (uint32_t)n * 4, p);
    wr16(rd32(h), (uint16_t)(n + 1));
}

static void port_list_remove(gaddr p)
{
    gaddr h = rd32(LM_PortList);
    if (!h || !rd32(h))
        return;
    int n = (int16_t)rd16(rd32(h));
    for (int i = 0; i < n; i++) {
        gaddr e = rd32(h) + 2 + (uint32_t)i * 4;
        if (rd32(e) != p)
            continue;
        gmemcpy(e, e + 4, (uint32_t)(n - i - 1) * 4);
        wr16(rd32(h), (uint16_t)(n - 1));
        tb_SetHandleSize(h, 2 + (n - 1) * 4);
        return;
    }
}

void tb_OpenPort(gaddr p)
{
    port_list_add(p);
    wr16(p + GrafPort_portBits_off + 4, 0); /* make sure it is not taken for a CGrafPort */
    GrafPort_set_visRgn(p, tb_NewRgn());
    GrafPort_set_clipRgn(p, tb_NewRgn());
    tb_InitPort(p);
}

void tb_ClosePort(gaddr p)
{
    port_list_remove(p);
    qd_port_forget(p);
    qd_screen_port_closed(p);
    tb_DisposeRgn(GrafPort_visRgn(p));
    tb_DisposeRgn(GrafPort_clipRgn(p));
}

/* ---- Colour ports ---- */

static void rgb_put(gaddr dst, uint16_t r, uint16_t g, uint16_t b)
{
    wr16(dst, r);
    wr16(dst + 2, g);
    wr16(dst + 4, b);
}

void tb_InitCPort(gaddr p)
{
    /* The pixmap mirrors the main device's. */
    gaddr pmh = rd32(p + CGrafPort_portPixMap_off);
    gaddr screen_pm = rd32(GDevice_gdPMap(rd32(rd32(LM_MainDevice))));
    gmemcpy(rd32(pmh), screen_pm, PixMap_SIZEOF);
    wr16(p + CGrafPort_portVersion_off, 0xC000);
    wr16(p + CGrafPort_chExtra_off, 0);
    wr16(p + CGrafPort_pnLocHFrac_off, 0x8000);
    rgb_put(p + CGrafPort_rgbFgColor_off, 0, 0, 0);
    rgb_put(p + CGrafPort_rgbBkColor_off, 0xFFFF, 0xFFFF, 0xFFFF);
    GrafPort_set_fgColor(p, 1);
    GrafPort_set_bkColor(p, 0);
    tb_InitPort(p);
}

/* A colour port's own pixel patterns, made by OpenCPort.  PenPixPat and
 * BackPixPat put the program's pattern in the port in their place (the
 * program keeps it and disposes it); PenPat, BackPat and PenNormal go back
 * to the port's own, and CloseCPort disposes only these. */
static const uint32_t own_offs[3] = { CGrafPort_pnPixPat_off, CGrafPort_bkPixPat_off, CGrafPort_fillPixPat_off };
static struct port_pats {
    gaddr port, pp[3];
} *own_pats;
static size_t nown_pats;

void qd_port_pats_reset(void)
{
    free(own_pats);
    own_pats = NULL;
    nown_pats = 0;
}

static struct port_pats *port_pats_of(gaddr port)
{
    for (size_t i = 0; port && i < nown_pats; i++)
        if (own_pats[i].port == port)
            return &own_pats[i];
    return NULL;
}

/* The port's own pattern handle for an offset: put back in the port when
 * the program's is there; 0 for a port not opened by OpenCPort. */
static gaddr own_pixpat(gaddr port, uint32_t off)
{
    struct port_pats *o = port_pats_of(port);
    for (int i = 0; o && i < 3; i++)
        if (own_offs[i] == off) {
            wr32(port + off, o->pp[i]);
            return o->pp[i];
        }
    return rd32(port + off);
}

void tb_OpenCPort(gaddr p)
{
    port_list_add(p);
    wr32(p + CGrafPort_portPixMap_off, tb_NewHandle(PixMap_SIZEOF, false, true));
    wr16(p + CGrafPort_portVersion_off, 0xC000);
    wr32(p + CGrafPort_grafVars_off, tb_NewHandle(36, false, true));
    wr32(p + CGrafPort_bkPixPat_off, qd_pixpat_for(qd_pat_white));
    wr32(p + CGrafPort_pnPixPat_off, qd_pixpat_for(qd_pat_black));
    wr32(p + CGrafPort_fillPixPat_off, qd_pixpat_for(qd_pat_black));
    struct port_pats *o = port_pats_of(p);
    if (!o) {
        struct port_pats *n = realloc(own_pats, (nown_pats + 1) * sizeof *own_pats);
        if (n) {
            own_pats = n;
            o = &own_pats[nown_pats++];
        }
    }
    if (o) {
        o->port = p;
        for (int i = 0; i < 3; i++)
            o->pp[i] = rd32(p + own_offs[i]);
    }
    GrafPort_set_visRgn(p, tb_NewRgn());
    GrafPort_set_clipRgn(p, tb_NewRgn());
    tb_InitCPort(p);
}

void tb_CloseCPort(gaddr p)
{
    tb_ClosePort(p);
    tb_DisposeHandle(rd32(p + CGrafPort_portPixMap_off));
    tb_DisposeHandle(rd32(p + CGrafPort_grafVars_off));
    struct port_pats *o = port_pats_of(p);
    for (int i = 0; i < 3; i++)
        tb_DisposeHandle(o ? o->pp[i] : rd32(p + own_offs[i]));
    if (o)
        *o = own_pats[--nown_pats];
}

void tb_SetPort(gaddr p)
{
    gaddr g = qd_globals_ptr();
    if (g)
        wr32(g, p);
}

void tb_GetPort(gaddr pp) { wr32(pp, qd_port()); }
void tb_GrafDevice(int16_t d) { GrafPort_set_device(qd_port(), d); }

void tb_SetPortBits(gaddr bm)
{
    gaddr p = qd_port();
    if (!qd_port_is_color(p))
        gmemcpy(p + GrafPort_portBits_off, bm, BitMap_SIZEOF);
}

void tb_PortSize(int16_t w, int16_t h)
{
    gaddr p = qd_port();
    qrect r = qrect_get(p + GrafPort_portRect_off);
    r.right = (int16_t)(r.left + w);
    r.bottom = (int16_t)(r.top + h);
    qrect_put(p + GrafPort_portRect_off, r);
}

void tb_MovePortTo(int16_t lg, int16_t tg)
{
    /* Move the port's bitmap bounds so local (0,0) is at global (lg, tg). */
    gaddr p = qd_port();
    gaddr bounds = qd_port_is_color(p) ? rd32(rd32(p + CGrafPort_portPixMap_off)) + PixMap_bounds_off
                                       : p + GrafPort_portBits_off + BitMap_bounds_off;
    qrect r = qrect_get(p + GrafPort_portRect_off);
    qrect b = qrect_get(bounds);
    int16_t w = (int16_t)(b.right - b.left), h = (int16_t)(b.bottom - b.top);
    b.left = (int16_t)(r.left - lg);
    b.top = (int16_t)(r.top - tg);
    b.right = (int16_t)(b.left + w);
    b.bottom = (int16_t)(b.top + h);
    qrect_put(bounds, b);
}

void tb_SetOrigin(int16_t h, int16_t v)
{
    gaddr p = qd_port();
    qrect r = qrect_get(p + GrafPort_portRect_off);
    int16_t dh = (int16_t)(h - r.left), dv = (int16_t)(v - r.top);
    if (!dh && !dv)
        return;
    tb_OffsetRect(p + GrafPort_portRect_off, dh, dv);
    gaddr bounds = qd_port_is_color(p) ? rd32(rd32(p + CGrafPort_portPixMap_off)) + PixMap_bounds_off
                                       : p + GrafPort_portBits_off + BitMap_bounds_off;
    tb_OffsetRect(bounds, dh, dv);
    tb_OffsetRgn(GrafPort_visRgn(p), dh, dv);
}

void tb_SetClip(gaddr r) { tb_CopyRgn(r, GrafPort_clipRgn(qd_port())); }
void tb_GetClip(gaddr r) { tb_CopyRgn(GrafPort_clipRgn(qd_port()), r); }
void tb_ClipRect(gaddr r) { tb_RectRgn(GrafPort_clipRgn(qd_port()), r); }

/* An old-style pattern in a colour port: its pixel pattern becomes a
 * black-and-white one (type 0) with these bits. */
static void set_old_pattern(gaddr pph, const uint8_t pat[8])
{
    if (!pph || !rd32(pph))
        return;
    PixPat_set_patType(rd32(pph), 0);
    gput(rd32(pph) + PixPat_pat1Data_off, pat, 8);
}

void tb_BackPat(gaddr pp)
{
    gaddr p = qd_port();
    uint8_t pat[8];
    gget(pat, pp, 8);
    if (qd_port_is_color(p))
        set_old_pattern(own_pixpat(p, CGrafPort_bkPixPat_off), pat);
    else
        gmemcpy(p + GrafPort_bkPat_off, pp, 8);
}

/* The 8x8 pattern a port uses for pen, background or fill. */
void qd_port_pattern(gaddr port, int which, uint8_t out[8])
{
    if (qd_port_is_color(port)) {
        uint32_t off = which == 0 ? CGrafPort_pnPixPat_off : which == 1 ? CGrafPort_bkPixPat_off : CGrafPort_fillPixPat_off;
        gaddr h = rd32(port + off);
        if (h && rd32(h)) {
            gget(out, rd32(h) + PixPat_pat1Data_off, 8);
            return;
        }
        memcpy(out, which == 1 ? qd_pat_white : qd_pat_black, 8);
        return;
    }
    uint32_t off = which == 0 ? GrafPort_pnPat_off : which == 1 ? GrafPort_bkPat_off : GrafPort_fillPat_off;
    gget(out, port + off, 8);
}

/* ---- Pen ---- */

/* The standard bottlenecks: whatever the trap table holds for them. */
static const uint16_t std_traps[] = { 0xA882, 0xA890, 0xA8A0, 0xA8AF, 0xA8B6, 0xA8BD, 0xA8C5,
                                      0xA8D1, 0xA8EB, 0xA8F1, 0xA8ED, 0xA8EE, 0xA8F0 };

void tb_SetStdProcs(gaddr procs)
{
    for (unsigned i = 0; i < sizeof std_traps / sizeof std_traps[0]; i++)
        wr32(procs + i * 4, rd32(TB_TRAP_TABLE + (std_traps[i] & 0x3FFu) * 4));
}

void tb_SetStdCProcs(gaddr procs)
{
    tb_SetStdProcs(procs);
    gmemset(procs + QDProcs_SIZEOF, 0, CQDProcs_SIZEOF - QDProcs_SIZEOF);
}

void tb_HidePen(void) { GrafPort_set_pnVis(qd_port(), (int16_t)(GrafPort_pnVis(qd_port()) - 1)); }
void tb_ShowPen(void) { GrafPort_set_pnVis(qd_port(), (int16_t)(GrafPort_pnVis(qd_port()) + 1)); }
void tb_GetPen(gaddr ptp) { wr32(ptp, rd32(qd_port() + GrafPort_pnLoc_off)); }

void tb_GetPenState(gaddr ps)
{
    gaddr p = qd_port();
    wr32(ps + PenState_pnLoc_off, rd32(p + GrafPort_pnLoc_off));
    wr32(ps + PenState_pnSize_off, rd32(p + GrafPort_pnSize_off));
    wr16(ps + PenState_pnMode_off, (uint16_t)GrafPort_pnMode(p));
    uint8_t pat[8];
    qd_port_pattern(p, 0, pat);
    gput(ps + PenState_pnPat_off, pat, 8);
}

void tb_SetPenState(gaddr ps)
{
    gaddr p = qd_port();
    wr32(p + GrafPort_pnLoc_off, rd32(ps + PenState_pnLoc_off));
    wr32(p + GrafPort_pnSize_off, rd32(ps + PenState_pnSize_off));
    GrafPort_set_pnMode(p, (int16_t)rd16(ps + PenState_pnMode_off));
    tb_PenPat(ps + PenState_pnPat_off);
}

void tb_PenSize(int16_t w, int16_t h) { wr32(qd_port() + GrafPort_pnSize_off, make_pt(h, w)); }
void tb_PenMode(int16_t m) { GrafPort_set_pnMode(qd_port(), m); }

void tb_PenPat(gaddr pp)
{
    gaddr p = qd_port();
    uint8_t pat[8];
    gget(pat, pp, 8);
    if (qd_port_is_color(p))
        set_old_pattern(own_pixpat(p, CGrafPort_pnPixPat_off), pat);
    else
        gmemcpy(p + GrafPort_pnPat_off, pp, 8);
}

void tb_PenNormal(void)
{
    gaddr p = qd_port();
    wr32(p + GrafPort_pnSize_off, make_pt(1, 1));
    GrafPort_set_pnMode(p, patCopy);
    if (qd_port_is_color(p))
        set_old_pattern(own_pixpat(p, CGrafPort_pnPixPat_off), qd_pat_black);
    else
        gput(p + GrafPort_pnPat_off, qd_pat_black, 8);
}

void tb_MoveTo(int16_t h, int16_t v) { wr32(qd_port() + GrafPort_pnLoc_off, make_pt(v, h)); }

void tb_Move(int16_t dh, int16_t dv)
{
    gaddr p = qd_port();
    uint32_t loc = rd32(p + GrafPort_pnLoc_off);
    wr32(p + GrafPort_pnLoc_off, make_pt((int16_t)(pt_v(loc) + dv), (int16_t)(pt_h(loc) + dh)));
}

/* ---- Colour ---- */

/* Is a colour dark enough to be black on a 1-bit screen? */
static bool rgb_dark(uint16_t r, uint16_t g, uint16_t b)
{
    return (uint32_t)r * 30 + (uint32_t)g * 59 + (uint32_t)b * 11 < 0x8000u * 100;
}

static void old_color_rgb(int32_t c, uint16_t rgb[3])
{
    switch (c) {
    case whiteColor: rgb[0] = rgb[1] = rgb[2] = 0xFFFF; break;
    case 69: rgb[0] = 0xFC00, rgb[1] = 0xF37D, rgb[2] = 0x052F; break;   /* yellow */
    case 137: rgb[0] = 0xF2D7, rgb[1] = 0x0856, rgb[2] = 0x84EC; break;  /* magenta */
    case 205: rgb[0] = 0xDD6B, rgb[1] = 0x08C2, rgb[2] = 0x06A2; break;  /* red */
    case 273: rgb[0] = 0x0241, rgb[1] = 0xAB54, rgb[2] = 0xEAFF; break;  /* cyan */
    case 341: rgb[0] = 0x0000, rgb[1] = 0x8000, rgb[2] = 0x11B0; break;  /* green */
    case 409: rgb[0] = 0x0000, rgb[1] = 0x0000, rgb[2] = 0xD400; break;  /* blue */
    default: rgb[0] = rgb[1] = rgb[2] = 0; break;                        /* black */
    }
}

/* The nearest of the eight old colours, which is what RGBForeColor and
 * RGBBackColor give a GrafPort (the Installer's licence text, in red). */
static int32_t nearest_old_color(const uint16_t rgb[3])
{
    static const int32_t olds[] = { blackColor, whiteColor, 69, 137, 205, 273, 341, 409 };
    int32_t best = blackColor;
    int64_t best_d = INT64_MAX;
    for (size_t i = 0; i < sizeof olds / sizeof olds[0]; i++) {
        uint16_t o[3];
        old_color_rgb(olds[i], o);
        int64_t d = 0;
        for (int k = 0; k < 3; k++)
            d += ((int64_t)rgb[k] - o[k]) * ((int64_t)rgb[k] - o[k]);
        if (d < best_d)
            best_d = d, best = olds[i];
    }
    return best;
}

static bool fg_override;
static uint16_t fg_override_rgb[3];

void qd_set_fg_override(const uint16_t *rgb)
{
    fg_override = rgb != NULL;
    if (rgb)
        memcpy(fg_override_rgb, rgb, sizeof fg_override_rgb);
}

/* The port's foreground and background as pixel values at its depth. */
/* Colours given by index (PmForeColor and PmBackColor for a palette entry
 * that owns a slot of the screen's table): the index, while the port's
 * colour is still the one it was set with. */
#define MAX_INDEXED 64
static struct {
    gaddr port;
    bool fore;
    uint16_t rgb[3];
    uint32_t index;
} indexed[MAX_INDEXED];

void qd_port_set_index(gaddr port, bool fore, uint32_t index)
{
    if (!qd_port_is_color(port))
        return;
    int slot = -1;
    for (int i = 0; i < MAX_INDEXED; i++) {
        if (indexed[i].port == port && indexed[i].fore == fore) {
            slot = i;
            break;
        }
        if (slot < 0 && !indexed[i].port)
            slot = i;
    }
    if (slot < 0)
        slot = (int)(port % MAX_INDEXED);
    gaddr c = port + (fore ? CGrafPort_rgbFgColor_off : CGrafPort_rgbBkColor_off);
    indexed[slot].port = port;
    indexed[slot].fore = fore;
    for (int k = 0; k < 3; k++)
        indexed[slot].rgb[k] = rd16(c + (uint32_t)k * 2);
    indexed[slot].index = index;
    if (fore)
        GrafPort_set_fgColor(port, (int32_t)index);
    else
        GrafPort_set_bkColor(port, (int32_t)index);
}

void qd_port_forget(gaddr port)
{
    for (int i = 0; i < MAX_INDEXED; i++)
        if (indexed[i].port == port)
            indexed[i].port = 0;
}

static bool index_for(gaddr port, bool fore, const uint16_t rgb[3], uint32_t *index)
{
    for (int i = 0; i < MAX_INDEXED; i++)
        if (indexed[i].port == port && indexed[i].fore == fore) {
            if (memcmp(indexed[i].rgb, rgb, sizeof indexed[i].rgb))
                return false;
            *index = indexed[i].index;
            return true;
        }
    return false;
}

void qd_port_bits(gaddr port, uint32_t *fg, uint32_t *bk)
{
    uint16_t f[3], b[3];
    if (qd_port_is_color(port)) {
        gaddr fa = port + CGrafPort_rgbFgColor_off, ba = port + CGrafPort_rgbBkColor_off;
        f[0] = rd16(fa); f[1] = rd16(fa + 2); f[2] = rd16(fa + 4);
        b[0] = rd16(ba); b[1] = rd16(ba + 2); b[2] = rd16(ba + 4);
    } else {
        old_color_rgb(GrafPort_fgColor(port), f);
        old_color_rgb(GrafPort_bkColor(port), b);
    }
    if (fg_override)
        memcpy(f, fg_override_rgb, sizeof f);
    pixdst d = qd_port_pixels(port);
    if (d.depth == 1) {
        *fg = (uint32_t)rgb_dark(f[0], f[1], f[2]);
        *bk = (uint32_t)rgb_dark(b[0], b[1], b[2]);
        return;
    }
    *fg = qd_rgb_px(d.depth, d.ctab, f);
    *bk = qd_rgb_px(d.depth, d.ctab, b);
    if (qd_port_is_color(port)) {
        uint32_t v;
        if (!fg_override && qd_search_color(d.base, f, &v))
            *fg = v;
        if (qd_search_color(d.base, b, &v))
            *bk = v;
    }
    /* Palette slots: on the indexed screen (or anything sharing its table). */
    uint32_t idx;
    if (qd_port_is_color(port) && d.depth <= 8 && d.ctab && d.ctab == qd_screen_ctab()) {
        if (!fg_override && index_for(port, true, f, &idx))
            *fg = idx;
        if (index_for(port, false, b, &idx))
            *bk = idx;
    }
}

/* A colour port's colour: its RGB, and in fgColor or bkColor the pixel
 * value, as Color2Index gives it (CopyBits colorizes by these). */
static int32_t port_pixel(gaddr p, const uint16_t rgb[3])
{
    pixdst d = qd_port_pixels(p);
    uint32_t v;
    if (d.depth > 1 && qd_search_color(d.base, rgb, &v))
        return (int32_t)v;
    return d.depth == 1 ? (rgb_dark(rgb[0], rgb[1], rgb[2]) ? 1 : 0) : (int32_t)qd_rgb_px(d.depth, d.ctab, rgb);
}

static void set_fore(gaddr p, const uint16_t rgb[3])
{
    rgb_put(p + CGrafPort_rgbFgColor_off, rgb[0], rgb[1], rgb[2]);
    GrafPort_set_fgColor(p, port_pixel(p, rgb));
}

static void set_back(gaddr p, const uint16_t rgb[3])
{
    rgb_put(p + CGrafPort_rgbBkColor_off, rgb[0], rgb[1], rgb[2]);
    GrafPort_set_bkColor(p, port_pixel(p, rgb));
}

/* The old-style colours: in a colour port, as their RGB colours. */
void tb_ForeColor(int32_t c)
{
    gaddr p = qd_port();
    if (qd_port_is_color(p)) {
        uint16_t rgb[3];
        old_color_rgb(c, rgb);
        set_fore(p, rgb);
    } else {
        GrafPort_set_fgColor(p, c);
    }
}

void tb_BackColor(int32_t c)
{
    gaddr p = qd_port();
    if (qd_port_is_color(p)) {
        uint16_t rgb[3];
        old_color_rgb(c, rgb);
        set_back(p, rgb);
    } else {
        GrafPort_set_bkColor(p, c);
    }
}

void tb_ColorBit(int16_t b) { GrafPort_set_colrBit(qd_port(), b); }

void tb_RGBForeColor(gaddr c)
{
    gaddr p = qd_port();
    uint16_t rgb[3] = { rd16(c), rd16(c + 2), rd16(c + 4) };
    if (qd_port_is_color(p))
        set_fore(p, rgb);
    else
        GrafPort_set_fgColor(p, nearest_old_color(rgb));
}

void tb_RGBBackColor(gaddr c)
{
    gaddr p = qd_port();
    uint16_t rgb[3] = { rd16(c), rd16(c + 2), rd16(c + 4) };
    if (qd_port_is_color(p))
        set_back(p, rgb);
    else
        GrafPort_set_bkColor(p, nearest_old_color(rgb));
}

void tb_GetForeColor(gaddr c)
{
    gaddr p = qd_port();
    if (qd_port_is_color(p)) {
        gmemcpy(c, p + CGrafPort_rgbFgColor_off, 6);
    } else {
        uint16_t rgb[3];
        old_color_rgb(GrafPort_fgColor(p), rgb);
        rgb_put(c, rgb[0], rgb[1], rgb[2]);
    }
}

void tb_GetBackColor(gaddr c)
{
    gaddr p = qd_port();
    if (qd_port_is_color(p)) {
        gmemcpy(c, p + CGrafPort_rgbBkColor_off, 6);
    } else {
        uint16_t rgb[3];
        old_color_rgb(GrafPort_bkColor(p), rgb);
        rgb_put(c, rgb[0], rgb[1], rgb[2]);
    }
}

/* The colour blend and the pinned modes use: GrafVars' rgbOpColor. */
void tb_OpColor(gaddr c)
{
    gaddr port = qd_port();
    if (!port || !qd_port_is_color(port))
        return;
    gaddr gv = rd32(port + CGrafPort_grafVars_off);
    if (gv && rd32(gv))
        gmemcpy(rd32(gv), c, 6);
}

void qd_port_op_color(gaddr port, uint16_t out[3])
{
    out[0] = out[1] = out[2] = 0;
    gaddr gv = port && qd_port_is_color(port) ? rd32(port + CGrafPort_grafVars_off) : 0;
    if (gv && rd32(gv))
        for (int i = 0; i < 3; i++)
            out[i] = rd16(rd32(gv) + (uint32_t)i * 2);
}
void tb_HiliteColor(gaddr c) {}
/* QDErr: nothing here fails; what SetQDError (CarbonLib) left. */
int16_t tb_QDError(void) { return (int16_t)rd16(LM_QDErr_ADDR); }

/* A pixel pattern with its pixel map and (empty) data handles, which
 * applications fill in themselves. */
gaddr tb_NewPixPat(void)
{
    gaddr h = qd_pixpat_for(qd_pat_black);
    gaddr pm = tb_NewPixMap(), data = tb_NewHandle(0, false, true), xdata = tb_NewHandle(0, false, true);
    gaddr p = rd32(h);
    PixPat_set_patType(p, 1); /* pixelPat */
    wr32(p + PixPat_patMap_off, pm);
    wr32(p + PixPat_patData_off, data);
    wr32(p + PixPat_patXData_off, xdata);
    wr16(p + PixPat_patXValid_off, 0xFFFF); /* the expanded data needs rebuilding */
    return h;
}

/* ---- Cursor (state only; the host draws the real cursor) ---- */

/* What the host shows: the cursor's image, its level (hidden below 0) and
 * the rectangle ShieldCursor keeps it out of.  Kept under a lock: the host
 * reads it from its own thread. */
uint32_t ev_mouse_global(void);
static pthread_mutex_t cursor_lock = PTHREAD_MUTEX_INITIALIZER;
static machine_cursor cursor = { .arrow = true, .kind = MACHINE_CURSOR_ARROW };
static int cursor_level, shield_depth;
static qrect shield; /* global */
static uint32_t cursor_version;

static void cursor_changed(void)
{
    cursor.hidden = cursor_level < 0;
    cursor_version++;
    /* Low memory that programs read: CrsrState (the level) and CrsrVis. */
    wr16(LM_CrsrState, (uint16_t)(int16_t)cursor_level);
    wr8(LM_CrsrVis, cursor_level >= 0 ? 0xFF : 0);
}

uint32_t machine_cursor_state(machine_cursor *out)
{
    pthread_mutex_lock(&cursor_lock);
    *out = cursor;
    /* Shielded: hidden only while the mouse is in the rectangle. */
    if (shield_depth > 0 && cursor_level + shield_depth >= 0) {
        uint32_t pt = ev_mouse_global();
        int16_t v = (int16_t)(pt >> 16), h = (int16_t)pt;
        out->hidden = v >= shield.top && v < shield.bottom && h >= shield.left && h < shield.right;
    }
    uint32_t version = cursor_version;
    pthread_mutex_unlock(&cursor_lock);
    return version;
}

/* One of the System's own cursors (CURS 1 to 4), which the host can show
 * as its own equivalent; MACHINE_CURSOR_CUSTOM otherwise. */
static int standard_kind(const machine_cursor *c)
{
    static const int kinds[5] = { 0, MACHINE_CURSOR_IBEAM, MACHINE_CURSOR_CROSS, MACHINE_CURSOR_PLUS,
                                  MACHINE_CURSOR_WATCH };
    for (unsigned i = 0; i < sysres_count; i++) {
        const sysres_entry *e = &sysres[i];
        if (e->type != 0x43555253u /* CURS */ || e->id < 1 || e->id > 4 || e->len < 68)
            continue;
        if (!memcmp(e->data, c->data, 32) && !memcmp(e->data + 32, c->mask, 32))
            return kinds[e->id];
    }
    return MACHINE_CURSOR_CUSTOM;
}

/* TheCrsr ($844): the current cursor's record, kept as QuickDraw kept it
 * (LMGetTheCursor reads it). */
#define LM_TheCrsr 0x0844u

static void set_image(gaddr data, gaddr mask, gaddr hot)
{
    gmemcpy(LM_TheCrsr, data, 32);
    gmemcpy(LM_TheCrsr + 32, mask, 32);
    gmemcpy(LM_TheCrsr + 64, hot, 4);
    pthread_mutex_lock(&cursor_lock);
    gget(cursor.data, data, 32);
    gget(cursor.mask, mask, 32);
    cursor.hot_v = (int16_t)rd16(hot);
    cursor.hot_h = (int16_t)rd16(hot + 2);
    bool arrow = true;
    for (int i = 0; i < 16; i++)
        if ((cursor.data[i * 2] << 8 | cursor.data[i * 2 + 1]) != arrow_data[i] ||
            (cursor.mask[i * 2] << 8 | cursor.mask[i * 2 + 1]) != arrow_mask[i])
            arrow = false;
    cursor.arrow = arrow && cursor.hot_v == 1 && cursor.hot_h == 1;
    cursor.kind = cursor.arrow ? MACHINE_CURSOR_ARROW : standard_kind(&cursor);
    cursor.color = false;
    cursor_version++;
    pthread_mutex_unlock(&cursor_lock);
}

/* A CCrsr's pixel map and pixels: handles, or (in a 'crsr' resource as
 * GetCCursor copies it) offsets from the record's start. */
static gaddr crsr_part(gaddr c, uint32_t field, uint32_t size)
{
    uint32_t v = rd32(c + field);
    if (!v)
        return 0;
    if (v < size)
        return c + v;
    return rd32(v);
}

/* The colours of a colour cursor (crsrType $8001) as RGBA: its pixels where
 * the mask is set; where it isn't, black for pixels that invert, else clear. */
static bool color_image(gaddr c, uint32_t size, uint8_t rgba[16 * 16 * 4])
{
    if (rd16(c) != 0x8001)
        return false;
    gaddr pm = crsr_part(c, 2, size), data = crsr_part(c, 6, size);
    if (!pm || !data)
        return false;
    int depth = (int16_t)rd16(pm + PixMap_pixelSize_off);
    uint32_t rb = rd16(pm + PixMap_rowBytes_off) & 0x3FFF;
    if (depth < 1 || depth > 32 || !rb)
        return false;
    uint32_t ct = rd32(pm + PixMap_pmTable_off);
    gaddr ctab_ptr = ct && ct < size ? c + ct : ct ? rd32(ct) : 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            uint8_t *o = rgba + (y * 16 + x) * 4;
            int i = y * 2 + x / 8, bit = 0x80 >> (x % 8);
            bool m = rd8(c + 52 + (uint32_t)i) & bit, d = rd8(c + 20 + (uint32_t)i) & bit;
            if (!m) {
                o[0] = o[1] = o[2] = 0;
                o[3] = d ? 255 : 0;
                continue;
            }
            gaddr row = data + (uint32_t)y * rb;
            uint32_t v;
            if (depth == 32)
                v = rd32(row + (uint32_t)x * 4);
            else if (depth == 16)
                v = rd16(row + (uint32_t)x * 2);
            else {
                int b = x * depth;
                v = (uint32_t)(rd8(row + (uint32_t)(b >> 3)) >> (8 - depth - (b & 7))) & ((1u << depth) - 1);
            }
            uint16_t rgb[3] = { 0, 0, 0 };
            if (depth >= 16) {
                qd_px_rgb(depth, 0, v, rgb);
            } else if (ctab_ptr) {
                /* The colour table's entries: value, r, g, b. */
                int n = rd16(ctab_ptr + 6) + 1;
                for (int k = 0; k < n; k++) {
                    gaddr e = ctab_ptr + 8 + (uint32_t)k * 8;
                    if (rd16(e) == v || (rd16(ctab_ptr + 4) & 0x8000 && k == (int)v)) {
                        rgb[0] = rd16(e + 2);
                        rgb[1] = rd16(e + 4);
                        rgb[2] = rd16(e + 6);
                        break;
                    }
                }
            }
            o[0] = (uint8_t)(rgb[0] >> 8);
            o[1] = (uint8_t)(rgb[1] >> 8);
            o[2] = (uint8_t)(rgb[2] >> 8);
            o[3] = 255;
        }
    return true;
}

void tb_InitCursor(void)
{
    for (int i = 0; i < 16; i++) {
        wr16(LM_TheCrsr + (uint32_t)i * 2, arrow_data[i]);
        wr16(LM_TheCrsr + 32 + (uint32_t)i * 2, arrow_mask[i]);
    }
    wr32(LM_TheCrsr + 64, 0x00010001);
    pthread_mutex_lock(&cursor_lock);
    cursor.arrow = true;
    cursor.kind = MACHINE_CURSOR_ARROW;
    cursor.color = false;
    cursor_level = 0;
    shield_depth = 0;
    cursor_changed();
    pthread_mutex_unlock(&cursor_lock);
}

/* Cursor: data (16 words), mask (16 words), hotSpot. */
void tb_SetCursor(gaddr cp)
{
    if (cp)
        set_image(cp, cp + 32, cp + 64);
}

/* CCrsr: its black-and-white version (crsr1Data, crsrMask, crsrHotSpot),
 * and its colours. */
void cursor_set_color(gaddr ch)
{
    gaddr c = ch ? rd32(ch) : 0;
    if (!c)
        return;
    set_image(c + 20, c + 52, c + 84);
    uint8_t rgba[sizeof cursor.rgba];
    if (!color_image(c, (uint32_t)tb_GetHandleSize(ch), rgba))
        return;
    pthread_mutex_lock(&cursor_lock);
    memcpy(cursor.rgba, rgba, sizeof rgba);
    cursor.color = true;
    cursor.arrow = false;
    cursor.kind = MACHINE_CURSOR_CUSTOM;
    cursor_version++;
    pthread_mutex_unlock(&cursor_lock);
}

void tb_HideCursor(void)
{
    pthread_mutex_lock(&cursor_lock);
    cursor_level--;
    cursor_changed();
    pthread_mutex_unlock(&cursor_lock);
}

void tb_ShowCursor(void)
{
    pthread_mutex_lock(&cursor_lock);
    if (shield_depth > 0)
        shield_depth--;
    if (cursor_level < 0)
        cursor_level++;
    cursor_changed();
    pthread_mutex_unlock(&cursor_lock);
}

/* Hidden until the mouse moves: the host does that part. */
void tb_ObscureCursor(void)
{
    pthread_mutex_lock(&cursor_lock);
    cursor.obscures++;
    cursor_version++;
    pthread_mutex_unlock(&cursor_lock);
}

/* Hidden while the mouse is in the rectangle (local to a port whose
 * bounds' top left is p); ShowCursor ends it. */
void tb_ShieldCursor(gaddr rp, uint32_t p)
{
    qrect r = qrect_get(rp);
    int16_t dv = (int16_t)(p >> 16), dh = (int16_t)p;
    pthread_mutex_lock(&cursor_lock);
    shield = (qrect){ (int16_t)(r.top - dv), (int16_t)(r.left - dh), (int16_t)(r.bottom - dv), (int16_t)(r.right - dh) };
    shield_depth++;
    cursor_level--;
    cursor_changed();
    pthread_mutex_unlock(&cursor_lock);
}

/* ---- Window Manager port (also needed by QuickDraw-level code) ---- */

void tb_GetWMgrPort(gaddr wp) { wr32(wp, rd32(LM_WMgrPort)); }
void tb_GetCWMgrPort(gaddr wp) { wr32(wp, rd32(LM_WMgrCPort)); }

/* ---- Coordinate conversion ---- */

void tb_LocalToGlobal(gaddr pt)
{
    pixdst d = qd_port_pixels(qd_port());
    wr16(pt, (uint16_t)((int16_t)rd16(pt) - d.bounds.top));
    wr16(pt + 2, (uint16_t)((int16_t)rd16(pt + 2) - d.bounds.left));
}

void tb_GlobalToLocal(gaddr pt)
{
    pixdst d = qd_port_pixels(qd_port());
    wr16(pt, (uint16_t)((int16_t)rd16(pt) + d.bounds.top));
    wr16(pt + 2, (uint16_t)((int16_t)rd16(pt + 2) + d.bounds.left));
}
