/*
 * Color QuickDraw extras: colour tables and conversions, pixmaps, devices,
 * GWorlds, colour icons and cursors, pixel patterns, the Palette Manager
 * and auxiliary control and window records.
 *
 * The screen is the only real device; GWorlds are colour ports over their
 * own pixel memory, at any depth our drawing code handles (1-32 bits), each
 * with an offscreen device of its own.
 */
#include <stdlib.h>
#include <string.h>

#include "m68k.h"
#include "qd_internal.h"
#include "tb_internal.h"
#include "trap.h"

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
    uint32_t v;
    rgb_get(c, rgb);
    if (qd_search_color(qd_pixmap_base(pm, false), rgb, &v))
        return (int32_t)v;
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
    /* The standard tables: 1, 2, 4, 8 colour; 33-40 the same depths in grey;
     * 66-72 the colour tables with the highlight colour, here the standard
     * ones (the Star Trek demo fades its screen through table 72, writing
     * through the handle without checking it). */
    bool gray = id > 32 && id < 64;
    int depth = id > 64 ? id - 64 : gray ? id - 32 : id;
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8)
        return 0;
    return make_table(1 << depth, gray ? fill_gray : fill_std, depth, id);
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
    qd_device_itable(h);
    return h;
}

void tb_InitGDevice(int16_t refNum, int32_t mode, gaddr gdh) { GDevice_set_gdMode(rd32(gdh), mode); }

void tb_DisposeGDevice(gaddr gdh)
{
    if (!gdh || gdh == rd32(LM_MainDevice))
        return;
    tb_DisposePixMap(GDevice_gdPMap(rd32(gdh)));
    if (rd32(rd32(gdh) + GDevice_gdITable_off))
        tb_DisposeHandle(rd32(rd32(gdh) + GDevice_gdITable_off));
    tb_DisposeHandle(gdh);
}

gaddr tb_GetMaxDevice(gaddr globalRect) { return rd32(LM_MainDevice); }
void tb_GDeviceChanged(gaddr gdh) {}

/* ---------------------------------------------------------------------- */
/* GWorlds: colour ports over their own pixels */

static int gworld_depth(int16_t depth) { return depth > 0 ? depth : qd_screen_depth(); }

/* Depth 0 asks for the screen's depth and takes boundsRect in global
 * coordinates, only to choose the device: the world itself starts at (0,0),
 * as on a Mac (Squark makes its world from its window's global rectangle
 * and draws at 0,0).  The bounds keep the rectangle's alignment within a
 * long word of screen pixels: a left edge of -60 at 1 bit makes bounds
 * (0,-4)-(100,200) under a portRect of (0,0)-(100,200). */
static qrect gworld_bounds(int16_t pixelDepth, qrect r, int depth, qrect *port_rect)
{
    if (pixelDepth == 0) {
        int per = depth < 32 ? 32 / depth : 1, shift = ((r.left % per) + per) % per;
        r = (qrect){ 0, (int16_t)-shift, (int16_t)(r.bottom - r.top), (int16_t)(r.right - r.left) };
    }
    *port_rect = r;
    if (pixelDepth == 0)
        port_rect->left = 0;
    return r;
}

static void set_port_rect(gaddr port, qrect r)
{
    qrect_put(port + GrafPort_portRect_off, r);
    tb_RectRgn(GrafPort_visRgn(port), port + GrafPort_portRect_off);
}

static uint32_t row_bytes_for(int w, int depth) { return (uint32_t)((w * depth + 31) / 32 * 4); }

static bool set_pixels(gaddr port, qrect r, int depth, gaddr ctab)
{
    gaddr pm = rd32(rd32(port + CGrafPort_portPixMap_off));
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0)
        return false;
    /* Rows have room for the image to be realigned to the screen (by up to
     * a long word less a pixel, as UpdateGWorld's alignPix does), so an
     * 8-bit world 32 pixels wide has rowBytes 36.  Programs count on it:
     * Space Pebbles steps its 32-pixel icon worlds by width + 4. */
    int slack = depth < 32 ? 32 / depth - 1 : 0;
    uint32_t rb = row_bytes_for(w + slack, depth);
    /* System 7.5's QuickDraw and later give every depth a long to spare
     * and round rows up to 16 bytes (for video digitizers): the Mac OS 8 of
     * PowerPC programs.  Angel Egg reads its 384-pixel 8-bit backgrounds,
     * saved 400 bytes a row, straight into its world when the file's rows
     * are the world's. */
    if (rd16(LM_SysVersion) >= 0x0750)
        rb = ((uint32_t)((w * depth + 30) / 32 + 1) * 4 + 15) & ~15u;
    /* And a row to spare after the last: tile scrollers copy whole tiles
     * past the right edge, which on the last row runs off the end (Slippy
     * and STARFIGHT write 15 bytes over, scrolled 15 pixels across). */
    /* The pixels are a handle, unlocked (pmVersion 2) until LockPixels. */
    gaddr bits = trap_new_handle_clear((int32_t)(rb * (uint32_t)(h + 1)));
    if (!bits || !rd32(bits))
        return false;
    if (depth == 32)
        gmemset(rd32(bits), 0xFF, rb * (uint32_t)h); /* white */
    PixMap_set_baseAddr(pm, bits);
    PixMap_set_pmVersion(pm, 2);
    PixMap_set_rowBytes(pm, (int16_t)(rb | 0x8000));
    qrect_put(pm + PixMap_bounds_off, r);
    PixMap_set_pixelSize(pm, (int16_t)depth);
    PixMap_set_pixelType(pm, depth > 8 ? 16 : 0);
    PixMap_set_cmpCount(pm, depth > 8 ? 3 : 1);
    PixMap_set_cmpSize(pm, (int16_t)(depth == 32 ? 8 : depth == 16 ? 5 : depth));
    /* The GWorld owns its colour table (a copy of the one asked for, or
     * the standard one for the depth; a direct-colour world gets the 8-bit
     * table), and DisposeGWorld disposes it. */
    uint32_t ct = ctab ? ctab : tb_GetCTable((int16_t)(depth <= 8 ? depth : 8));
    if (ctab)
        tb_HandToHand(&ct);
    if (PixMap_pmTable(pm))
        tb_DisposeHandle(PixMap_pmTable(pm));
    PixMap_set_pmTable(pm, ct);
    qrect_put(port + GrafPort_portRect_off, r);
    /* The visRgn is the world's rectangle; the clipRgn stays as it was (wide
     * open in a new world, as OpenCPort leaves it): programs intersect it
     * with rectangles in coordinates of their own after SetOrigin. */
    tb_RectRgn(GrafPort_visRgn(port), port + GrafPort_portRect_off);
    return true;
}

/* Each GWorld's device: an offscreen GDevice sharing the world's pixmap
 * (unless it was made with noNewDevice), which SetGWorld makes current, so
 * Color2Index and the like answer for the world's depth and colours. */
enum { noNewDevice = 1 << 1 };
static struct gworld_dev {
    gaddr port, gdh;
    bool own;
} *gworlds;
static size_t ngworlds;

void qd_gworlds_reset(void)
{
    free(gworlds);
    gworlds = NULL;
    ngworlds = 0;
}

static struct gworld_dev *gworld_of(gaddr port)
{
    for (size_t i = 0; port && i < ngworlds; i++)
        if (gworlds[i].port == port)
            return &gworlds[i];
    return NULL;
}

static void describe_offscreen_device(gaddr gdh, gaddr port)
{
    gaddr gd = rd32(gdh), pmh = rd32(port + CGrafPort_portPixMap_off);
    GDevice_set_gdPMap(gd, pmh);
    GDevice_set_gdType(gd, PixMap_pixelSize(rd32(pmh)) > 8 ? 2 : 0); /* direct or CLUT */
    gmemcpy(gd + GDevice_gdRect_off, rd32(pmh) + PixMap_bounds_off, 8);
    qd_device_itable(gdh);
}

static void add_gworld(gaddr port, gaddr aGDevice, int32_t flags)
{
    struct gworld_dev *n = realloc(gworlds, (ngworlds + 1) * sizeof *gworlds);
    if (!n)
        return;
    gworlds = n;
    struct gworld_dev *g = &gworlds[ngworlds++];
    g->port = port;
    g->own = !(flags & noNewDevice);
    if (g->own) {
        g->gdh = tb_NewHandle(GDevice_SIZEOF, true, true);
        gaddr gd = rd32(g->gdh);
        GDevice_set_gdRefNum(gd, 0);
        GDevice_set_gdFlags(gd, (int16_t)((1 << 13) | (1 << 10) | 1)); /* noDriver, ramInit, gdDevType */
        GDevice_set_gdResPref(gd, 4); /* inverse tables of 4 bits a component */
        describe_offscreen_device(g->gdh, port);
    } else {
        g->gdh = aGDevice ? aGDevice : rd32(LM_MainDevice);
    }
}

static void remove_gworld(gaddr port)
{
    struct gworld_dev *g = gworld_of(port);
    if (!g)
        return;
    if (rd32(LM_TheGDevice) == g->gdh)
        wr32(LM_TheGDevice, rd32(LM_MainDevice));
    if (g->own) {
        if (rd32(rd32(g->gdh) + GDevice_gdITable_off))
            tb_DisposeHandle(rd32(rd32(g->gdh) + GDevice_gdITable_off));
        tb_DisposeHandle(g->gdh); /* its pixmap is the world's */
    }
    *g = gworlds[--ngworlds];
}

/* The handle holding a GWorld's pixels (whichever the program put there),
 * or 0 for pixels of its own. */
static gaddr pixels_handle(gaddr pm)
{
    gaddr base = PixMap_baseAddr(pm);
    if (PixMap_pmVersion(pm) == 2)
        return base;
    if (PixMap_pmVersion(pm) == 1 && base)
        return tb_RecoverHandle(base, false);
    return 0;
}

int16_t tb_NewGWorld(gaddr offscreenGWorld, int16_t pixelDepth, gaddr boundsRect, gaddr cTable, gaddr aGDevice,
                     int32_t flags)
{
    gaddr saved = qd_port();
    gaddr port = tb_NewPtr(CGrafPort_SIZEOF, false, true);
    if (!port)
        return memFullErr;
    tb_OpenCPort(port);
    /* InitCPort lent the port the screen's colour table; set_pixels gives
     * the world one of its own. */
    PixMap_set_pmTable(rd32(rd32(port + CGrafPort_portPixMap_off)), 0);
    int depth = gworld_depth(pixelDepth);
    qrect pr, r = gworld_bounds(pixelDepth, qrect_get(boundsRect), depth, &pr);
    if (!set_pixels(port, r, depth, cTable)) {
        tb_CloseCPort(port);
        tb_DisposePtr(port);
        tb_SetPort(saved);
        return memFullErr;
    }
    set_port_rect(port, pr);
    tb_SetPort(saved);
    /* A GWorld's port says so: portVersion $C001 (programs choose how to
     * draw by it: Graphing Calculator's surfaces). */
    wr16(port + CGrafPort_portVersion_off, 0xC001);
    add_gworld(port, aGDevice, flags);
    wr32(offscreenGWorld, port);
    return noErr;
}

/* QTNewGWorldFromPtr (CarbonLib's NewGWorldFromPtr): a world drawing into
 * the caller's pixels, of a depth (1 to 32) as the pixel format.  The
 * world is a GWorld's but for its pixels, which stay the caller's. */
int16_t tb_QTNewGWorldFromPtr(gaddr gw, int32_t pixelFormat, gaddr boundsRect, int32_t cTable, int32_t aGDevice,
                              int32_t flags, gaddr baseAddr, int32_t rowBytes)
{
    int depth = pixelFormat;
    if (!gw || !baseAddr || rowBytes <= 0 || rowBytes > 0x3FFF ||
        (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16 && depth != 32))
        return paramErr;
    int16_t err = tb_NewGWorld(gw, (int16_t)depth, boundsRect, (gaddr)cTable, (gaddr)aGDevice, flags);
    if (err)
        return err;
    gaddr pm = rd32(rd32(rd32(gw) + CGrafPort_portPixMap_off));
    gaddr own = pixels_handle(pm);
    PixMap_set_baseAddr(pm, baseAddr);
    PixMap_set_pmVersion(pm, 0);
    PixMap_set_rowBytes(pm, (int16_t)(rowBytes | 0x8000));
    if (own)
        tb_DisposeHandle(own);
    return noErr;
}

int32_t tb_UpdateGWorld(gaddr offscreenGWorld, int16_t pixelDepth, gaddr boundsRect, gaddr cTable, gaddr aGDevice,
                        int32_t flags)
{
    gaddr port = rd32(offscreenGWorld);
    gaddr pm = rd32(rd32(port + CGrafPort_portPixMap_off));
    qrect cur = qrect_get(pm + PixMap_bounds_off);
    int depth = gworld_depth(pixelDepth);
    qrect pr, r = gworld_bounds(pixelDepth, qrect_get(boundsRect), depth, &pr);
    if (r.right - r.left == cur.right - cur.left && r.bottom - r.top == cur.bottom - cur.top &&
        depth == PixMap_pixelSize(pm)) {
        qrect_put(pm + PixMap_bounds_off, r);
        set_port_rect(port, pr);
        return 0;
    }
    bool locked = PixMap_pmVersion(pm) == 1;
    gaddr old = pixels_handle(pm);
    if (!set_pixels(port, r, depth, cTable))
        return memFullErr;
    set_port_rect(port, pr);
    if (old)
        tb_DisposeHandle(old);
    if (locked)
        tb_LockPixels(rd32(port + CGrafPort_portPixMap_off));
    struct gworld_dev *g = gworld_of(port);
    if (g && g->own)
        describe_offscreen_device(g->gdh, port);
    return 1 << 2; /* reallocPix */
}

void tb_DisposeGWorld(gaddr port)
{
    if (!port)
        return;
    gaddr pm = rd32(rd32(port + CGrafPort_portPixMap_off));
    gaddr bits = pixels_handle(pm);
    if (PixMap_pmTable(pm))
        tb_DisposeHandle(PixMap_pmTable(pm));
    if (qd_port() == port)
        tb_SetPort(rd32(LM_WMgrPort));
    remove_gworld(port);
    tb_CloseCPort(port);
    if (bits)
        tb_DisposeHandle(bits);
    tb_DisposePtr(port);
}

void tb_GetGWorld(gaddr port, gaddr gdh)
{
    if (port)
        wr32(port, qd_port());
    if (gdh)
        wr32(gdh, rd32(LM_TheGDevice));
}

/* A GWorld brings its own device and gdh is ignored; for another port gdh
 * becomes current. */
void tb_SetGWorld(gaddr port, gaddr gdh)
{
    tb_SetPort(port);
    struct gworld_dev *g = gworld_of(port);
    if (g)
        wr32(LM_TheGDevice, g->gdh);
    else if (gdh)
        wr32(LM_TheGDevice, gdh);
}

/* As QuickDraw's: LockPixels locks the handle and puts the pixels' address
 * in baseAddr (pmVersion 1); UnlockPixels puts the handle back (2).  Other
 * versions are pixels of the program's own, left alone.  The pixels are
 * never purged. */
int8_t tb_LockPixels(gaddr pmh)
{
    gaddr pm = pmh ? rd32(pmh) : 0;
    if (!pm || PixMap_pmVersion(pm) != 2)
        return 1;
    gaddr h = PixMap_baseAddr(pm);
    if (!h || !rd32(h))
        return 0;
    PixMap_set_baseAddr(pm, rd32(h));
    PixMap_set_pmVersion(pm, 1);
    tb_HLock(h);
    return 1;
}

void tb_UnlockPixels(gaddr pmh)
{
    gaddr pm = pmh ? rd32(pmh) : 0;
    if (!pm || PixMap_pmVersion(pm) != 1)
        return;
    gaddr h = tb_RecoverHandle(PixMap_baseAddr(pm), false);
    if (!h)
        return;
    tb_HUnlock(h);
    PixMap_set_baseAddr(pm, h);
    PixMap_set_pmVersion(pm, 2);
}

void tb_AllowPurgePixels(gaddr pm) {}
void tb_NoPurgePixels(gaddr pm) {}
int32_t tb_GetPixelsState(gaddr pmh)
{
    gaddr pm = pmh ? rd32(pmh) : 0;
    return pm && PixMap_pmVersion(pm) == 2 ? 0 : 0x80; /* pixelsLocked */
}
void tb_SetPixelsState(gaddr pm, int32_t state)
{
    if (state & 0x80)
        tb_LockPixels(pm);
    else
        tb_UnlockPixels(pm);
}
gaddr tb_GetPixBaseAddr(gaddr pm) { return pm && rd32(pm) ? qd_pixmap_base(rd32(pm), false) : 0; }
int8_t tb_PixMap32Bit(gaddr pm) { return 0; }
gaddr tb_GetGWorldPixMap(gaddr port)
{
    /* No port: GetGWorld before InitGraf (Sokoban 3D, checking the depth)
       left nothing current; the screen is what a launched program finds. */
    if (!port) {
        gaddr gd = rd32(LM_TheGDevice);
        return gd && rd32(gd) ? GDevice_gdPMap(rd32(gd)) : 0;
    }
    return rd32(port + CGrafPort_portPixMap_off);
}
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
    /* Not a handle the Memory Manager knows (one already disposed): its
     * size is an error, and nothing is drawn. */
    int32_t size = tb_GetHandleSize(icon);
    if (size <= 0 || size > 0x1000000) {
        LOGF(LOG_UNIMPL, "PlotCIcon: %08X is no colour icon (size %d; it points at %08X: %08X %08X %08X)\n", icon, size,
             rd32(icon), rd32(rd32(icon) - 12), rd32(rd32(icon) - 8), rd32(rd32(icon) - 4));
        return;
    }
    uint32_t len = (uint32_t)size;
    uint8_t *bytes = malloc(len);
    gget(bytes, rd32(icon), len);
    int w, h;
    uint8_t *rgba = qd_cicn_rgba(bytes, len, &w, &h);
    free(bytes);
    if (!rgba)
        return;
    qd_draw_rgba(rgba, w, h, qrect_get(rect));
    qrect at = qrect_get(rect);
    walker_note_drawn(qd_port(), at.top, at.left, at.bottom, at.right);
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

void cursor_set_color(gaddr c);
void tb_SetCCursor(gaddr c)
{
    if (c && rd32(c)) {
        cursor_set_color(c);
        walker_note_cursor(rd32(c) + 20);
    }
}
void tb_DisposeCCursor(gaddr c)
{
    if (c)
        tb_DisposeHandle(c);
}

/* ---------------------------------------------------------------------- */
/* Pixel patterns: their own colours on colour screens (qd_pixpat_image),
 * their black-and-white version elsewhere */

/* A 'ppat' resource: the PixPat, then at the offsets its fields give the
 * PixMap (whose pmTable is the colour table's offset) and the pixels. */
gaddr tb_GetPixPat(int16_t id)
{
    gaddr r = tb_GetResource((int32_t)fourcc("ppat"), id);
    if (!r || !rd32(r))
        return 0;
    uint32_t size = (uint32_t)tb_GetHandleSize(r);
    gaddr res = rd32(r);
    int type = (int16_t)rd16(res + PixPat_patType_off);
    uint32_t map_off = rd32(res + PixPat_patMap_off), data_off = rd32(res + PixPat_patData_off);
    if (type != 1 || map_off + PixMap_SIZEOF > size || data_off >= size) {
        uint8_t pat[8];
        gget(pat, res + PixPat_pat1Data_off, 8);
        return qd_pixpat_for(pat);
    }
    gaddr pp = tb_NewPixPat();
    gaddr p = rd32(pp);
    gmemcpy(p + PixPat_pat1Data_off, rd32(r) + PixPat_pat1Data_off, 8);
    gaddr pmh = rd32(p + PixPat_patMap_off);
    gaddr ctab = rd32(rd32(pmh) + PixMap_pmTable_off);
    gmemcpy(rd32(pmh), rd32(r) + map_off, PixMap_SIZEOF);
    PixMap_set_baseAddr(rd32(pmh), 0);
    uint32_t ct_off = rd32(rd32(r) + map_off + PixMap_pmTable_off);
    if (ct_off && ct_off + 8 <= size) {
        uint32_t n = (uint32_t)rd16(rd32(r) + ct_off + 6) + 1;
        uint32_t len = 8 + n * 8;
        if (ct_off + len > size)
            len = size - ct_off;
        if (!ctab)
            ctab = tb_NewHandle(0, false, true);
        tb_SetHandleSize(ctab, (int32_t)len);
        gmemcpy(rd32(ctab), rd32(r) + ct_off, len);
    }
    PixMap_set_pmTable(rd32(pmh), ctab);
    qrect b = qrect_get(rd32(pmh) + PixMap_bounds_off);
    uint32_t len = (uint32_t)(PixMap_rowBytes(rd32(pmh)) & 0x3FFF) * (uint32_t)(b.bottom - b.top);
    uint32_t end = ct_off > data_off ? ct_off : size;
    if (data_off + len > end)
        len = end - data_off;
    gaddr data = rd32(rd32(pp) + PixPat_patData_off);
    tb_SetHandleSize(data, (int32_t)len);
    gmemcpy(rd32(data), rd32(r) + data_off, len);
    return pp;
}

/* A pixel pattern's colours as w x h RGB triples (malloc'd); false for a
 * black-and-white pattern (type 0) or one without pixels. */
bool qd_pixpat_image(gaddr pp, int *w, int *h, uint16_t (**rgb)[3])
{
    if (!pp || !rd32(pp))
        return false;
    gaddr p = rd32(pp);
    int type = (int16_t)rd16(p + PixPat_patType_off);
    if (type == 2) { /* MakeRGBPat: one colour, kept in patXData */
        gaddr x = rd32(p + PixPat_patXData_off);
        if (!x || !rd32(x) || tb_GetHandleSize(x) < 6)
            return false;
        *w = *h = 1;
        *rgb = malloc(sizeof **rgb);
        for (int i = 0; i < 3; i++)
            (*rgb)[0][i] = rd16(rd32(x) + (uint32_t)i * 2);
        return true;
    }
    if (type != 1)
        return false;
    gaddr pmh = rd32(p + PixPat_patMap_off), data = rd32(p + PixPat_patData_off);
    if (!pmh || !rd32(pmh) || !data || !rd32(data))
        return false;
    gaddr pm = rd32(pmh);
    qrect b = qrect_get(pm + PixMap_bounds_off);
    int ww = b.right - b.left, hh = b.bottom - b.top, depth = PixMap_pixelSize(pm);
    uint32_t rb = (uint32_t)(PixMap_rowBytes(pm) & 0x3FFF);
    if (ww <= 0 || hh <= 0 || ww > 256 || hh > 256 || !depth || (uint32_t)tb_GetHandleSize(data) < rb * (uint32_t)hh)
        return false;
    gaddr ctab = PixMap_pmTable(pm);
    *w = ww;
    *h = hh;
    *rgb = malloc(sizeof **rgb * (size_t)ww * (size_t)hh);
    for (int y = 0; y < hh; y++)
        for (int x = 0; x < ww; x++) {
            gaddr row = rd32(data) + (uint32_t)y * rb;
            uint32_t v;
            if (depth == 32)
                v = rd32(row + (uint32_t)x * 4);
            else if (depth == 16)
                v = rd16(row + (uint32_t)x * 2);
            else {
                int bit = x * depth;
                v = (uint32_t)(rd8(row + (uint32_t)(bit >> 3)) >> (8 - depth - (bit & 7))) & ((1u << depth) - 1);
            }
            qd_px_rgb(depth, ctab, v, (*rgb)[y * ww + x]);
        }
    return true;
}

void tb_DisposePixPat(gaddr pp)
{
    if (pp) {
        qd_port_pixpat_disposed(pp);
        tb_DisposeHandle(pp);
    }
}

/* The pattern itself goes in a colour port, as QuickDraw put it: programs
 * read it back from the port and dispose of it before setting the next
 * (Chromosomethingorother's DisposePixPat, then BackPixPat). */
static void set_port_pixpat(uint32_t color_off, uint32_t old_off, gaddr pp)
{
    gaddr port = qd_port();
    if (!pp || !rd32(pp))
        return;
    if (qd_port_is_color(port)) {
        wr32(port + color_off, pp);
    } else {
        gmemcpy(port + old_off, rd32(pp) + PixPat_pat1Data_off, 8);
    }
}

void tb_PenPixPat(gaddr pp) { set_port_pixpat(CGrafPort_pnPixPat_off, GrafPort_pnPat_off, pp); }
void tb_BackPixPat(gaddr pp) { set_port_pixpat(CGrafPort_bkPixPat_off, GrafPort_bkPat_off, pp); }

/* A colour as a pattern: the colour, and for black and white dithered by
 * its lightness. */
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
    /* The colour itself, for colour screens. */
    gaddr x = rd32(rd32(pp) + PixPat_patXData_off);
    if (!x) {
        x = tb_NewHandle(6, false, true);
        wr32(rd32(pp) + PixPat_patXData_off, x);
    } else {
        tb_SetHandleSize(x, 6);
    }
    if (rd32(x))
        for (int i = 0; i < 3; i++)
            wr16(rd32(x) + (uint32_t)i * 2, c[i]);
}

static void fill_with(void (*fill)(gaddr, gaddr), gaddr r, gaddr pp)
{
    if (!pp || !rd32(pp))
        return;
    gaddr pat = tb_NewPtr(8, false, false);
    gmemcpy(pat, rd32(pp) + PixPat_pat1Data_off, 8);
    qd_fill_pixpat = pp;
    fill(r, pat);
    qd_fill_pixpat = 0;
    tb_DisposePtr(pat);
}

void tb_FillCRect(gaddr r, gaddr pp) { fill_with(tb_FillRect, r, pp); }
void tb_FillCOval(gaddr r, gaddr pp) { fill_with(tb_FillOval, r, pp); }

/* The others take extra arguments, so they copy the pattern themselves. */
static gaddr pat_of(gaddr pp)
{
    if (!pp || !rd32(pp))
        return 0;
    gaddr pat = tb_NewPtr(8, false, false);
    gmemcpy(pat, rd32(pp) + PixPat_pat1Data_off, 8);
    return pat;
}

void tb_FillCRoundRect(gaddr r, int16_t ow, int16_t oh, gaddr pp)
{
    gaddr pat = pat_of(pp);
    if (pat) {
        qd_fill_pixpat = pp;
        tb_FillRoundRect(r, ow, oh, pat);
        qd_fill_pixpat = 0;
        tb_DisposePtr(pat);
    }
}

void tb_FillCArc(gaddr r, int16_t start, int16_t angle, gaddr pp)
{
    gaddr pat = pat_of(pp);
    if (pat) {
        qd_fill_pixpat = pp;
        tb_FillArc(r, start, angle, pat);
        qd_fill_pixpat = 0;
        tb_DisposePtr(pat);
    }
}

void tb_FillCRgn(gaddr rgn, gaddr pp)
{
    gaddr pat = pat_of(pp);
    if (pat) {
        qd_fill_pixpat = pp;
        tb_FillRgn(rgn, pat);
        qd_fill_pixpat = 0;
        tb_DisposePtr(pat);
    }
}

void tb_FillCPoly(gaddr poly, gaddr pp)
{
    gaddr pat = pat_of(pp);
    if (pat) {
        qd_fill_pixpat = pp;
        tb_FillPoly(poly, pat);
        qd_fill_pixpat = 0;
        tb_DisposePtr(pat);
    }
}

/* ---------------------------------------------------------------------- */
/* Palette Manager.  On an indexed screen (2 to 8 bits), activating a
 * window's palette gives its animated entries (and tolerant ones with no
 * close colour) slots of their own in the screen's colour table; PmForeColor
 * and PmBackColor draw with a slot's index, and AnimatePalette and
 * AnimateEntry change the slot's colour, so what was drawn with it changes
 * at once (fades, colour cycling).  Courteous entries draw with the nearest
 * colour.  On a direct screen, palettes only supply colours. */

enum { pmTolerant = 0x0002, pmAnimated = 0x0004, pmExplicit = 0x0008 };
static gaddr device_table(void);
static gaddr current_device(void);
static void bump_seed(gaddr ct);

#define MAX_WINDOW_PALETTES 64
static struct {
    gaddr w, pal;
} window_pal[MAX_WINDOW_PALETTES];

static gaddr default_palette(void);
static bool default_tried; /* the application's 'pltt' 0 looked for */
static gaddr system_palette;

void tb_InitPalettes(void) { memset(window_pal, 0, sizeof window_pal); }

static void qd_control_colors_reset(void);

void qd_palettes_reset(void)
{
    memset(window_pal, 0, sizeof window_pal);
    default_tried = false;
    system_palette = 0;
    qd_control_colors_reset();
}

static gaddr entry(gaddr pal, int i) { return rd32(pal) + Palette_pmInfo_off + (uint32_t)i * ColorInfo_SIZEOF; }
static int entries(gaddr pal) { return pal && rd32(pal) ? (int16_t)rd16(rd32(pal) + Palette_pmEntries_off) : 0; }

/* Slots of the screen's table that palette entries own: a reservation is
 * kept in the entry's ciPrivate as (generation << 16 | index + 1), so a new
 * table (a depth change, RestoreDeviceClut) ends them all. */
static bool claimed[256];
static gaddr claimed_for; /* the table the slots belong to */
static uint16_t generation = 1;

static int table_size(gaddr ct) { return ct && rd32(ct) ? (int16_t)rd16(rd32(ct) + 6) + 1 : 0; }

/* The screen's table, if the screen is indexed; reservations follow it. */
static gaddr indexed_table(void)
{
    if (qd_screen_depth() > 8 || qd_screen_depth() < 2)
        return 0;
    gaddr dt = device_table();
    if (dt != claimed_for) {
        memset(claimed, 0, sizeof claimed);
        claimed_for = dt;
        generation++;
    }
    return dt;
}

/* The slot an entry owns, or -1. */
static int slot_of(gaddr pal, int i)
{
    uint32_t v = rd32(entry(pal, i) + ColorInfo_ciPrivate_off);
    if (!indexed_table() || (v >> 16) != generation || !(v & 0xFFFF))
        return -1;
    return (int)(v & 0xFFFF) - 1;
}

static void set_slot_color(gaddr dt, int idx, gaddr rgb)
{
    gmemcpy(rd32(dt) + 8 + (uint32_t)idx * 8 + 2, rgb, 6);
}

/* Give an entry a slot: its own index if explicit, else a free one from the
 * top; never the first (white) or last (black). */
static int claim_slot(gaddr pal, int i, bool explicit_)
{
    gaddr dt = indexed_table();
    int n = table_size(dt);
    if (n < 4)
        return -1;
    int idx = -1;
    if (explicit_) {
        idx = i % n;
        if (idx == 0 || idx == n - 1)
            return -1;
    } else {
        for (int k = n - 2; k >= 1; k--)
            if (!claimed[k]) {
                idx = k;
                break;
            }
    }
    if (idx < 0)
        return -1;
    claimed[idx] = true;
    wr32(entry(pal, i) + ColorInfo_ciPrivate_off, (uint32_t)generation << 16 | (uint32_t)(idx + 1));
    return idx;
}

static void release_slots(gaddr pal)
{
    for (int i = 0; i < entries(pal); i++) {
        int idx = slot_of(pal, i);
        if (idx >= 0)
            claimed[idx] = false;
        wr32(entry(pal, i) + ColorInfo_ciPrivate_off, 0);
    }
}

/* How far apart two colours are, as the Palette Manager's tolerance
 * measures it: the largest component difference. */
static uint16_t color_distance(gaddr a, gaddr b)
{
    uint16_t d = 0;
    for (int k = 0; k < 3; k++) {
        int x = (int)rd16(a + (uint32_t)k * 2) - (int)rd16(b + (uint32_t)k * 2);
        if (x < 0)
            x = -x;
        if (x > d)
            d = (uint16_t)x;
    }
    return d;
}

static void activate(gaddr pal)
{
    gaddr dt = indexed_table();
    if (!pal || !rd32(pal) || !dt)
        return;
    bool changed = false;
    for (int i = 0; i < entries(pal); i++) {
        gaddr e = entry(pal, i);
        uint16_t usage = rd16(e + ColorInfo_ciUsage_off);
        int idx = slot_of(pal, i);
        if (usage & pmAnimated) {
            if (idx < 0)
                idx = claim_slot(pal, i, usage & pmExplicit);
        } else if (usage & pmTolerant) {
            if (idx < 0) {
                uint16_t best = 0xFFFF;
                for (int k = 0; k < table_size(dt); k++) {
                    uint16_t d = color_distance(rd32(dt) + 8 + (uint32_t)k * 8 + 2, e + ColorInfo_ciRGB_off);
                    if (d < best)
                        best = d;
                }
                if (best > rd16(e + ColorInfo_ciTolerance_off))
                    idx = claim_slot(pal, i, usage & pmExplicit);
            }
        }
        if (idx >= 0) {
            set_slot_color(dt, idx, e + ColorInfo_ciRGB_off);
            changed = true;
        }
    }
    if (changed)
        bump_seed(dt);
}

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
    for (int i = 0; i < entries(h); i++)
        wr32(entry(h, i) + ColorInfo_ciPrivate_off, 0);
    return h;
}

void tb_DisposePalette(gaddr pal)
{
    if (pal && rd32(pal))
        release_slots(pal);
    for (int i = 0; i < MAX_WINDOW_PALETTES; i++)
        if (window_pal[i].pal == pal)
            window_pal[i].w = window_pal[i].pal = 0;
    if (pal)
        tb_DisposeHandle(pal);
}

void tb_ActivatePalette(gaddr w)
{
    gaddr pal = tb_GetPalette(w);
    activate(pal ? pal : default_palette());
}

/* The standard table again, and no slots owned. */
void tb_RestoreDeviceClut(gaddr gdh)
{
    gaddr dt = indexed_table();
    if (!dt)
        return;
    int depth = qd_screen_depth(), n = table_size(dt);
    for (int k = 0; k < n; k++) {
        uint16_t rgb[3];
        qd_px_rgb(depth, 0, (uint32_t)k, rgb);
        for (int c = 0; c < 3; c++)
            wr16(rd32(dt) + 8 + (uint32_t)k * 8 + 2 + (uint32_t)c * 2, rgb[c]);
    }
    memset(claimed, 0, sizeof claimed);
    generation++;
    bump_seed(dt);
}

void tb_NSetPalette(gaddr w, gaddr pal, int16_t updates)
{
    for (int i = 0; i < MAX_WINDOW_PALETTES; i++)
        if (window_pal[i].w == w || !window_pal[i].w) {
            window_pal[i].w = w;
            window_pal[i].pal = pal;
            break;
        }
    if (updates && w && w != 0xFFFFFFFFu && w == rd32(LM_WindowList))
        activate(pal); /* the front window's palette takes effect */
}

void tb_SetPalette(gaddr w, gaddr pal, int8_t updates) { tb_NSetPalette(w, pal, updates ? -1 : 0); }

gaddr tb_GetPalette(gaddr w)
{
    for (int i = 0; i < MAX_WINDOW_PALETTES; i++)
        if (window_pal[i].w == w)
            return window_pal[i].pal;
    return 0;
}

/* The palette of windows without one: the application's default ('pltt'
 * 0, taken when first needed), or else the system's (the standard colour
 * table's colours). */
static gaddr default_palette(void)
{
    gaddr p = tb_GetPalette(0xFFFFFFFFu);
    if (p)
        return p;
    if (!default_tried) {
        default_tried = true;
        p = tb_GetNewPalette(0);
        if (p) {
            tb_NSetPalette(0xFFFFFFFFu, p, 0);
            return p;
        }
    }
    if (!system_palette) {
        gaddr ct = tb_GetCTable(8);
        if (ct) {
            system_palette = tb_NewPalette(256, ct, 0, 0);
            tb_DisposeCTable(ct);
        }
    }
    return system_palette;
}

static gaddr current_palette(void)
{
    gaddr p = tb_GetPalette(qd_port());
    return p ? p : default_palette();
}

static void pm_color(int16_t i, bool fore)
{
    gaddr pal = current_palette();
    if (i < 0 || i >= entries(pal))
        return;
    gaddr c = tb_NewPtr(6, false, false);
    gmemcpy(c, entry(pal, i) + ColorInfo_ciRGB_off, 6);
    if (fore)
        tb_RGBForeColor(c);
    else
        tb_RGBBackColor(c);
    tb_DisposePtr(c);
    /* An entry with a slot of its own draws with that index (and the port's
     * fgColor or bkColor says so, which programs read back). */
    int idx = slot_of(pal, i);
    if (idx >= 0)
        qd_port_set_index(qd_port(), fore, (uint32_t)idx);
}

void tb_PmForeColor(int16_t i) { pm_color(i, true); }
void tb_PmBackColor(int16_t i) { pm_color(i, false); }

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

/* A new colour for an entry, and for its slot on the screen if it has one. */
static bool animate_entry(gaddr pal, int i, gaddr rgb)
{
    if (i < 0 || i >= entries(pal))
        return false;
    gmemcpy(entry(pal, i) + ColorInfo_ciRGB_off, rgb, 6);
    int idx = slot_of(pal, i);
    if (idx < 0 || !(rd16(entry(pal, i) + ColorInfo_ciUsage_off) & pmAnimated))
        return false;
    set_slot_color(device_table(), idx, rgb);
    return true;
}

static gaddr palette_of_window(gaddr w)
{
    gaddr pal = tb_GetPalette(w);
    return pal ? pal : tb_GetPalette(0xFFFFFFFFu);
}

void tb_AnimateEntry(gaddr w, int16_t i, gaddr c)
{
    if (animate_entry(palette_of_window(w), i, c))
        bump_seed(device_table());
}

void tb_AnimatePalette(gaddr w, gaddr ctab, int16_t index, int16_t dst, int16_t n)
{
    gaddr pal = palette_of_window(w);
    if (!ctab || !rd32(ctab))
        return;
    bool changed = false;
    for (int k = 0; k < n && index + k < table_size(ctab); k++)
        changed |= animate_entry(pal, dst + k, rd32(ctab) + 8 + (uint32_t)(index + k) * 8 + 2);
    if (changed)
        bump_seed(device_table());
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

/* The default record (owner nil) holds the standard control colours, the
 * System's cctb 0: black frame and text on a white body and thumb.  A
 * control has a record of its own only once it is given colours
 * (SetControlColor, or a cctb resource with its CNTL's ID); GetAuxCtl
 * answers false and the default for the others, as on a Mac, so a CDEF
 * that tells custom colours from the standard ones by the table sees
 * the standard one (Grayscale Buttons' radio buttons drew on black). */
static struct {
    gaddr control, aux;
} auxes[128];
static gaddr default_aux_h;

static gaddr default_aux(void)
{
    gaddr h = default_aux_h;
    if (h && rd32(h))
        return h;
    static const uint16_t rgb[4] = { 0x0000, 0xFFFF, 0x0000, 0xFFFF }; /* frame, body, text, thumb */
    gaddr ct = tb_NewHandle(8 + 4 * 8, true, true);
    wr32(rd32(ct), 0);                                           /* ctSeed */
    wr16(rd32(ct) + 4, 0);                                       /* ctFlags */
    wr16(rd32(ct) + 6, 3);                                       /* ctSize: entries less one */
    for (int i = 0; i < 4; i++) {
        gaddr e = rd32(ct) + 8 + (uint32_t)i * 8;
        wr16(e, (uint16_t)i);
        wr16(e + 2, rgb[i]), wr16(e + 4, rgb[i]), wr16(e + 6, rgb[i]);
    }
    h = default_aux_h = tb_NewHandle(AuxCtlRec_SIZEOF, true, true);
    wr32(rd32(h) + AuxCtlRec_acCTable_off, ct);
    return h;
}

static int aux_slot(gaddr c)
{
    for (int i = 0; i < 128; i++)
        if (c && auxes[i].control == c && auxes[i].aux && rd32(auxes[i].aux))
            return i;
    return -1;
}

int8_t tb_GetAuxiliaryControlRecord(gaddr c, gaddr acHndl)
{
    int i = aux_slot(c);
    wr32(acHndl, i >= 0 ? auxes[i].aux : default_aux());
    return i >= 0;
}

void tb_SetControlColor(gaddr ctl, gaddr ctab)
{
    if (!ctl)
        return;
    int i = aux_slot(ctl);
    if (i < 0) {
        for (i = 0; i < 128 && auxes[i].control && auxes[i].aux && rd32(auxes[i].aux); i++)
            ;
        if (i == 128)
            i = (int)(ctl % 128);
        else if (auxes[i].aux)
            tb_DisposeHandle(auxes[i].aux);
        gaddr h = tb_NewHandle(AuxCtlRec_SIZEOF, false, true);
        wr32(rd32(h) + AuxCtlRec_acOwner_off, ctl);
        auxes[i].control = ctl;
        auxes[i].aux = h;
    }
    wr32(rd32(auxes[i].aux) + AuxCtlRec_acCTable_off, ctab);
}

/* A new heap: the records went with the old one. */
static void qd_control_colors_reset(void)
{
    memset(auxes, 0, sizeof auxes);
    default_aux_h = 0;
}

/* DisposeControl: its colours go with it. */
void qd_forget_control_colors(gaddr ctl)
{
    int i = aux_slot(ctl);
    if (i < 0)
        return;
    tb_DisposeHandle(auxes[i].aux);
    auxes[i].control = auxes[i].aux = 0;
}


/* ---------------------------------------------------------------------- */
/* Device colour tables: SetEntries changes the screen's colours (palette
 * animation): a new seed, and the display's colours. */

static gaddr device_table(void)
{
    gaddr pm = device_pixmap();
    return pm ? PixMap_pmTable(pm) : 0;
}

static void bump_seed(gaddr ct)
{
    if (ct && rd32(ct))
        wr32(rd32(ct), rd32(rd32(ct)) + 1);
    qd_screen_load_clut(ct);
}

int32_t tb_GetCTSeed(void)
{
    static int32_t seed = 1024;
    return ++seed;
}

/* count is the number of entries less one; start -1 takes each entry's
 * value as its index. */
void tb_SetEntries(int16_t start, int16_t count, gaddr specs)
{
    gaddr ct = device_table();
    if (!ct || !rd32(ct))
        return;
    int n = (int16_t)rd16(rd32(ct) + 6) + 1;
    for (int i = 0; i <= count; i++) {
        gaddr s = specs + (uint32_t)i * 8;
        int index = start < 0 ? (int16_t)rd16(s) : start + i;
        if (index < 0 || index >= n)
            continue;
        gmemcpy(rd32(ct) + 8 + (uint32_t)index * 8 + 2, s + 2, 6);
    }
    bump_seed(ct);
    if (rd32(rd32(current_device()) + GDevice_gdITable_off))
        qd_device_itable(current_device());
}

/* The selection is a ReqListRec (a pointer, not a handle): reqLSize, the
 * number of entries less one, then that many entry numbers. */
void tb_SaveEntries(gaddr src, gaddr result, gaddr selection)
{
    gaddr ct = src ? src : device_table();
    if (!ct || !rd32(ct) || !result || !selection)
        return;
    int n = (int16_t)rd16(selection) + 1, have = (int16_t)rd16(rd32(ct) + 6) + 1;
    if (n < 1)
        return;
    tb_SetHandleSize(result, 8 + n * 8);
    if (!rd32(result) || tb_GetHandleSize(result) < 8 + n * 8)
        return;
    wr32(rd32(result), 0);
    wr16(rd32(result) + 4, 0);
    wr16(rd32(result) + 6, (uint16_t)(n - 1));
    for (int i = 0; i < n; i++) {
        int index = (int16_t)rd16(selection + 2 + (uint32_t)i * 2);
        gaddr d = rd32(result) + 8 + (uint32_t)i * 8;
        wr16(d, (uint16_t)index);
        if (index >= 0 && index < have)
            gmemcpy(d + 2, rd32(ct) + 8 + (uint32_t)index * 8 + 2, 6);
    }
}

/* srcTable's entries, in order, go to the entries of dstTable (the
 * device's, and the screen, when nil) that the selection names. */
void tb_RestoreEntries(gaddr src, gaddr dst, gaddr selection)
{
    if (!src || !rd32(src) || !selection)
        return;
    int n = (int16_t)rd16(rd32(src) + 6) + 1, want = (int16_t)rd16(selection) + 1;
    if (want < n)
        n = want;
    if (n < 1)
        return;
    if (!dst || dst == device_table()) {
        gaddr tmp = tb_NewPtr(n * 8, false, false);
        if (!tmp)
            return;
        gmemcpy(tmp, rd32(src) + 8, (uint32_t)n * 8);
        for (int i = 0; i < n; i++)
            wr16(tmp + (uint32_t)i * 8, rd16(selection + 2 + (uint32_t)i * 2));
        tb_SetEntries(-1, (int16_t)(n - 1), tmp);
        tb_DisposePtr(tmp);
        return;
    }
    if (!rd32(dst))
        return;
    int have = (int16_t)rd16(rd32(dst) + 6) + 1;
    for (int i = 0; i < n; i++) {
        int index = (int16_t)rd16(selection + 2 + (uint32_t)i * 2);
        if (index >= 0 && index < have)
            gmemcpy(rd32(dst) + 8 + (uint32_t)index * 8 + 2, rd32(src) + 8 + (uint32_t)i * 8 + 2, 6);
    }
    bump_seed(dst);
}

void tb_ProtectEntry(int16_t index, int8_t protect) {}
void tb_ReserveEntry(int16_t index, int8_t reserve) {}
void tb_SetClientID(int16_t id) {}
/* An inverse table: iTabSeed (the colour table's seed), iTabRes, then for
 * every colour cut to res bits a component (red highest) the index of
 * the nearest entry.  Programs that set pixels themselves look colours up
 * in their device's (Graphing Calculator's surfaces). */
void tb_MakeITable(gaddr ctab, gaddr itab, int16_t res)
{
    gaddr gdh = current_device();
    if (!ctab && gdh)
        ctab = PixMap_pmTable(rd32(GDevice_gdPMap(rd32(gdh))));
    if (!itab && gdh)
        itab = rd32(rd32(gdh) + GDevice_gdITable_off);
    if (!ctab || !rd32(ctab) || !itab)
        return;
    if (res < 3 || res > 5)
        res = 4;
    int n = (int16_t)rd16(rd32(ctab) + 6) + 1;
    int depth = n <= 2 ? 1 : n <= 4 ? 2 : n <= 16 ? 4 : 8;
    uint32_t cells = 1u << (3 * res);
    tb_SetHandleSize(itab, (int32_t)(6 + cells));
    if (tb_GetHandleSize(itab) < (int32_t)(6 + cells))
        return;
    gaddr t = rd32(itab);
    wr32(t, rd32(rd32(ctab)));
    wr16(t + 4, (uint16_t)res);
    uint32_t mask = (1u << res) - 1;
    for (uint32_t i = 0; i < cells; i++) {
        uint16_t rgb[3];
        for (int k = 0; k < 3; k++) {
            uint32_t q = (i >> (res * (2 - k))) & mask; /* the cell's value, spread over 16 bits */
            rgb[k] = (uint16_t)(q * 0xFFFFu / mask);
        }
        wr8(t + 6 + i, (uint8_t)qd_rgb_px(depth, ctab, rgb));
    }
}

/* Give a device its inverse table handle, as every device had one (a
 * direct device's unbuilt: Color It! stores the colour seed in it), and
 * an indexed device the table for its colour table. */
void qd_device_itable(gaddr gdh)
{
    if (!gdh || !rd32(gdh))
        return;
    gaddr itab = rd32(rd32(gdh) + GDevice_gdITable_off);
    if (!itab) {
        itab = tb_NewHandle(6, true, true);
        if (!itab)
            return;
        wr32(rd32(gdh) + GDevice_gdITable_off, itab);
        wr16(rd32(itab) + 4, 4); /* iTabRes */
    }
    gaddr pmh = GDevice_gdPMap(rd32(gdh));
    if (!pmh || !rd32(pmh) || PixMap_pixelSize(rd32(pmh)) > 8 || !PixMap_pmTable(rd32(pmh)))
        return;
    int16_t res = (int16_t)rd16(rd32(gdh) + GDevice_gdResPref_off);
    tb_MakeITable(PixMap_pmTable(rd32(pmh)), itab, res < 3 ? 4 : res);
}
void tb_AddComp(gaddr proc) {}

/* ---- Search procedures ----
 * The current device's chain of SProcRecs (nxtSrch, srchProc), newest
 * first.  Each is asked for the pixel value of a colour before the colour
 * table is searched: programs use them to map colours of their own (codes
 * for shades, Graphing Calculator) to entries they chose. */

static gaddr current_device(void)
{
    gaddr gd = rd32(LM_TheGDevice);
    return gd && rd32(gd) ? gd : rd32(LM_MainDevice);
}

void tb_AddSearch(gaddr proc)
{
    gaddr gdh = current_device();
    gaddr h = tb_NewHandle(8, false, true);
    if (!gdh || !h)
        return;
    wr32(rd32(h), rd32(rd32(gdh) + GDevice_gdSearchProc_off));
    wr32(rd32(h) + 4, proc);
    wr32(rd32(gdh) + GDevice_gdSearchProc_off, h);
}

void tb_DelSearch(gaddr proc)
{
    gaddr gdh = current_device();
    if (!gdh)
        return;
    gaddr link = rd32(gdh) + GDevice_gdSearchProc_off;
    for (gaddr h = rd32(link); h && rd32(h); link = rd32(h), h = rd32(link)) {
        if (rd32(rd32(h) + 4) == proc) {
            wr32(link, rd32(rd32(h)));
            tb_DisposeHandle(h);
            return;
        }
    }
}

/* The current device's search procedures' pixel value for a colour, when
 * one answers and the pixels drawn into (base) are the device's own. */
bool qd_search_color(gaddr base, const uint16_t rgb[3], uint32_t *out)
{
    static bool busy; /* a search procedure that draws maps its colours plainly */
    gaddr gdh = current_device();
    if (busy || !gdh)
        return false;
    gaddr h = rd32(rd32(gdh) + GDevice_gdSearchProc_off);
    gaddr pmh = GDevice_gdPMap(rd32(gdh));
    if (!h || !pmh || !rd32(pmh) || qd_pixmap_base(rd32(pmh), false) != base)
        return false;
    cpu68k *c = g_cpu;
    busy = true;
    bool found = false;
    for (; h && rd32(h) && !found && !c->halted; h = rd32(rd32(h))) {
        /* FUNCTION (VAR rgb: RGBColor; VAR position: LONGINT): BOOLEAN */
        c->a[7] -= 12;
        gaddr rgbp = c->a[7], posp = c->a[7] + 8;
        wr16(rgbp, rgb[0]);
        wr16(rgbp + 2, rgb[1]);
        wr16(rgbp + 4, rgb[2]);
        wr32(posp, 0);
        cpu_push16(c, 0);
        cpu_push32(c, rgbp);
        cpu_push32(c, posp);
        guest_call(rd32(rd32(h) + 4));
        found = (cpu_pop16(c) >> 8) != 0;
        if (found)
            *out = rd32(posp);
        c->a[7] += 12;
    }
    busy = false;
    return found;
}
void tb_DelComp(gaddr proc) {}
void tb_AllocCursor(void) {}

/* Whether the screen can show the colour exactly (not dithered). */
int8_t tb_RealColor(gaddr color)
{
    if (qd_screen_depth() > 8)
        return 1;
    uint16_t c[3], back[3];
    rgb_get(color, c);
    gaddr pm = device_pixmap();
    uint32_t v = qd_rgb_px(PixMap_pixelSize(pm), PixMap_pmTable(pm), c);
    qd_px_rgb(PixMap_pixelSize(pm), PixMap_pmTable(pm), v, back);
    return (c[0] >> 8) == (back[0] >> 8) && (c[1] >> 8) == (back[1] >> 8) && (c[2] >> 8) == (back[2] >> 8);
}

void tb_CopyPixPat(gaddr src, gaddr dst)
{
    if (!src || !dst || !rd32(src))
        return;
    int32_t n = tb_GetHandleSize(src);
    tb_SetHandleSize(dst, n);
    gmemcpy(rd32(dst), rd32(src), (uint32_t)n);
}

void tb_SetDeviceAttribute(gaddr gdh, int16_t attribute, int8_t value)
{
    if (!gdh || !rd32(gdh) || attribute < 0 || attribute > 15)
        return;
    uint16_t f = (uint16_t)GDevice_gdFlags(rd32(gdh));
    f = value ? (uint16_t)(f | (1u << attribute)) : (uint16_t)(f & ~(1u << attribute));
    GDevice_set_gdFlags(rd32(gdh), (int16_t)f);
}

/* One device: the drawing procedure runs once, for the screen. */
void tb_DeviceLoop(gaddr rgn, gaddr proc, int32_t data, uint32_t flags)
{
    gaddr gd = rd32(LM_MainDevice);
    if (!proc)
        return;
    cpu68k *c = g_cpu;
    cpu_push16(c, (uint16_t)qd_screen_depth());
    cpu_push16(c, (uint16_t)(gd && rd32(gd) ? GDevice_gdFlags(rd32(gd)) : 0));
    cpu_push32(c, gd);
    cpu_push32(c, (uint32_t)data);
    guest_call(proc);
}

/* ---------------------------------------------------------------------- */
/* Palette Manager odds and ends */

int32_t tb_Entry2Index(int16_t i)
{
    gaddr pal = current_palette();
    if (i < 0 || i >= entries(pal))
        return 0;
    return tb_Color2Index(entry(pal, i) + ColorInfo_ciRGB_off);
}

void tb_SaveFore(gaddr cs) { wr16(cs, 0); tb_GetForeColor(cs + 2); }
void tb_SaveBack(gaddr cs) { wr16(cs, 0); tb_GetBackColor(cs + 2); }
void tb_RestoreFore(gaddr cs) { tb_RGBForeColor(cs + 2); }
void tb_RestoreBack(gaddr cs) { tb_RGBBackColor(cs + 2); }

/* The screen can be 1, 8, 16 or 32 bits deep.  SetDepth asks for the mode's
 * depth, or (32-bit mode numbers aside) the mode number 0x80.. itself. */
static int mode_depth(int16_t depth)
{
    if (depth >= 0x80 && depth <= 0x85)
        return 1 << (depth - 0x80);
    return depth;
}

int16_t tb_SetDepth(gaddr gdh, int16_t depth, int16_t which, int16_t flags)
{
    int before = qd_screen_depth();
    if (!qd_screen_set_depth(mode_depth(depth)))
        return -1;
    if (qd_screen_depth() != before)
        display_want_screen("it changes the screen's depth"); /* a game's, usually */
    return noErr;
}

int16_t tb_HasDepth(gaddr gdh, int16_t depth, int16_t which, int16_t flags)
{
    int d = mode_depth(depth);
    int log2 = 0;
    while ((1 << log2) < d)
        log2++;
    return qd_screen_supports(d) ? 0x0080 + log2 : 0; /* the mode for the depth */
}

int16_t tb_PMgrVersion(void) { return 0x0202; }

static int16_t palette_updates;
void tb_SetPaletteUpdates(gaddr pal, int16_t updates) { palette_updates = updates; }
int16_t tb_GetPaletteUpdates(gaddr pal) { return palette_updates; }

/* The grey between two colours: their average luminance. */
int8_t tb_GetGray(gaddr gdh, gaddr back, gaddr fore)
{
    uint16_t a[3], b[3];
    rgb_get(back, a);
    rgb_get(fore, b);
    for (int i = 0; i < 3; i++)
        a[i] = (uint16_t)(((uint32_t)a[i] + b[i]) / 2);
    rgb_set(fore, a);
    return 1;
}

/* ---------------------------------------------------------------------- */
/* The rest of QDExtensions */

gaddr tb_GetGWorldDevice(gaddr port)
{
    struct gworld_dev *g = gworld_of(port);
    return g ? g->gdh : rd32(LM_MainDevice);
}
int8_t tb_QDDone(gaddr port) { return 1; }  /* drawing is never pending */
int32_t tb_OffscreenVersion(void) { return 0x130; }

/* A pixmap at the screen's depth for a global rectangle. */
static int16_t screen_buffer(gaddr rect, gaddr gdhp, gaddr pmp)
{
    qrect r = qrect_get(rect);
    int depth = qd_screen_depth(), w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0)
        return paramErr;
    gaddr pmh = tb_NewPixMap();
    uint32_t rb = row_bytes_for(w, depth);
    gaddr bits = tb_NewPtr((int32_t)(rb * (uint32_t)h), false, true);
    if (!pmh || !bits)
        return memFullErr;
    gaddr pm = rd32(pmh);
    PixMap_set_baseAddr(pm, bits);
    PixMap_set_rowBytes(pm, (int16_t)(rb | 0x8000));
    qrect_put(pm + PixMap_bounds_off, r);
    PixMap_set_pixelSize(pm, (int16_t)depth);
    wr32(gdhp, rd32(LM_MainDevice));
    wr32(pmp, pmh);
    return noErr;
}

int16_t tb_NewScreenBuffer(gaddr rect, int8_t purgeable, gaddr gdhp, gaddr pmp) { return screen_buffer(rect, gdhp, pmp); }
int16_t tb_NewTempScreenBuffer(gaddr rect, int8_t purgeable, gaddr gdhp, gaddr pmp)
{
    return screen_buffer(rect, gdhp, pmp);
}

void tb_DisposeScreenBuffer(gaddr pmh)
{
    if (!pmh || !rd32(pmh))
        return;
    tb_DisposePtr(PixMap_baseAddr(rd32(pmh)));
    tb_DisposePixMap(pmh);
}

/* Palettes and colour tables, entry for entry. */
void tb_Palette2CTab(gaddr pal, gaddr ctab)
{
    int n = entries(pal);
    if (!ctab || n <= 0)
        return;
    tb_SetHandleSize(ctab, 8 + n * 8);
    gaddr p = rd32(ctab);
    wr32(p, (uint32_t)tb_GetCTSeed());
    wr16(p + 4, 0);
    wr16(p + 6, (uint16_t)(n - 1));
    for (int i = 0; i < n; i++) {
        wr16(p + 8 + (uint32_t)i * 8, (uint16_t)i);
        gmemcpy(p + 8 + (uint32_t)i * 8 + 2, entry(pal, i) + ColorInfo_ciRGB_off, 6);
    }
}

void tb_CTab2Palette(gaddr ctab, gaddr pal, int16_t usage, int16_t tolerance)
{
    if (!ctab || !rd32(ctab) || !pal)
        return;
    int n = (int16_t)rd16(rd32(ctab) + 6) + 1;
    tb_SetHandleSize(pal, Palette_pmInfo_off + n * ColorInfo_SIZEOF);
    wr16(rd32(pal) + Palette_pmEntries_off, (uint16_t)n);
    for (int i = 0; i < n; i++) {
        gaddr e = entry(pal, i);
        gmemset(e, 0, ColorInfo_SIZEOF);
        gmemcpy(e + ColorInfo_ciRGB_off, rd32(ctab) + 8 + (uint32_t)i * 8 + 2, 6);
        wr16(e + ColorInfo_ciUsage_off, (uint16_t)usage);
        wr16(e + ColorInfo_ciTolerance_off, (uint16_t)tolerance);
    }
}

void tb_SetPortPix(gaddr pm)
{
    gaddr port = qd_port();
    if (port && qd_port_is_color(port) && pm)
        wr32(port + CGrafPort_portPixMap_off, pm);
}
