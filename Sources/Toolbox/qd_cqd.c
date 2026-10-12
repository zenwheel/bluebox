/*
 * Color QuickDraw extras: colour tables and conversions, pixmaps, devices,
 * GWorlds, colour icons and cursors, pixel patterns, the Palette Manager
 * and auxiliary control and window records.
 *
 * The screen is the only real device; GWorlds are colour ports over their
 * own pixel memory, at any depth our drawing code handles (1-32 bits).
 */
#include <stdlib.h>
#include <string.h>

#include "qd_internal.h"
#include "tb_internal.h"

static void rgb_get(gaddr p, uint16_t c[3])
{
    c[0] = rd16(p);
    c[1] = rd16(p + 2);
    c[2] = rd16(p + 4);
}

static void rgb_set(gaddr p, const uint16_t c[3])
{
    wr16(p, c[0]);
    wr16(p + 2, c[1]);
    wr16(p + 4, c[2]);
}

/* ---------------------------------------------------------------------- */
/* Colour tables */

static gaddr device_pixmap(void)
{
    gaddr gd = rd32(LM_TheGDevice);
    if (!gd || !rd32(gd))
        gd = rd32(LM_MainDevice);
    return rd32(GDevice_gdPMap(rd32(gd)));
}

int32_t tb_Color2Index(gaddr c)
{
    gaddr pm = device_pixmap();
    uint16_t rgb[3];
    rgb_get(c, rgb);
    return (int32_t)qd_rgb_px(PixMap_pixelSize(pm), PixMap_pmTable(pm), rgb);
}

void tb_Index2Color(int32_t i, gaddr c)
{
    gaddr pm = device_pixmap();
    uint16_t rgb[3];
    qd_px_rgb(PixMap_pixelSize(pm), PixMap_pmTable(pm), (uint32_t)i, rgb);
    rgb_set(c, rgb);
}

void tb_InvertColor(gaddr c)
{
    uint16_t rgb[3];
    rgb_get(c, rgb);
    for (int i = 0; i < 3; i++)
        rgb[i] = (uint16_t)~rgb[i];
    rgb_set(c, rgb);
}

void tb_GetSubTable(gaddr myColors, int16_t iTabRes, gaddr targetTbl) {}

/* A colour table of n entries filled by a function. */
static gaddr make_table(int n, void (*fill)(int, uint16_t[3], int), int arg, int32_t seed)
{
    gaddr h = tb_NewHandle(8 + n * 8, false, true);
    gaddr p = rd32(h);
    wr32(p, (uint32_t)seed);
    wr16(p + 4, 0);
    wr16(p + 6, (uint16_t)(n - 1));
    for (int i = 0; i < n; i++) {
        uint16_t c[3];
        fill(i, c, arg);
        wr16(p + 8 + (uint32_t)i * 8, (uint16_t)i);
        rgb_set(p + 8 + (uint32_t)i * 8 + 2, c);
    }
    return h;
}

static void fill_std(int i, uint16_t c[3], int depth)
{
    if (depth == 8)
        qd_std_rgb8(i, c);
    else
        qd_px_rgb(depth, 0, (uint32_t)i, c);
}

static void fill_gray(int i, uint16_t c[3], int depth)
{
    int max = (1 << depth) - 1;
    uint16_t v = (uint16_t)(0xFFFF - i * 0xFFFF / (max ? max : 1));
    c[0] = c[1] = c[2] = v;
}

gaddr tb_GetCTable(int16_t id)
{
    gaddr r = tb_GetResource((int32_t)fourcc("clut"), id);
    if (r && rd32(r)) {
        uint32_t h = r;
        tb_HandToHand(&h);
        return h;
    }
    /* The standard tables: 1, 2, 4, 8 colour; 33-40 the same depths in grey. */
    int depth = id > 32 ? id - 32 : id;
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8)
        return 0;
    return make_table(1 << depth, id > 32 ? fill_gray : fill_std, depth, id);
}

void tb_DisposeCTable(gaddr h)
{
    if (h)
        tb_DisposeHandle(h);
}

void tb_CTabChanged(gaddr h)
{
    if (h && rd32(h))
        wr32(rd32(h), rd32(rd32(h)) + 1); /* a new seed */
}

/* ---------------------------------------------------------------------- */
/* Pixmaps and devices */

gaddr tb_NewPixMap(void)
{
    gaddr h = tb_NewHandle(PixMap_SIZEOF, false, true);
    gaddr src = device_pixmap();
    gmemcpy(rd32(h), src, PixMap_SIZEOF);
    PixMap_set_baseAddr(rd32(h), 0);
    uint32_t ct = PixMap_pmTable(src);
    if (ct) {
        tb_HandToHand(&ct);
        PixMap_set_pmTable(rd32(h), ct);
    }
    return h;
}

void tb_DisposePixMap(gaddr h)
{
    if (!h)
        return;
    if (rd32(h) && PixMap_pmTable(rd32(h)))
        tb_DisposeHandle(PixMap_pmTable(rd32(h)));
    tb_DisposeHandle(h);
}

void tb_CopyPixMap(gaddr src, gaddr dst)
{
    gaddr keep = PixMap_pmTable(rd32(dst));
    gmemcpy(rd32(dst), rd32(src), PixMap_SIZEOF);
    gaddr st = PixMap_pmTable(rd32(src));
    if (keep && st) {
        int32_t n = tb_GetHandleSize(st);
        tb_SetHandleSize(keep, n);
        gmemcpy(rd32(keep), rd32(st), (uint32_t)n);
        PixMap_set_pmTable(rd32(dst), keep);
    }
}

gaddr tb_NewGDevice(int16_t refNum, int32_t mode)
{
    gaddr h = tb_NewHandle(GDevice_SIZEOF, true, true);
    GDevice_set_gdPMap(rd32(h), tb_NewPixMap());
    GDevice_set_gdRefNum(rd32(h), refNum);
    GDevice_set_gdMode(rd32(h), mode);
    return h;
}

void tb_InitGDevice(int16_t refNum, int32_t mode, gaddr gdh) { GDevice_set_gdMode(rd32(gdh), mode); }

void tb_DisposeGDevice(gaddr gdh)
{
    if (!gdh || gdh == rd32(LM_MainDevice))
        return;
    tb_DisposePixMap(GDevice_gdPMap(rd32(gdh)));
    tb_DisposeHandle(gdh);
}

gaddr tb_GetMaxDevice(gaddr globalRect) { return rd32(LM_MainDevice); }
void tb_GDeviceChanged(gaddr gdh) {}

/* ---------------------------------------------------------------------- */
/* GWorlds: colour ports over their own pixels */

static int gworld_depth(int16_t depth) { return depth > 0 ? depth : qd_screen_depth(); }

static uint32_t row_bytes_for(int w, int depth) { return (uint32_t)((w * depth + 31) / 32 * 4); }

static bool set_pixels(gaddr port, qrect r, int depth, gaddr ctab)
{
    gaddr pm = rd32(rd32(port + CGrafPort_portPixMap_off));
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0)
        return false;
    uint32_t rb = row_bytes_for(w, depth);
    gaddr bits = tb_NewPtr((int32_t)(rb * (uint32_t)h), false, true);
    if (!bits)
        return false;
    if (depth == 32)
        gmemset(bits, 0xFF, rb * (uint32_t)h); /* white */
    PixMap_set_baseAddr(pm, bits);
    PixMap_set_rowBytes(pm, (int16_t)(rb | 0x8000));
    qrect_put(pm + PixMap_bounds_off, r);
    PixMap_set_pixelSize(pm, (int16_t)depth);
    PixMap_set_pixelType(pm, depth > 8 ? 16 : 0);
    PixMap_set_cmpCount(pm, depth > 8 ? 3 : 1);
    PixMap_set_cmpSize(pm, (int16_t)(depth == 32 ? 8 : depth == 16 ? 5 : depth));
    if (depth <= 8) {
        uint32_t ct = ctab ? ctab : tb_GetCTable((int16_t)depth);
        if (ctab)
            tb_HandToHand(&ct);
        PixMap_set_pmTable(pm, ct);
    }
    qrect_put(port + GrafPort_portRect_off, r);
    tb_RectRgn(GrafPort_visRgn(port), port + GrafPort_portRect_off);
    tb_RectRgn(GrafPort_clipRgn(port), port + GrafPort_portRect_off);
    return true;
}

int16_t tb_NewGWorld(gaddr offscreenGWorld, int16_t pixelDepth, gaddr boundsRect, gaddr cTable, gaddr aGDevice,
                     int32_t flags)
{
    gaddr saved = qd_port();
    gaddr port = tb_NewPtr(CGrafPort_SIZEOF, false, true);
    if (!port)
        return memFullErr;
    tb_OpenCPort(port);
    if (!set_pixels(port, qrect_get(boundsRect), gworld_depth(pixelDepth), cTable)) {
        tb_CloseCPort(port);
        tb_DisposePtr(port);
        tb_SetPort(saved);
        return memFullErr;
    }
    tb_SetPort(saved);
    wr32(offscreenGWorld, port);
    return noErr;
}

int32_t tb_UpdateGWorld(gaddr offscreenGWorld, int16_t pixelDepth, gaddr boundsRect, gaddr cTable, gaddr aGDevice,
                        int32_t flags)
{
    gaddr port = rd32(offscreenGWorld);
    gaddr pm = rd32(rd32(port + CGrafPort_portPixMap_off));
    qrect r = qrect_get(boundsRect), cur = qrect_get(pm + PixMap_bounds_off);
    int depth = gworld_depth(pixelDepth);
    if (r.right - r.left == cur.right - cur.left && r.bottom - r.top == cur.bottom - cur.top &&
        depth == PixMap_pixelSize(pm)) {
        qrect_put(pm + PixMap_bounds_off, r);
        qrect_put(port + GrafPort_portRect_off, r);
        return 0;
    }
    gaddr old = PixMap_baseAddr(pm);
    if (!set_pixels(port, r, depth, cTable))
        return memFullErr;
    tb_DisposePtr(old);
    return 1 << 2; /* reallocPix */
}

void tb_DisposeGWorld(gaddr port)
{
    if (!port)
        return;
    gaddr pm = rd32(rd32(port + CGrafPort_portPixMap_off));
    gaddr bits = PixMap_baseAddr(pm);
    if (PixMap_pmTable(pm))
        tb_DisposeHandle(PixMap_pmTable(pm));
    if (qd_port() == port)
        tb_SetPort(rd32(LM_WMgrPort));
    tb_CloseCPort(port);
    tb_DisposePtr(bits);
    tb_DisposePtr(port);
}

void tb_GetGWorld(gaddr port, gaddr gdh)
{
    if (port)
        wr32(port, qd_port());
    if (gdh)
        wr32(gdh, rd32(LM_TheGDevice));
}

void tb_SetGWorld(gaddr port, gaddr gdh)
{
    tb_SetPort(port);
    if (gdh)
        wr32(LM_TheGDevice, gdh);
}

int8_t tb_LockPixels(gaddr pm) { return 1; }
void tb_UnlockPixels(gaddr pm) {}
void tb_AllowPurgePixels(gaddr pm) {}
void tb_NoPurgePixels(gaddr pm) {}
int32_t tb_GetPixelsState(gaddr pm) { return 0x80; /* pixelsLocked */ }
void tb_SetPixelsState(gaddr pm, int32_t state) {}
gaddr tb_GetPixBaseAddr(gaddr pm) { return pm && rd32(pm) ? PixMap_baseAddr(rd32(pm)) : 0; }
int8_t tb_PixMap32Bit(gaddr pm) { return 0; }
gaddr tb_GetGWorldPixMap(gaddr port) { return rd32(port + CGrafPort_portPixMap_off); }
void tb_PortChanged(gaddr port) {}
void tb_PixPatChanged(gaddr ppat) {}

/* ---------------------------------------------------------------------- */
/* Pixels */

void tb_GetCPixel(int16_t h, int16_t v, gaddr colorp)
{
    pixdst d = qd_port_pixels(qd_port());
    uint16_t c[3] = { 0xFFFF, 0xFFFF, 0xFFFF };
    if (h >= d.bounds.left && h < d.bounds.right && v >= d.bounds.top && v < d.bounds.bottom)
        qd_px_rgb(d.depth, d.ctab, get_px(&d, h - d.bounds.left, v - d.bounds.top), c);
    rgb_set(colorp, c);
}

void tb_SetCPixel(int16_t h, int16_t v, gaddr colorp)
{
    gaddr port = qd_port();
    pixdst d = qd_port_pixels(port);
    hrgn clip;
    hrgn_init(&clip);
    qd_port_clip(port, &clip);
    if (hrgn_contains(&clip, h, v) && h >= d.bounds.left && h < d.bounds.right && v >= d.bounds.top &&
        v < d.bounds.bottom) {
        uint16_t c[3];
        rgb_get(colorp, c);
        put_px(&d, h - d.bounds.left, v - d.bounds.top, qd_rgb_px(d.depth, d.ctab, c));
    }
    hrgn_free(&clip);
}

void qd_draw_rgba(const uint8_t *rgba, int w, int h, qrect dst)
{
    gaddr port = qd_port();
    pixdst d = qd_port_pixels(port);
    hrgn clip;
    hrgn_init(&clip);
    qd_port_clip(port, &clip);
    int dw = dst.right - dst.left, dh = dst.bottom - dst.top;
    for (int y = 0; y < dh; y++) {
        int ty = dst.top + y, n;
        const int16_t *xs = hrgn_row(&clip, ty, &n);
        if (!xs || ty < d.bounds.top || ty >= d.bounds.bottom)
            continue;
        int sy = y * h / (dh ? dh : 1);
        for (int x = 0; x < dw; x++) {
            int tx = dst.left + x;
            bool in = false;
            for (int k = 0; k < n; k += 2)
                if (tx >= xs[k] && tx < xs[k + 1]) {
                    in = true;
                    break;
                }
            if (!in || tx < d.bounds.left || tx >= d.bounds.right)
                continue;
            int sx = x * w / (dw ? dw : 1);
            const uint8_t *p = rgba + ((size_t)sy * (size_t)w + (size_t)sx) * 4;
            if (!p[3])
                continue;
            uint16_t c[3] = { (uint16_t)(p[0] * 0x101), (uint16_t)(p[1] * 0x101), (uint16_t)(p[2] * 0x101) };
            put_px(&d, tx - d.bounds.left, ty - d.bounds.top, qd_rgb_px(d.depth, d.ctab, c));
        }
    }
    hrgn_free(&clip);
}

/* ---------------------------------------------------------------------- */
/* Colour icons and cursors */

/* We keep a cicn's resource bytes in the handle we return; PlotCIcon
 * decodes them. */
gaddr tb_GetCIcon(int16_t id)
{
    gaddr r = tb_GetResource((int32_t)fourcc("cicn"), id);
    if (!r || !rd32(r))
        return 0;
    uint32_t h = r;
    tb_HandToHand(&h);
    return h;
}

void tb_PlotCIcon(gaddr rect, gaddr icon)
{
    if (!icon || !rd32(icon))
        return;
    uint32_t len = (uint32_t)tb_GetHandleSize(icon);
    uint8_t *bytes = malloc(len);
    gget(bytes, rd32(icon), len);
    int w, h;
    uint8_t *rgba = qd_cicn_rgba(bytes, len, &w, &h);
    free(bytes);
    if (!rgba)
        return;
    qd_draw_rgba(rgba, w, h, qrect_get(rect));
    free(rgba);
}

void tb_DisposeCIcon(gaddr icon)
{
    if (icon)
        tb_DisposeHandle(icon);
}

gaddr tb_GetCCursor(int16_t id)
{
    gaddr r = tb_GetResource((int32_t)fourcc("crsr"), id);
    if (!r || !rd32(r))
        return 0;
    uint32_t h = r;
    tb_HandToHand(&h);
    return h;
}

void tb_SetCCursor(gaddr c) {} /* the host draws its own pointer */
void tb_DisposeCCursor(gaddr c)
{
    if (c)
        tb_DisposeHandle(c);
}

/* ---------------------------------------------------------------------- */
/* Pixel patterns: drawn through their black-and-white version */

gaddr tb_GetPixPat(int16_t id)
{
    gaddr r = tb_GetResource((int32_t)fourcc("ppat"), id);
    if (!r || !rd32(r))
        return 0;
    uint8_t pat[8];
    gget(pat, rd32(r) + PixPat_pat1Data_off, 8);
    return qd_pixpat_for(pat);
}

void tb_DisposePixPat(gaddr pp)
{
    if (pp)
        tb_DisposeHandle(pp);
}

static void set_port_pixpat(uint32_t color_off, uint32_t old_off, gaddr pp)
{
    gaddr port = qd_port();
    if (!pp || !rd32(pp))
        return;
    if (qd_port_is_color(port)) {
        gaddr cur = rd32(port + color_off);
        if (cur && rd32(cur) && cur != pp)
            gmemcpy(rd32(cur), rd32(pp), PixPat_SIZEOF);
        else if (!cur)
            wr32(port + color_off, pp);
    } else {
        gmemcpy(port + old_off, rd32(pp) + PixPat_pat1Data_off, 8);
    }
}

void tb_PenPixPat(gaddr pp) { set_port_pixpat(CGrafPort_pnPixPat_off, GrafPort_pnPat_off, pp); }
void tb_BackPixPat(gaddr pp) { set_port_pixpat(CGrafPort_bkPixPat_off, GrafPort_bkPat_off, pp); }

/* A colour as a pattern: dithered to black and white by its lightness. */
void tb_MakeRGBPat(gaddr pp, gaddr colorp)
{
    uint16_t c[3];
    rgb_get(colorp, c);
    int lum = (c[0] * 30 + c[1] * 59 + c[2] * 11) / 100 >> 13; /* 0..7 */
    static const uint8_t ramp[8][8] = {
        { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }, { 0xFF, 0xDD, 0xFF, 0x77, 0xFF, 0xDD, 0xFF, 0x77 },
        { 0xEE, 0xBB, 0xEE, 0xBB, 0xEE, 0xBB, 0xEE, 0xBB }, { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 },
        { 0xAA, 0x44, 0xAA, 0x11, 0xAA, 0x44, 0xAA, 0x11 }, { 0x88, 0x22, 0x88, 0x22, 0x88, 0x22, 0x88, 0x22 },
        { 0x80, 0x08, 0x80, 0x08, 0x80, 0x08, 0x80, 0x08 }, { 0, 0, 0, 0, 0, 0, 0, 0 },
    };
    if (!pp || !rd32(pp))
        return;
    PixPat_set_patType(rd32(pp), 2);
    gput(rd32(pp) + PixPat_pat1Data_off, ramp[lum], 8);
}

static void fill_with(void (*fill)(gaddr, gaddr), gaddr r, gaddr pp)
{
    if (!pp || !rd32(pp))
        return;
    gaddr pat = tb_NewPtr(8, false, false);
    gmemcpy(pat, rd32(pp) + PixPat_pat1Data_off, 8);
    fill(r, pat);
    tb_DisposePtr(pat);
}

void tb_FillCRect(gaddr r, gaddr pp) { fill_with(tb_FillRect, r, pp); }
void tb_FillCOval(gaddr r, gaddr pp) { fill_with(tb_FillOval, r, pp); }

/* ---------------------------------------------------------------------- */
/* Palette Manager: palettes are kept and their colours used for drawing;
 * the 8-bit screen's table stays the standard one. */

#define MAX_WINDOW_PALETTES 64
static struct {
    gaddr w, pal;
} window_pal[MAX_WINDOW_PALETTES];

void tb_InitPalettes(void) { memset(window_pal, 0, sizeof window_pal); }

static gaddr entry(gaddr pal, int i) { return rd32(pal) + Palette_pmInfo_off + (uint32_t)i * ColorInfo_SIZEOF; }
static int entries(gaddr pal) { return pal && rd32(pal) ? (int16_t)rd16(rd32(pal) + Palette_pmEntries_off) : 0; }

gaddr tb_NewPalette(int16_t n, gaddr srcColors, int16_t usage, int16_t tolerance)
{
    gaddr h = tb_NewHandle(Palette_pmInfo_off + n * ColorInfo_SIZEOF, false, true);
    if (!h)
        return 0;
    wr16(rd32(h) + Palette_pmEntries_off, (uint16_t)n);
    int src_n = srcColors && rd32(srcColors) ? (int16_t)rd16(rd32(srcColors) + 6) + 1 : 0;
    for (int i = 0; i < n; i++) {
        gaddr e = entry(h, i);
        if (i < src_n)
            gmemcpy(e + ColorInfo_ciRGB_off, rd32(srcColors) + 8 + (uint32_t)i * 8 + 2, 6);
        wr16(e + ColorInfo_ciUsage_off, (uint16_t)usage);
        wr16(e + ColorInfo_ciTolerance_off, (uint16_t)tolerance);
    }
    return h;
}

/* 'pltt': the Palette record as stored (entries, then ColorInfo records). */
gaddr tb_GetNewPalette(int16_t id)
{
    gaddr r = tb_GetResource((int32_t)fourcc("pltt"), id);
    if (!r || !rd32(r))
        return 0;
    uint32_t h = r;
    tb_HandToHand(&h);
    return h;
}

void tb_DisposePalette(gaddr pal)
{
    for (int i = 0; i < MAX_WINDOW_PALETTES; i++)
        if (window_pal[i].pal == pal)
            window_pal[i].w = window_pal[i].pal = 0;
    if (pal)
        tb_DisposeHandle(pal);
}

void tb_ActivatePalette(gaddr w) {}
void tb_RestoreDeviceClut(gaddr gdh) {}

void tb_NSetPalette(gaddr w, gaddr pal, int16_t updates)
{
    for (int i = 0; i < MAX_WINDOW_PALETTES; i++)
        if (window_pal[i].w == w || !window_pal[i].w) {
            window_pal[i].w = w;
            window_pal[i].pal = pal;
            return;
        }
}

void tb_SetPalette(gaddr w, gaddr pal, int8_t updates) { tb_NSetPalette(w, pal, updates ? -1 : 0); }

gaddr tb_GetPalette(gaddr w)
{
    for (int i = 0; i < MAX_WINDOW_PALETTES; i++)
        if (window_pal[i].w == w)
            return window_pal[i].pal;
    return 0;
}

static gaddr current_palette(void)
{
    gaddr p = tb_GetPalette(qd_port());
    return p ? p : tb_GetPalette(0xFFFFFFFFu); /* the application's default */
}

static void pm_color(int16_t i, void (*set)(gaddr))
{
    gaddr pal = current_palette();
    if (i < 0 || i >= entries(pal))
        return;
    gaddr c = tb_NewPtr(6, false, false);
    gmemcpy(c, entry(pal, i) + ColorInfo_ciRGB_off, 6);
    set(c);
    tb_DisposePtr(c);
}

void tb_PmForeColor(int16_t i) { pm_color(i, tb_RGBForeColor); }
void tb_PmBackColor(int16_t i) { pm_color(i, tb_RGBBackColor); }

void tb_GetEntryColor(gaddr pal, int16_t i, gaddr c)
{
    if (i >= 0 && i < entries(pal))
        gmemcpy(c, entry(pal, i) + ColorInfo_ciRGB_off, 6);
}

void tb_SetEntryColor(gaddr pal, int16_t i, gaddr c)
{
    if (i >= 0 && i < entries(pal))
        gmemcpy(entry(pal, i) + ColorInfo_ciRGB_off, c, 6);
}

void tb_GetEntryUsage(gaddr pal, int16_t i, gaddr usage, gaddr tolerance)
{
    if (i < 0 || i >= entries(pal))
        return;
    if (usage)
        wr16(usage, rd16(entry(pal, i) + ColorInfo_ciUsage_off));
    if (tolerance)
        wr16(tolerance, rd16(entry(pal, i) + ColorInfo_ciTolerance_off));
}

void tb_SetEntryUsage(gaddr pal, int16_t i, int16_t usage, int16_t tolerance)
{
    if (i < 0 || i >= entries(pal))
        return;
    if (usage != -1)
        wr16(entry(pal, i) + ColorInfo_ciUsage_off, (uint16_t)usage);
    if (tolerance != -1)
        wr16(entry(pal, i) + ColorInfo_ciTolerance_off, (uint16_t)tolerance);
}

void tb_AnimateEntry(gaddr w, int16_t i, gaddr c) { tb_SetEntryColor(tb_GetPalette(w), i, c); }

void tb_AnimatePalette(gaddr w, gaddr ctab, int16_t index, int16_t dst, int16_t n)
{
    gaddr pal = tb_GetPalette(w);
    for (int k = 0; k < n; k++) {
        gaddr src = rd32(ctab) + 8 + (uint32_t)(index + k) * 8 + 2;
        if (dst + k < entries(pal))
            gmemcpy(entry(pal, dst + k) + ColorInfo_ciRGB_off, src, 6);
    }
}

void tb_CopyPalette(gaddr src, gaddr dst, int16_t src_start, int16_t dst_start, int16_t n)
{
    if (dst_start + n > entries(dst)) {
        tb_SetHandleSize(dst, Palette_pmInfo_off + (dst_start + n) * ColorInfo_SIZEOF);
        wr16(rd32(dst) + Palette_pmEntries_off, (uint16_t)(dst_start + n));
    }
    for (int k = 0; k < n && src_start + k < entries(src); k++)
        gmemcpy(entry(dst, dst_start + k), entry(src, src_start + k), ColorInfo_SIZEOF);
}

void tb_ResizePalette(gaddr pal, int16_t size)
{
    int old = entries(pal);
    tb_SetHandleSize(pal, Palette_pmInfo_off + size * ColorInfo_SIZEOF);
    if (size > old)
        gmemset(entry(pal, old), 0, (uint32_t)(size - old) * ColorInfo_SIZEOF);
    wr16(rd32(pal) + Palette_pmEntries_off, (uint16_t)size);
}

/* ---------------------------------------------------------------------- */
/* Auxiliary records: the standard colours */

int8_t tb_GetAuxiliaryControlRecord(gaddr c, gaddr acHndl)
{
    gaddr h = tb_NewHandle(AuxCtlRec_SIZEOF, false, true);
    wr32(rd32(h) + AuxCtlRec_acOwner_off, c);
    wr32(rd32(h) + AuxCtlRec_acCTable_off, tb_GetCTable(1));
    wr32(acHndl, h);
    return 1;
}

void tb_SetWinColor(gaddr w, gaddr new_w_ctab) {}
