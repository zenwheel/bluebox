/*
 * QuickDraw 3D's and RAVE's frames to and from guest pixels: a rendered
 * frame (host 0xAARRGGBB) into a port, a pixel map or a RAVE memory
 * device at whatever depth it has, and the pixel formats their textures
 * and pixmaps come in.
 */
#include <stdlib.h>

#include "q3_internal.h"
#include "tb_internal.h"

static uint32_t to_depth(const pixdst *d, uint32_t argb)
{
    switch (d->depth) {
    case 32: return argb & 0xFFFFFF;
    case 16: return (argb >> 9 & 0x7C00) | (argb >> 6 & 0x03E0) | (argb >> 3 & 0x001F);
    default: {
        uint16_t rgb[3] = { (uint16_t)((argb >> 16 & 0xFF) * 0x101), (uint16_t)((argb >> 8 & 0xFF) * 0x101),
                            (uint16_t)((argb & 0xFF) * 0x101) };
        return qd_rgb_px(d->depth, d->ctab, rgb);
    }
    }
}

void q3_put_pixels(const uint32_t *argb, int width, int height, const pixdst *d, int x, int y, const hrgn *clip)
{
    for (int row = 0; row < height; row++) {
        int ty = y + row;
        if (ty < d->bounds.top || ty >= d->bounds.bottom)
            continue;
        int n = 0;
        const int16_t *xs = NULL;
        if (clip) {
            xs = hrgn_row(clip, ty, &n);
            if (!xs)
                continue;
        }
        const uint32_t *src = argb + (size_t)row * (size_t)width;
        int x0 = x > d->bounds.left ? x : d->bounds.left;
        int x1 = x + width < d->bounds.right ? x + width : d->bounds.right;
        /* The clip's runs on this row, or the whole row. */
        for (int k = 0; k < (clip ? n : 2); k += 2) {
            int a = clip ? (xs[k] > x0 ? xs[k] : x0) : x0;
            int b = clip ? (xs[k + 1] < x1 ? xs[k + 1] : x1) : x1;
            if (d->depth == 32) {
                gaddr p = d->base + (uint32_t)(ty - d->bounds.top) * d->row_bytes + (uint32_t)(a - d->bounds.left) * 4;
                for (int tx = a; tx < b; tx++, p += 4)
                    wr32(p, src[tx - x] & 0xFFFFFF);
                continue;
            }
            for (int tx = a; tx < b; tx++)
                put_px(d, tx - d->bounds.left, ty - d->bounds.top, to_depth(d, src[tx - x]));
        }
    }
}

void q3_get_pixels(uint32_t *argb, int width, int height, const pixdst *d, int x, int y)
{
    for (int row = 0; row < height; row++) {
        int ty = y + row;
        uint32_t *out = argb + (size_t)row * (size_t)width;
        for (int col = 0; col < width; col++) {
            int tx = x + col;
            if (ty < d->bounds.top || ty >= d->bounds.bottom || tx < d->bounds.left || tx >= d->bounds.right) {
                out[col] = 0xFFFFFFFF;
                continue;
            }
            uint32_t v = get_px(d, tx - d->bounds.left, ty - d->bounds.top);
            if (d->depth == 32) {
                out[col] = 0xFF000000 | v;
            } else if (d->depth == 16) {
                uint32_t r = v >> 10 & 31, g = v >> 5 & 31, b = v & 31;
                out[col] = 0xFF000000 | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
            } else {
                uint16_t rgb[3];
                qd_px_rgb(d->depth, d->ctab, v, rgb);
                out[col] = 0xFF000000 | (uint32_t)(rgb[0] >> 8) << 16 | (uint32_t)(rgb[1] >> 8) << 8 | rgb[2] >> 8;
            }
        }
    }
}

void q3_put_port(const uint32_t *argb, int width, int height, gaddr port, int x, int y)
{
    if (!port)
        return;
    pixdst d = qd_port_pixels(port);
    hrgn clip;
    hrgn_init(&clip);
    qd_port_clip(port, &clip);
    q3_put_pixels(argb, width, height, &d, x, y, &clip);
    hrgn_free(&clip);
}

int q3_pixel_bytes(int format)
{
    switch (format) {
    case Q3PX_RGB32:
    case Q3PX_ARGB32: return 4;
    case Q3PX_RGB24: return 3;
    default: return 2;
    }
}

uint32_t q3_pixel_argb(int format, gaddr p)
{
    switch (format) {
    case Q3PX_RGB32: return 0xFF000000 | rd32(p);
    case Q3PX_ARGB32: return rd32(p);
    case Q3PX_RGB24: return 0xFF000000 | (uint32_t)rd8(p) << 16 | (uint32_t)rd8(p + 1) << 8 | rd8(p + 2);
    case Q3PX_RGB16_565: {
        uint32_t v = rd16(p), r = v >> 11 & 31, g = v >> 5 & 63, b = v & 31;
        return 0xFF000000 | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
    }
    case Q3PX_ARGB16_4444: {
        uint32_t v = rd16(p);
        return (v >> 12) * 0x11u << 24 | (v >> 8 & 15) * 0x11u << 16 | (v >> 4 & 15) * 0x11u << 8 | (v & 15) * 0x11u;
    }
    default: {
        uint32_t v = rd16(p), r = v >> 10 & 31, g = v >> 5 & 31, b = v & 31;
        uint32_t a = format == Q3PX_ARGB16 ? (v & 0x8000 ? 0xFF000000u : 0) : 0xFF000000u;
        return a | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
    }
    }
}

uint32_t *q3_image_argb(gaddr image, int width, int height, uint32_t row_bytes, int format, bool flip)
{
    if (width <= 0 || height <= 0 || !image)
        return NULL;
    uint32_t *px = malloc((size_t)width * (size_t)height * 4);
    if (!px)
        return NULL;
    int bytes = q3_pixel_bytes(format);
    for (int y = 0; y < height; y++) {
        gaddr row = image + (uint32_t)(flip ? height - 1 - y : y) * row_bytes;
        for (int x = 0; x < width; x++)
            px[(size_t)y * (size_t)width + (size_t)x] = q3_pixel_argb(format, row + (uint32_t)(x * bytes));
    }
    return px;
}
