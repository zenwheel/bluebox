/*
 * The screen: a framebuffer at GA_SCREEN (1, 8, 16 or 32 bits deep), the main
 * GDevice describing it, and PNG snapshots for headless runs.  SetDepth can
 * change the depth while an application runs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "qd_internal.h"
#include "tb_internal.h"

int g_screen_width = 1024, g_screen_height = 768;
static uint32_t screen_row_bytes;
static int depth_pref = 32, screen_depth = 1;
static gaddr screen_ctab;
static unsigned nports; /* ports drawing on the screen, below */

void machine_set_screen_depth(int depth) { depth_pref = qd_screen_supports(depth) ? depth : 1; }
int qd_screen_depth(void) { return screen_depth; }
gaddr qd_screen_ctab(void) { return screen_depth == 1 ? 0 : screen_ctab; }

uint32_t qd_screen_row_bytes(void) { return screen_row_bytes; }

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
    return (uint32_t)((width * depth + 31) / 32 * 4); /* whole longs */
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
        screen_ctab = make_ctab(bw, 2, 1);  /* unused by direct pixels */
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
    GDevice_set_gdType(gd, screen_depth >= 16 ? 2 : 0); /* direct or CLUT device */
    wr16(LM_ScreenRow, (uint16_t)screen_row_bytes);
}

void qd_screen_init(int width, int height)
{
    g_screen_width = width;
    g_screen_height = height;
    screen_depth = depth_pref;
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
     * driver: games look for one to switch modes through) */
    GDevice_set_gdFlags(gd, (int16_t)((1 << 15) | (1 << 13) | (1 << 12) | (1 << 11) | (1 << 10)));
    GDevice_set_gdPMap(gd, pmh);
    qrect_put(gd + GDevice_gdRect_off, (qrect){ 0, 0, (int16_t)height, (int16_t)width });
    GDevice_set_gdResPref(gd, 3);
    GDevice_set_gdMode(gd, 0x80);
    describe_screen(gd, pm);

    wr32(LM_MainDevice, gdh);
    wr32(LM_DeviceList, gdh);
    wr32(LM_TheGDevice, gdh);
    nports = 0;
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
    return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16 || depth == 32;
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
void machine_screen_rgba(int x, int y, int w, int h, uint8_t *out)
{
    pixdst d = { GA_SCREEN, screen_row_bytes, { 0, 0, (int16_t)g_screen_height, (int16_t)g_screen_width },
                 screen_depth, qd_screen_ctab() };
    static uint8_t lut[256][3];
    static gaddr lut_for;
    static uint32_t lut_seed;
    static int lut_depth;
    if (screen_depth > 1 && screen_depth <= 8) {
        uint32_t seed = rd32(rd32(screen_ctab));
        if (lut_for != screen_ctab || lut_seed != seed || lut_depth != screen_depth) {
            lut_depth = screen_depth;
            for (int i = 0; i < (1 << screen_depth); i++) {
                uint16_t c[3];
                qd_px_rgb(screen_depth, screen_ctab, (uint32_t)i, c);
                lut[i][0] = (uint8_t)(c[0] >> 8);
                lut[i][1] = (uint8_t)(c[1] >> 8);
                lut[i][2] = (uint8_t)(c[2] >> 8);
            }
            lut_for = screen_ctab;
            lut_seed = seed;
        }
    }
    for (int r = 0; r < h; r++) {
        uint8_t *o = out + (size_t)r * (size_t)w * 4;
        int sy = y + r;
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
                o[0] = lut[v][0];
                o[1] = lut[v][1];
                o[2] = lut[v][2];
            } else {
                o[0] = o[1] = o[2] = v ? 0 : 255;
            }
            o[3] = 255;
        }
    }
}

bool machine_screenshot(const char *path)
{
    int w = g_screen_width, h = g_screen_height;
    size_t raw_row = (size_t)w * 3 + 1; /* filter byte + RGB */
    size_t raw_len = raw_row * (size_t)h;
    uint8_t *raw = malloc(raw_len);
    uint8_t *rgba = malloc((size_t)w * 4);
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * raw_row;
        row[0] = 0;
        machine_screen_rgba(0, y, w, 1, rgba);
        for (int x = 0; x < w; x++)
            memcpy(row + 1 + x * 3, rgba + x * 4, 3);
    }
    free(rgba);
    /* zlib stream of stored blocks. */
    size_t nblocks = raw_len / 65535 + 1;
    size_t zlen = 2 + raw_len + nblocks * 5 + 4;
    uint8_t *z = malloc(zlen);
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
        ihdr[9] = 2;  /* RGB */
        ihdr[10] = 0;
        ihdr[11] = 0;
        ihdr[12] = 0;
        ok = fwrite(sig, 1, 8, fp) == 8 && chunk(fp, "IHDR", ihdr, 13) && chunk(fp, "IDAT", z, o) &&
             chunk(fp, "IEND", NULL, 0);
        ok = (fclose(fp) == 0) && ok;
    }
    free(z);
    free(raw);
    return ok;
}
