/*
 * Icon drawing shared by the Icon Utilities (icons.c) and Icon Services
 * (iconservices.c).
 */
#ifndef ICONS_INTERNAL_H
#define ICONS_INTERNAL_H

#include <stdint.h>

#include "qd_internal.h"

/* The largest icon: Icon Services' thumbnails, 128 pixels square. */
#define ICON_MAX 128

/* An icon as RGBA, w x h; alpha 0 is transparent. */
typedef struct {
    int w, h;
    uint8_t px[ICON_MAX * ICON_MAX * 4];
} icon_img;

/* Colour data `d` (depth 1, 4 or 8) plus a 1-bit mask (or none) into img. */
void icon_render(icon_img *img, int w, int h, int depth, const uint8_t *d, const uint8_t *mask);
/* Where an icon goes in a rectangle with an alignment; a small icon in a
 * smaller place may be cropped to what it draws first. */
qrect icon_place(icon_img *img, qrect r, int16_t align);
/* Draw it there in the current port, with a transform. */
void icon_plot(icon_img *img, qrect r, int16_t align, int16_t transform);

#endif
