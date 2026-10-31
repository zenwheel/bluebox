/*
 * Mac OS 8.5 Appearance theme files ('thme'): the reader and the layout
 * engine that draws a theme's parts (thme.c).
 */
#ifndef THME_H
#define THME_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "mem.h"
#include "qd_internal.h"

/* Measures a layout's kind-1 coordinates take (index 1..15). */
#define THME_MEASURES 16

/* Window regions a layout's records make (the low byte of their flags). */
enum {
    THME_TITLE_BAR = 0x01,
    THME_TITLE_TEXT = 0x02,
    THME_CLOSE = 0x04,
    THME_ZOOM = 0x08,
    THME_STRUCTURE = 0x20,
    THME_GROW = 0x40,
    THME_COLLAPSE = 0x80,
    THME_CONTENT = 0x200,
};

bool thme_fork_is_theme(const uint8_t *fork, size_t len);
bool thme_load(const char *path, const uint8_t *fork, size_t len, char *err, size_t errlen);
void thme_unload(void);
bool thme_on(void);
const char *thme_name(void);

/* The theme's brushes (kThemeBrush 1-47) and text colours (1-46). */
bool thme_brush_rgb(int brush, uint16_t out[3]);
bool thme_text_rgb(int color, uint16_t out[3]);

/* A part ('layo' id) for a rectangle, state bits and measures (m[1..]). */
bool thme_has_part(int id);
bool thme_draw_part(int id, qrect b, uint32_t state, const int *m, uint32_t pressed);
bool thme_part_rect(int id, qrect b, uint32_t state, const int *m, uint32_t which, qrect *out);
uint32_t thme_part_hit(int id, qrect b, uint32_t state, const int *m, uint32_t which, int h, int v);
bool thme_part_region(int id, qrect b, uint32_t state, const int *m, uint32_t also, hrgn *out);

/* Off-screen: into an RGBA buffer w x h. */
void thme_render_part(uint8_t *rgba, int w, int h, int id, qrect b, uint32_t state, const int *m, uint32_t pressed);
void thme_render_image(uint8_t *rgba, int w, int h, uint32_t type, int16_t id, int frame, int x, int y);
bool thme_image_size(uint32_t type, int16_t id, int *w, int *h, int *n);
void thme_render_sheet(uint8_t *rgba, int w, int h);
void thme_dump(FILE *f, bool coords);

/* Windows: the variants theme.c names (0x100 floating, 0x200 collapsed). */
bool thme_draw_window(gaddr w, int var, qrect c, bool hilited, bool goaway, bool zoom, uint32_t pressed);
bool thme_window_region(gaddr w, int var, qrect c, bool goaway, bool zoom, hrgn *out);
int thme_window_hit(gaddr w, int var, qrect c, bool goaway, bool zoom, int h, int v);
bool thme_draw_grow(qrect c, bool active);

/* Controls, as the Appearance Manager's ThemeDrawState (0 inactive, 1
 * active, 2 pressed) gives them. */
bool thme_push_button(qrect r, uint32_t state, bool is_default);
bool thme_check_box(qrect box, bool radio, int value, uint32_t state);
bool thme_scroll_bar(qrect r, bool vertical, int hilite, bool enabled, bool active, qrect thumb);

#endif
