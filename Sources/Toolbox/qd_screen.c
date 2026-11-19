/*
 * The screen: a framebuffer at GA_SCREEN (1, 8, 16 or 32 bits deep, as
 * deep as fits there), the main GDevice describing it, and PNG snapshots
 * for headless runs.  SetDepth can change the depth while an application
 * runs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "machine.h"
#include "present.h"
#include "qd_internal.h"
#include "tb_internal.h"

int g_screen_width = 1024, g_screen_height = 768;
static uint32_t screen_row_bytes;
static int depth_pref = 32, screen_depth = 1;
static gaddr screen_ctab;
static unsigned nports; /* ports drawing on the screen, below */

void machine_set_screen_depth(int depth)
{
    depth_pref = depth == 2 || depth == 4 || depth == 8 || depth == 16 || depth == 32 ? depth : 1;
}
int machine_screen_depth_setting(void) { return depth_pref; }
int qd_screen_depth(void) { return screen_depth; }
int machine_screen_depth(void) { return screen_depth; }
gaddr qd_screen_ctab(void) { return screen_depth == 1 ? 0 : screen_ctab; }

uint32_t qd_screen_row_bytes(void) { return screen_row_bytes; }

/* The video hardware's colours, from the device's table only when a routine
 * changes the screen's colours: a program may write colours of its own into
 * the table to build an inverse table for drawing, then fade them in with
 * SetEntries (Apeiron's splash), and the screen never shows them before. */
static uint8_t clut[256][3];

void qd_screen_load_clut(gaddr ctab)
{
    if (!ctab || ctab != screen_ctab || screen_depth <= 1 || screen_depth > 8 || !rd32(ctab))
        return;
    for (int i = 0; i < (1 << screen_depth); i++) {
        uint16_t c[3];
        qd_px_rgb(screen_depth, ctab, (uint32_t)i, c);
        qd_screen_set_hw_entry(i, c);
    }
}

/* An entry of the video hardware's table (the driver's SetEntries, which
 * leaves the device's colour table as it is), luminance-mapped in the
 * driver's gray mode. */
void qd_screen_set_hw_entry(int index, const uint16_t rgb[3])
{
    if (index < 0 || index > 255)
        return;
    uint16_t c[3] = { rgb[0], rgb[1], rgb[2] };
    if (video_gray())
        c[0] = c[1] = c[2] = (uint16_t)(((uint32_t)c[0] * 30 + (uint32_t)c[1] * 59 + (uint32_t)c[2] * 11) / 100);
    for (int k = 0; k < 3; k++)
        clut[index][k] = (uint8_t)(c[k] >> 8);
}

void qd_screen_get_hw_entry(int index, uint16_t rgb[3])
{
    for (int k = 0; k < 3; k++)
        rgb[k] = index >= 0 && index < 256 ? (uint16_t)(clut[index][k] * 0x101) : 0;
}

/* A color table handle: entries as (value, r, g, b). */
static gaddr make_ctab(const uint16_t (*rgb)[3], int n, int32_t seed)
{
    gaddr h = tb_NewHandle(8 + n * 8, true, true);
    gaddr p = rd32(h);
    wr32(p, (uint32_t)seed);  /* ctSeed */
    wr16(p + 4, 0x8000);      /* ctFlags: device table */
    wr16(p + 6, (uint16_t)(n - 1));
    for (int i = 0; i < n; i++) {
        gaddr e = p + 8 + (uint32_t)i * 8;
        wr16(e, (uint16_t)i);
        wr16(e + 2, rgb[i][0]);
        wr16(e + 4, rgb[i][1]);
        wr16(e + 6, rgb[i][2]);
    }
    return h;
}

static uint32_t row_bytes_for(int depth, int width)
{
    /* Whole words, as QuickDraw asks and a Mac's video drivers gave (at 1
     * bit a 1200-pixel row is 150 bytes, not 152); the same as whole longs
     * on the widths a rootless screen has (multiples of 64). */
    return (uint32_t)((width * depth + 15) / 16 * 2);
}

/* The main device's pixel map, colour table and type for screen_depth. */
static void describe_screen(gaddr gd, gaddr pm)
{
    static const uint16_t bw[2][3] = { { 0xFFFF, 0xFFFF, 0xFFFF }, { 0, 0, 0 } };
    static uint16_t std[256][3];
    PixMap_set_rowBytes(pm, (int16_t)(screen_row_bytes | 0x8000));
    PixMap_set_pixelSize(pm, (int16_t)screen_depth);
    if (screen_depth >= 16) {
        PixMap_set_pixelType(pm, 16);       /* RGBDirect */
        PixMap_set_cmpCount(pm, 3);
        PixMap_set_cmpSize(pm, screen_depth == 16 ? 5 : 8);
        /* Unused by direct pixels, but a full 256 colours (the standard
         * 8-bit ones): games written for 256 colours fade by writing all
         * 256 entries into the device's table and calling SetEntries, and
         * Squish wrote 2K past a two-entry table. */
        for (int i = 0; i < 256; i++)
            qd_px_rgb(8, 0, (uint32_t)i, std[i]);
        screen_ctab = make_ctab(std, 256, 1);
    } else {
        PixMap_set_pixelType(pm, 0);
        PixMap_set_cmpCount(pm, 1);
        PixMap_set_cmpSize(pm, (int16_t)screen_depth);
        /* The standard table for the depth (clut 2, 4 or 8). */
        int n = 1 << screen_depth;
        for (int i = 0; i < n && screen_depth > 1; i++)
            qd_px_rgb(screen_depth, 0, (uint32_t)i, std[i]);
        screen_ctab = screen_depth == 1 ? make_ctab(bw, 2, 1) : make_ctab(std, n, screen_depth);
    }
    PixMap_set_pmTable(pm, screen_ctab);
    qd_screen_load_clut(screen_ctab);
    GDevice_set_gdType(gd, screen_depth >= 16 ? 2 : 0); /* direct or CLUT device */
    /* The video driver's mode for the depth, as Apple's drivers numbered
     * them (128 for 1 bit up to 133 for 32): Bubblomania 1.0 wants 131,
     * 8 bits, or more. */
    int mode = 0x80;
    for (int d = screen_depth; d > 1; d >>= 1)
        mode++;
    GDevice_set_gdMode(gd, mode);
    wr16(LM_ScreenRow, (uint16_t)screen_row_bytes);
    video_mode_changed();
}

/* Whether the screen fits its space at a depth: in 24-bit mode that is
 * the megabyte of slot space a video card had, or a compact Mac's one-bit
 * page in RAM. */
static bool fits(int depth, int width, int height)
{
    return (uint64_t)row_bytes_for(depth, width) * (uint32_t)height <= g_screen_size;
}

static gaddr display_base; /* see qd_screen_set_display_base; qd_screen_init sets it */

void qd_screen_init(int width, int height)
{
    display_base = GA_SCREEN;
    g_screen_width = width;
    g_screen_height = height;
    screen_depth = depth_pref;
    while (screen_depth > 1 && !fits(screen_depth, width, height))
        screen_depth /= 2;
    screen_row_bytes = row_bytes_for(screen_depth, width);
    /* White: 0 in the 1- and 8-bit tables, all ones in direct colour. */
    gmemset(GA_SCREEN, screen_depth >= 16 ? 0xFF : 0, screen_row_bytes * (uint32_t)height);
    wr32(LM_ScrnBase, GA_SCREEN);
    wr16(LM_ScrVRes, 72);
    wr16(LM_ScrHRes, 72);

    /* Main device: PixMap of the screen with its colour table. */
    gaddr pmh = tb_NewHandle(PixMap_SIZEOF, true, true);
    gaddr pm = rd32(pmh);
    PixMap_set_baseAddr(pm, GA_SCREEN);
    qrect_put(pm + PixMap_bounds_off, (qrect){ 0, 0, (int16_t)height, (int16_t)width });
    PixMap_set_hRes(pm, 72 << 16);
    PixMap_set_vRes(pm, 72 << 16);

    gaddr gdh = tb_NewHandle(GDevice_SIZEOF, true, true);
    gaddr gd = rd32(gdh);
    /* screenActive, screenDevice, allInit, mainScreen, ramInit (it has a
     * driver: games look for one to switch modes through), gdDevType (a
     * colour device, not set to greys: Spheres) */
    GDevice_set_gdFlags(gd, (int16_t)((1 << 15) | (1 << 13) | (1 << 12) | (1 << 11) | (1 << 10) | 1));
    GDevice_set_gdPMap(gd, pmh);
    GDevice_set_gdRefNum(gd, (int16_t)(-1 - VIDEO_UNIT));
    qrect_put(gd + GDevice_gdRect_off, (qrect){ 0, 0, (int16_t)height, (int16_t)width });
    GDevice_set_gdResPref(gd, 3);
    describe_screen(gd, pm);

    wr32(LM_MainDevice, gdh);
    wr32(LM_DeviceList, gdh);
    wr32(LM_TheGDevice, gdh);
    qd_device_itable(gdh);
    nports = 0;
    qd_gworlds_reset();
    qd_port_pats_reset();
    qd_palettes_reset();
}

/* ---- Changing depth ---- */

/* Ports opened on the screen, so SetDepth can bring them up to date (the
 * ROM kept a PortList for the same purpose). */
#define MAX_PORTS 512
static gaddr ports[MAX_PORTS];

void qd_screen_port_opened(gaddr port)
{
    for (unsigned i = 0; i < nports; i++)
        if (ports[i] == port)
            return;
    if (nports < MAX_PORTS)
        ports[nports++] = port;
}

void qd_screen_port_closed(gaddr port)
{
    for (unsigned i = 0; i < nports; i++)
        if (ports[i] == port) {
            ports[i] = ports[--nports];
            return;
        }
}

bool qd_screen_supports(int depth)
{
    return (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16 || depth == 32) &&
           fits(depth, g_screen_width, g_screen_height);
}

/* Switch the screen to another depth: the device, screenBits, and every
 * port still drawing on the screen change with it; the contents are lost,
 * so everything is redrawn. */
bool qd_screen_set_depth(int depth)
{
    if (!qd_screen_supports(depth))
        return false;
    if (depth == screen_depth)
        return true;
    uint32_t old_rb = screen_row_bytes;
    screen_depth = depth;
    screen_row_bytes = row_bytes_for(depth, g_screen_width);
    gmemset(GA_SCREEN, depth >= 16 ? 0xFF : 0, screen_row_bytes * (uint32_t)g_screen_height);
    gaddr gd = rd32(rd32(LM_MainDevice));
    gaddr pm = rd32(GDevice_gdPMap(gd));
    describe_screen(gd, pm);
    qd_device_itable(rd32(LM_MainDevice));
    gaddr sb = qd_screen_bits();
    if (sb && BitMap_baseAddr(sb) == GA_SCREEN)
        BitMap_set_rowBytes(sb, (int16_t)screen_row_bytes);
    for (unsigned i = 0; i < nports; i++) {
        gaddr p = ports[i];
        if (qd_port_is_color(p)) {
            gaddr h = rd32(p + CGrafPort_portPixMap_off);
            gaddr ppm = h ? rd32(h) : 0;
            if (!ppm || PixMap_baseAddr(ppm) != GA_SCREEN)
                continue;
            PixMap_set_rowBytes(ppm, PixMap_rowBytes(pm));
            PixMap_set_pixelSize(ppm, PixMap_pixelSize(pm));
            PixMap_set_pixelType(ppm, PixMap_pixelType(pm));
            PixMap_set_cmpCount(ppm, PixMap_cmpCount(pm));
            PixMap_set_cmpSize(ppm, PixMap_cmpSize(pm));
            PixMap_set_pmTable(ppm, PixMap_pmTable(pm));
        } else {
            gaddr bm = p + GrafPort_portBits_off;
            if (BitMap_baseAddr(bm) == GA_SCREEN && ((uint16_t)BitMap_rowBytes(bm) & 0x3FFF) == old_rb)
                BitMap_set_rowBytes(bm, (int16_t)screen_row_bytes);
        }
    }
    wm_repaint_all();
    tb_DrawMenuBar();
    return true;
}

/* ---- Changing size ----
 *
 * The screen made another size while a program runs, as a Mac changed
 * resolution (a resumed program on a display, or at a scale, other than
 * the one it hibernated on): the device, screenBits, ScreenRow and every
 * port drawing on the screen take the new size, what it shows stays
 * where it was, and the Window Manager paints what is new
 * (wm_screen_resized).  A port's bounds
 * keep their top left (the port's place on the screen). */
static void resize_bounds(gaddr r, int width, int height)
{
    qrect b = qrect_get(r);
    b.bottom = (int16_t)(b.top + height);
    b.right = (int16_t)(b.left + width);
    qrect_put(r, b);
}

/* Whether the screen fits its space at another size, at its depth. */
bool qd_screen_size_fits(int width, int height) { return fits(screen_depth, width, height); }

bool qd_screen_set_size(int width, int height)
{
    if (width == g_screen_width && height == g_screen_height)
        return true;
    if (width < 512 || height < 342 || width > 4096 || height > 4096 || !fits(screen_depth, width, height))
        return false;
    uint32_t old_rb = screen_row_bytes;
    int old_w = g_screen_width, old_h = g_screen_height;
    /* What the screen shows stays where it was, from the top left: a game
     * that painted its backdrop once and draws only what moves, at places
     * it worked out for the old screen, goes on matching it.  What is new
     * is white until the Window Manager paints it. */
    uint32_t keep_rows = (uint32_t)(old_h < height ? old_h : height);
    uint32_t keep_bytes = row_bytes_for(screen_depth, old_w < width ? old_w : width);
    uint8_t *old = bb_malloc((size_t)old_rb * keep_rows, BB_BYTES);
    if (old)
        memcpy(old, gptr(GA_SCREEN), (size_t)old_rb * keep_rows);
    g_screen_width = width;
    g_screen_height = height;
    screen_row_bytes = row_bytes_for(screen_depth, width);
    gmemset(GA_SCREEN, screen_depth >= 16 ? 0xFF : 0, screen_row_bytes * (uint32_t)height);
    if (old) {
        for (uint32_t y = 0; y < keep_rows; y++)
            memcpy(gptr(GA_SCREEN + y * screen_row_bytes), old + (size_t)y * old_rb, keep_bytes);
        mem_wrote(GA_SCREEN, screen_row_bytes * keep_rows);
        bb_free(old);
    }
    gaddr gd = rd32(rd32(LM_MainDevice));
    gaddr pm = rd32(GDevice_gdPMap(gd));
    qrect screen = { 0, 0, (int16_t)height, (int16_t)width };
    qrect_put(gd + GDevice_gdRect_off, screen);
    qrect_put(pm + PixMap_bounds_off, screen);
    PixMap_set_rowBytes(pm, (int16_t)(screen_row_bytes | 0x8000));
    wr16(LM_ScreenRow, (uint16_t)screen_row_bytes);
    gaddr sb = qd_screen_bits();
    if (sb && BitMap_baseAddr(sb) == GA_SCREEN) {
        BitMap_set_rowBytes(sb, (int16_t)screen_row_bytes);
        qrect_put(sb + BitMap_bounds_off, screen);
    }
    for (unsigned i = 0; i < nports; i++) {
        gaddr p = ports[i];
        if (qd_port_is_color(p)) {
            gaddr h = rd32(p + CGrafPort_portPixMap_off);
            gaddr ppm = h ? rd32(h) : 0;
            if (!ppm || PixMap_baseAddr(ppm) != GA_SCREEN)
                continue;
            PixMap_set_rowBytes(ppm, PixMap_rowBytes(pm));
            resize_bounds(ppm + PixMap_bounds_off, width, height);
        } else {
            gaddr bm = p + GrafPort_portBits_off;
            if (BitMap_baseAddr(bm) != GA_SCREEN || ((uint16_t)BitMap_rowBytes(bm) & 0x3FFF) != old_rb)
                continue;
            BitMap_set_rowBytes(bm, (int16_t)screen_row_bytes);
            resize_bounds(bm + BitMap_bounds_off, width, height);
        }
    }
    video_mode_changed();
    return true;
}

/* What a larger screen has beyond an old one of old_w x old_h, below row
 * top, made from the old screen's edge repeated: a game's backdrop (a
 * texture, black) goes on to the new edges.  The old screen's last 16
 * pixels are made again too (its rounded corners).  Widths are whole
 * numbers of 16 pixels, so every depth's pixels are whole bytes. */
void qd_screen_extend(int old_w, int old_h, int top)
{
    int x0 = old_w - 16, y0 = old_h - 16;
    if (x0 < 16 || y0 <= top + 16)
        return;
    uint32_t bpp = (uint32_t)screen_depth, rb = screen_row_bytes;
    if (g_screen_width > old_w) {
        int band = x0 < 128 ? x0 : 128; /* the edge repeated, this wide */
        for (int y = top; y < (old_h < g_screen_height ? old_h : g_screen_height); y++) {
            uint8_t *row = gptr(GA_SCREEN + (uint32_t)y * rb);
            for (int x = x0; x < g_screen_width; x += band) {
                int n = g_screen_width - x < band ? g_screen_width - x : band;
                memcpy(row + (uint32_t)x * bpp / 8, row + (uint32_t)(x0 - band) * bpp / 8, (size_t)n * bpp / 8);
            }
        }
    }
    if (g_screen_height > old_h) {
        int band = y0 - top < 64 ? y0 - top : 64;
        for (int y = y0; y < g_screen_height; y++)
            memcpy(gptr(GA_SCREEN + (uint32_t)y * rb), gptr(GA_SCREEN + (uint32_t)(y0 - band + (y - y0) % band) * rb), rb);
    }
    mem_wrote(GA_SCREEN + (uint32_t)top * rb, (uint32_t)(g_screen_height - top) * rb);
}

gaddr tb_GetMainDevice(void) { return rd32(LM_MainDevice); }
gaddr tb_GetDeviceList(void) { return rd32(LM_DeviceList); }
gaddr tb_GetGDevice(void) { return rd32(LM_TheGDevice); }
void tb_SetGDevice(gaddr gdh) { wr32(LM_TheGDevice, gdh); }
int8_t tb_TestDeviceAttribute(gaddr gdh, int16_t attribute)
{
    return gdh && rd32(gdh) ? (((uint16_t)GDevice_gdFlags(rd32(gdh)) >> attribute) & 1) : 0;
}

gaddr tb_GetNextDevice(gaddr gdh) { return gdh ? GDevice_gdNextGD(rd32(gdh)) : 0; }

/* ---------------------------------------------------------------------- */
/* PNG snapshots (uncompressed deflate; no dependencies) */

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t k = i;
            for (int j = 0; j < 8; j++)
                k = (k & 1) ? 0xEDB88320u ^ (k >> 1) : k >> 1;
            crc_table[i] = k;
        }
    }
    c ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void put32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static bool chunk(FILE *fp, const char *type, const uint8_t *data, size_t len)
{
    uint8_t hdr[8];
    put32be(hdr, (uint32_t)len);
    memcpy(hdr + 4, type, 4);
    uint32_t crc = crc32_update(0, hdr + 4, 4);
    crc = crc32_update(crc, data, len);
    uint8_t c[4];
    put32be(c, crc);
    return fwrite(hdr, 1, 8, fp) == 8 && fwrite(data, 1, len, fp) == len && fwrite(c, 1, 4, fp) == 4;
}

void machine_screen_size(int *width, int *height)
{
    *width = g_screen_width;
    *height = g_screen_height;
}

/* A rectangle of the screen as RGBA (out holds w*h*4 bytes). */
/* The page the video circuitry shows (the compact Macs' alternate screen
 * is a second buffer below the main one): the host sees that, and all of
 * it is new. */
gaddr qd_screen_display_base(void) { return display_base; }

void qd_screen_set_display_base(gaddr base)
{
    if (base == display_base)
        return;
    present_memory_changing(NULL, 0); /* the GPU's view of the old page goes */
    display_base = base;
    if (g_watch_fn && g_watch_len) {
        g_watch_fn(base, 1);
        g_watch_fn(base + screen_row_bytes * (uint32_t)g_screen_height - 1, 1);
    }
}

/* The screen dimmed (After Dark's brightness: 0 black to 255 as it is),
 * as a monitor's gamma dimmed it: the pixels are unchanged. */
static int brightness = 255;
void qd_screen_set_brightness(int level) { brightness = level < 0 ? 0 : level > 255 ? 255 : level; }

static void dim(uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++, out += 4)
        for (int k = 0; k < 3; k++)
            out[k] = (uint8_t)(out[k] * brightness / 255);
}

void machine_screen_rgba(int x, int y, int w, int h, uint8_t *out)
{
    pixdst d = { display_base, screen_row_bytes, { 0, 0, (int16_t)g_screen_height, (int16_t)g_screen_width },
                 screen_depth, qd_screen_ctab() };
    bool inside = x >= 0 && y >= 0 && x + w <= g_screen_width && y + h <= g_screen_height;
    for (int r = 0; r < h; r++) {
        uint8_t *o = out + (size_t)r * (size_t)w * 4;
        int sy = y + r;
        /* The usual depths a row at a time (a host refreshes the whole
         * screen when it changes). */
        if (inside && screen_depth == 32) {
            const uint8_t *p = gptr(display_base + (uint32_t)sy * screen_row_bytes + (uint32_t)x * 4);
            for (int c = 0; c < w; c++, o += 4, p += 4)
                o[0] = p[1], o[1] = p[2], o[2] = p[3], o[3] = 255;
            continue;
        }
        if (inside && screen_depth == 8) {
            const uint8_t *p = gptr(display_base + (uint32_t)sy * screen_row_bytes + (uint32_t)x);
            for (int c = 0; c < w; c++, o += 4, p++)
                o[0] = clut[*p][0], o[1] = clut[*p][1], o[2] = clut[*p][2], o[3] = 255;
            continue;
        }
        for (int c = 0; c < w; c++, o += 4) {
            int sx = x + c;
            if (sx < 0 || sy < 0 || sx >= g_screen_width || sy >= g_screen_height) {
                o[0] = o[1] = o[2] = 255;
                o[3] = 255;
                continue;
            }
            uint32_t v = get_px(&d, sx, sy);
            if (screen_depth == 32) {
                o[0] = (uint8_t)(v >> 16);
                o[1] = (uint8_t)(v >> 8);
                o[2] = (uint8_t)v;
            } else if (screen_depth == 16) { /* x1555 */
                uint8_t r5 = (v >> 10) & 31, g5 = (v >> 5) & 31, b5 = v & 31;
                o[0] = (uint8_t)(r5 << 3 | r5 >> 2);
                o[1] = (uint8_t)(g5 << 3 | g5 >> 2);
                o[2] = (uint8_t)(b5 << 3 | b5 >> 2);
            } else if (screen_depth > 1) {
                o[0] = clut[v][0];
                o[1] = clut[v][1];
                o[2] = clut[v][2];
            } else {
                o[0] = o[1] = o[2] = v ? 0 : 255;
            }
            o[3] = 255;
        }
    }
    const uint8_t (*lut)[256] = video_output_lut();
    if (lut) {
        uint8_t *o = out;
        for (size_t i = 0, n = (size_t)w * (size_t)h; i < n; i++, o += 4)
            for (int k = 0; k < 3; k++)
                o[k] = lut[k][o[k]];
    }
    if (brightness != 255)
        dim(out, (size_t)w * (size_t)h);
}

/* A copy of what part of the screen shows, kept by a host that refreshes
 * its windows every frame: the pixels as they are in memory (the rect's
 * rows, whole bytes), and the state they are shown through. */
typedef struct {
    int32_t x, y, w, h, depth, brightness;
    uint32_t base, row_bytes;
    uint8_t clut[256][3], lut[3][256];
} shadow_head;
/* After it, not compared: the rows written this host has seen (dirty.c),
 * and its looks since it last compared everything. */
typedef struct {
    uint64_t seen;
    int32_t since_full, pad;
} shadow_tail;
/* Everything compared once in this many looks (a host's 60 a second):
 * the safety net under the tracking of writes. */
enum { SHADOW_FULL_EVERY = 60 };

enum { MAX_SCAN_BANDS = 64 };

static bool clip_rect(int *x0, int *y0, int *x1, int *y1, uint32_t *b0, uint32_t *b1)
{
    if (*x0 < 0)
        *x0 = 0;
    if (*y0 < 0)
        *y0 = 0;
    if (*x1 > g_screen_width)
        *x1 = g_screen_width;
    if (*y1 > g_screen_height)
        *y1 = g_screen_height;
    if (*x1 <= *x0 || *y1 <= *y0)
        return false;
    *b0 = (uint32_t)*x0 * (uint32_t)screen_depth / 8;
    *b1 = ((uint32_t)*x1 * (uint32_t)screen_depth + 7) / 8;
    return true;
}

size_t machine_screen_shadow_size(int x, int y, int w, int h)
{
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    uint32_t b0, b1;
    size_t rows = clip_rect(&x0, &y0, &x1, &y1, &b0, &b1) ? (size_t)(y1 - y0) * (b1 - b0) : 0;
    return sizeof(shadow_head) + sizeof(shadow_tail) + rows;
}


/* The rows [y0, y1) of the shown page whose bytes [b0, b1) differ from the
 * copy at o (n = b1 - b0 bytes a row, rows one after another), the copy
 * brought up to date, as bands. */
static int scan_rows(uint8_t *o, size_t stride, int y0, int y1, uint32_t b0, uint32_t n, int32_t *bands, int max)
{
    int nb = 0, run = -1;
    for (int r = y0; r < y1; r++, o += stride) {
        const uint8_t *p = gptr(display_base + (uint32_t)r * screen_row_bytes + b0);
        if (memcmp(o, p, n)) {
            memcpy(o, p, n);
            if (run < 0)
                run = r;
        } else if (run >= 0) {
            present_add_band(bands, &nb, max, run, r);
            run = -1;
        }
    }
    if (run >= 0)
        present_add_band(bands, &nb, max, run, y1);
    return nb;
}

/* scan_rows over only the rows in the bands in (n of them) within
 * [y0, y1); the copy at o holds row y0's bytes first. */
static int scan_bands(uint8_t *o, size_t stride, int y0_copy, int y0, int y1, uint32_t b0, uint32_t n, const int32_t *in,
                      int nin, int32_t *bands, int max)
{
    int nb = 0;
    for (int i = 0; i < nin; i++) {
        int a = in[i * 2] > y0 ? in[i * 2] : y0, b = in[i * 2 + 1] < y1 ? in[i * 2 + 1] : y1;
        if (b <= a)
            continue;
        int32_t part[MAX_SCAN_BANDS * 2];
        int k = scan_rows(o + (size_t)(a - y0_copy) * stride, stride, a, b, b0, n, part, MAX_SCAN_BANDS);
        for (int j = 0; j < k; j++)
            present_add_band(bands, &nb, max, part[j * 2], part[j * 2 + 1]);
    }
    return nb;
}

/* Whether it changed since the copy was taken, the copy brought up to
 * date: memory compared is several times faster than the pixels
 * converted and hashed, and a screen as large as the host's display
 * unchanged costs a fraction of a millisecond a frame. */
int machine_screen_changed_rows(int x, int y, int w, int h, uint8_t *shadow, size_t len, int32_t *bands, int max)
{
    if (max < 1)
        return 0;
    bool all = len != machine_screen_shadow_size(x, y, w, h);
    shadow_head head;
    memset(&head, 0, sizeof head); /* padding too: it is compared */
    head.x = x, head.y = y, head.w = w, head.h = h;
    head.depth = screen_depth, head.brightness = brightness;
    head.base = display_base, head.row_bytes = screen_row_bytes;
    if (screen_depth > 1 && screen_depth <= 8)
        memcpy(head.clut, clut, sizeof clut);
    const uint8_t (*lut)[256] = video_output_lut();
    if (lut)
        memcpy(head.lut, lut, sizeof head.lut);
    if (!all && memcmp(shadow, &head, sizeof head) != 0) {
        memcpy(shadow, &head, sizeof head);
        all = true;
    }
    /* Only the rows written since the last look are compared (dirty.c);
     * everything now and then, and always when writes aren't tracked. */
    shadow_tail *tail = (shadow_tail *)(void *)(shadow + sizeof head);
    uint8_t *copy = shadow + sizeof head + sizeof *tail;
    bool sized = len == machine_screen_shadow_size(x, y, w, h);
    int32_t marked[MAX_SCAN_BANDS * 2];
    int nm = sized ? present_dirty_rows(&tail->seen, y, y + h, marked, MAX_SCAN_BANDS) : PRESENT_DIRTY_ALL;
    if (nm == PRESENT_DIRTY_ALL)
        all = true;
    bool full = nm == PRESENT_DIRTY_UNKNOWN || (sized && ++tail->since_full >= SHADOW_FULL_EVERY);
    if (full && sized)
        tail->since_full = 0;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    uint32_t b0, b1;
    int n = 0;
    if (!all && clip_rect(&x0, &y0, &x1, &y1, &b0, &b1))
        n = full ? scan_rows(copy, b1 - b0, y0, y1, b0, b1 - b0, bands, max)
                 : scan_bands(copy, b1 - b0, y0, y0, y1, b0, b1 - b0, marked, nm, bands, max);
    else if (all && sized && clip_rect(&x0, &y0, &x1, &y1, &b0, &b1))
        scan_rows(copy, b1 - b0, y0, y1, b0, b1 - b0, bands, max); /* the copy taken */
    if (all) { /* everything, the parts off the screen too */
        bands[0] = y;
        bands[1] = y + h;
        return 1;
    }
    return n;
}

bool machine_screen_changed(int x, int y, int w, int h, uint8_t *shadow, size_t len)
{
    int32_t bands[2];
    return machine_screen_changed_rows(x, y, w, h, shadow, len, bands, 1) > 0;
}

/* The rect's rows in bands converted (out is the whole rect's, w*h*4),
 * clear outside the shape (w*h bytes, 0 outside; NULL for none) and, with
 * desktop, where the desktop shows (machine_desktop_mask). */
void machine_screen_rgba_bands(int x, int y, int w, int h, const int32_t *bands, int n, const uint8_t *shape,
                               bool desktop, uint8_t *out)
{
    BB_HOST static uint8_t *desk;
    BB_HOST static size_t desk_cap;
    for (int i = 0; i < n; i++) {
        int r0 = bands[i * 2] > y ? bands[i * 2] : y, r1 = bands[i * 2 + 1] < y + h ? bands[i * 2 + 1] : y + h;
        if (r1 <= r0)
            continue;
        size_t first = (size_t)(r0 - y) * (size_t)w, count = (size_t)(r1 - r0) * (size_t)w;
        uint32_t *px = (uint32_t *)(void *)(out + first * 4);
        machine_screen_rgba(x, r0, w, r1 - r0, (uint8_t *)px);
        if (shape)
            for (size_t k = 0; k < count; k++)
                if (!shape[first + k])
                    px[k] = 0;
        if (desktop) {
            if (desk_cap < count) {
                bb_host_free(desk);
                desk = bb_host_malloc(count);
                desk_cap = desk ? count : 0;
            }
            if (desk && machine_desktop_mask(x, r0, w, r1 - r0, desk))
                for (size_t k = 0; k < count; k++)
                    if (!desk[k])
                        px[k] = 0;
        }
    }
}

/* The screen as a presenter shows it (present.h). */
void present_screen_state(present_state *s)
{
    memset(s, 0, sizeof *s);
    s->width = g_screen_width;
    s->height = g_screen_height;
    s->depth = screen_depth;
    s->brightness = brightness;
    s->row_bytes = screen_row_bytes;
    s->base = display_base;
    memcpy(s->clut, clut, sizeof clut);
    const uint8_t (*lut)[256] = video_output_lut();
    s->lut_on = lut != NULL;
    if (lut)
        memcpy(s->lut, lut, sizeof s->lut);
}

/* The shown page's rows [0, height) compared with the copy at rows
 * (row_bytes a row) and brought up to date, as bands. */
int present_scan_rows(const present_state *s, uint8_t *rows, int32_t *bands, int max)
{
    if (s->base != display_base || s->row_bytes != screen_row_bytes || s->height > g_screen_height || max < 1)
        return -1;
    return scan_rows(rows, s->row_bytes, 0, s->height, 0, s->row_bytes, bands, max);
}

/* ... only the rows in the bands in (n of them, as dirty.c marked them). */
int present_scan_bands(const present_state *s, uint8_t *rows, const int32_t *in, int n, int32_t *bands, int max)
{
    if (s->base != display_base || s->row_bytes != screen_row_bytes || s->height > g_screen_height || max < 1)
        return -1;
    return scan_bands(rows, s->row_bytes, 0, 0, s->height, 0, s->row_bytes, in, n, bands, max);
}

bool machine_screenshot(const char *path)
{
    int w = g_screen_width, h = g_screen_height;
    uint8_t *rgba = bb_malloc((size_t)w * (size_t)h * 4, BB_BYTES);
    if (!rgba)
        return false;
    machine_screen_rgba(0, 0, w, h, rgba);
    bool ok = machine_write_png(path, rgba, w, h);
    bb_free(rgba);
    return ok;
}

/* A small picture of the screen, for a hibernated snapshot's picker: at
 * most max_w pixels wide, its height in proportion, each pixel the average
 * of the screen's under it (a dithered grey stays grey), as an RGB PNG in
 * memory (bb_host_free it).  Deflated, as a thumbnail rides in every snapshot's
 * launch record. */
bool machine_screen_thumbnail(int max_w, uint8_t **png, size_t *len)
{
    *png = NULL;
    *len = 0;
    int w = g_screen_width, h = g_screen_height;
    if (w <= 0 || h <= 0 || max_w <= 0)
        return false;
    int tw = w < max_w ? w : max_w;
    int th = (int)(((int64_t)h * tw + w / 2) / w);
    if (th < 1)
        th = 1;
    uint8_t *rgba = bb_host_malloc((size_t)w * (size_t)h * 4);
    size_t raw_row = (size_t)tw * 3 + 1;
    uint8_t *raw = bb_host_malloc(raw_row * (size_t)th);
    uLongf zlen = compressBound((uLong)(raw_row * (size_t)th));
    uint8_t *z = bb_host_malloc(zlen);
    bool ok = rgba && raw && z;
    if (ok) {
        machine_screen_rgba(0, 0, w, h, rgba);
        for (int ty = 0; ty < th; ty++) {
            int y0 = (int)((int64_t)ty * h / th), y1 = (int)((int64_t)(ty + 1) * h / th);
            if (y1 <= y0)
                y1 = y0 + 1;
            uint8_t *o = raw + (size_t)ty * raw_row;
            *o++ = 0; /* no filter */
            for (int tx = 0; tx < tw; tx++) {
                int x0 = (int)((int64_t)tx * w / tw), x1 = (int)((int64_t)(tx + 1) * w / tw);
                if (x1 <= x0)
                    x1 = x0 + 1;
                uint32_t sum[3] = { 0, 0, 0 }, n = 0;
                for (int y = y0; y < y1; y++)
                    for (int x = x0; x < x1; x++, n++) {
                        const uint8_t *p = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;
                        sum[0] += p[0];
                        sum[1] += p[1];
                        sum[2] += p[2];
                    }
                for (int k = 0; k < 3; k++)
                    *o++ = (uint8_t)((sum[k] + n / 2) / n);
            }
        }
        ok = compress2(z, &zlen, raw, (uLong)(raw_row * (size_t)th), 9) == Z_OK;
    }
    char *buf = NULL;
    size_t blen = 0;
    FILE *fp = ok ? open_memstream(&buf, &blen) : NULL;
    if (fp) {
        static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
        uint8_t ihdr[13] = { 0 };
        put32be(ihdr, (uint32_t)tw);
        put32be(ihdr + 4, (uint32_t)th);
        ihdr[8] = 8; /* bit depth */
        ihdr[9] = 2; /* RGB */
        ok = fwrite(sig, 1, 8, fp) == 8 && chunk(fp, "IHDR", ihdr, 13) && chunk(fp, "IDAT", z, zlen) &&
             chunk(fp, "IEND", NULL, 0);
        ok = fclose(fp) == 0 && ok;
    } else {
        ok = false;
    }
    bb_host_free(rgba);
    bb_host_free(raw);
    bb_host_free(z);
    if (!ok) {
        bb_host_free(buf); /* open_memstream's: plain malloc */
        return false;
    }
    *png = (uint8_t *)buf;
    *len = blen;
    return true;
}

/* An RGBA image as an uncompressed PNG: RGB, or RGBA keeping the alpha. */
static bool write_png(const char *path, const uint8_t *rgba, int w, int h, bool alpha)
{
    size_t bpp = alpha ? 4 : 3;
    size_t raw_row = (size_t)w * bpp + 1; /* filter byte + pixels */
    size_t raw_len = raw_row * (size_t)h;
    uint8_t *raw = bb_malloc(raw_len, BB_BYTES);
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * raw_row;
        row[0] = 0;
        for (int x = 0; x < w; x++)
            memcpy(row + 1 + (size_t)x * bpp, rgba + ((size_t)y * (size_t)w + (size_t)x) * 4, bpp);
    }
    /* zlib stream of stored blocks. */
    size_t nblocks = raw_len / 65535 + 1;
    size_t zlen = 2 + raw_len + nblocks * 5 + 4;
    uint8_t *z = bb_malloc(zlen, BB_BYTES);
    size_t o = 0;
    z[o++] = 0x78;
    z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t off = 0; off < raw_len || off == 0;) {
        size_t n = raw_len - off > 65535 ? 65535 : raw_len - off;
        z[o++] = off + n >= raw_len ? 1 : 0;
        z[o++] = (uint8_t)n;
        z[o++] = (uint8_t)(n >> 8);
        z[o++] = (uint8_t)~n;
        z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + off, n);
        o += n;
        off += n;
        if (n == 0)
            break;
    }
    put32be(z + o, (b << 16) | a);
    o += 4;

    FILE *fp = fopen(path, "wb");
    bool ok = fp != NULL;
    if (ok) {
        static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
        uint8_t ihdr[13];
        put32be(ihdr, (uint32_t)w);
        put32be(ihdr + 4, (uint32_t)h);
        ihdr[8] = 8;  /* bit depth */
        ihdr[9] = alpha ? 6 : 2; /* RGBA or RGB */
        ihdr[10] = 0;
        ihdr[11] = 0;
        ihdr[12] = 0;
        ok = fwrite(sig, 1, 8, fp) == 8 && chunk(fp, "IHDR", ihdr, 13) && chunk(fp, "IDAT", z, o) &&
             chunk(fp, "IEND", NULL, 0);
        ok = (fclose(fp) == 0) && ok;
    }
    bb_free(z);
    bb_free(raw);
    return ok;
}

bool machine_write_png(const char *path, const uint8_t *rgba, int w, int h) { return write_png(path, rgba, w, h, false); }
bool machine_write_png_alpha(const char *path, const uint8_t *rgba, int w, int h)
{
    return write_png(path, rgba, w, h, true);
}
