/*
 * The screen: a 1-bit framebuffer at GA_SCREEN, the main GDevice describing
 * it, and PNG snapshots for headless runs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "qd_internal.h"
#include "tb_internal.h"

int g_screen_width = 1024, g_screen_height = 768;
static uint32_t screen_row_bytes;

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

void qd_screen_init(int width, int height)
{
    g_screen_width = width;
    g_screen_height = height;
    screen_row_bytes = (uint32_t)((width + 31) / 32 * 4);
    gmemset(GA_SCREEN, 0, screen_row_bytes * (uint32_t)height); /* white */
    wr32(LM_ScrnBase, GA_SCREEN);
    wr16(LM_ScreenRow, (uint16_t)screen_row_bytes);
    wr16(LM_ScrVRes, 72);
    wr16(LM_ScrHRes, 72);

    /* Main device: PixMap of the screen with a black-and-white CLUT. */
    static const uint16_t bw[2][3] = { { 0xFFFF, 0xFFFF, 0xFFFF }, { 0, 0, 0 } };
    gaddr pmh = tb_NewHandle(PixMap_SIZEOF, true, true);
    gaddr pm = rd32(pmh);
    PixMap_set_baseAddr(pm, GA_SCREEN);
    PixMap_set_rowBytes(pm, (int16_t)(screen_row_bytes | 0x8000));
    qrect_put(pm + PixMap_bounds_off, (qrect){ 0, 0, (int16_t)height, (int16_t)width });
    PixMap_set_hRes(pm, 72 << 16);
    PixMap_set_vRes(pm, 72 << 16);
    PixMap_set_pixelSize(pm, 1);
    PixMap_set_cmpCount(pm, 1);
    PixMap_set_cmpSize(pm, 1);
    PixMap_set_pmTable(pm, make_ctab(bw, 2, 1));

    gaddr gdh = tb_NewHandle(GDevice_SIZEOF, true, true);
    gaddr gd = rd32(gdh);
    GDevice_set_gdType(gd, 0); /* CLUT device */
    GDevice_set_gdFlags(gd, (int16_t)((1 << 15) | (1 << 14) | (1 << 13) | (1 << 12) | (1 << 11) | (1 << 10)));
    GDevice_set_gdPMap(gd, pmh);
    qrect_put(gd + GDevice_gdRect_off, (qrect){ 0, 0, (int16_t)height, (int16_t)width });
    GDevice_set_gdResPref(gd, 3);
    GDevice_set_gdMode(gd, 0x80);

    wr32(LM_MainDevice, gdh);
    wr32(LM_DeviceList, gdh);
    wr32(LM_TheGDevice, gdh);
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

const uint8_t *machine_framebuffer(int *width, int *height, int *row_bytes)
{
    *width = g_screen_width;
    *height = g_screen_height;
    *row_bytes = (int)screen_row_bytes;
    return gptr(GA_SCREEN);
}

bool machine_screenshot(const char *path)
{
    int w = g_screen_width, h = g_screen_height;
    size_t raw_row = (size_t)(w + 7) / 8 + 1; /* filter byte + 1-bit pixels */
    size_t raw_len = raw_row * (size_t)h;
    uint8_t *raw = malloc(raw_len);
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * raw_row;
        row[0] = 0;
        /* PNG grayscale 1-bit: 1 = white; QuickDraw: 1 = black. */
        for (size_t i = 0; i + 1 < raw_row; i++)
            row[1 + i] = (uint8_t)~rd8(GA_SCREEN + (uint32_t)y * screen_row_bytes + (uint32_t)i);
    }
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
        ihdr[8] = 1;  /* bit depth */
        ihdr[9] = 0;  /* grayscale */
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
