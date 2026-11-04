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

/* A metric of the theme's 'tinf' ('mbht', 'lvth'...). */
bool thme_metric(const char *tag, int *out);

/* The theme's brushes (kThemeBrush 1-47) and text colours (1-46). */
bool thme_brush_rgb(int brush, uint16_t out[3]);
bool thme_text_rgb(int color, uint16_t out[3]);

/* A part ('layo' id) for a rectangle, state bits and measures (m[1..]). */
bool thme_has_part(int id);
/* A 'frme' round (or, for rings below 0, outside) a rectangle. */
bool thme_frame(int id, qrect r);
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
bool thme_default_ring(qrect r, uint32_t state);
bool thme_check_box(qrect box, bool radio, int value, uint32_t state);
bool thme_scroll_bar(qrect r, bool vertical, int hilite, bool enabled, bool active, qrect thumb);
bool thme_bevel(qrect r, int size, uint32_t state, bool on);
bool thme_disclosure(qrect r, int value, uint32_t state, bool left);
bool thme_little_arrows(qrect r, uint32_t state);
bool thme_popup(qrect r, uint32_t state, bool arrow_only);
bool thme_popup_arrow(qrect r, int orient, int size, uint32_t state);
bool thme_focus_rect(qrect r);
bool thme_tab(qrect r, int style, bool small, int dir);
bool thme_tab_pane(qrect r, uint32_t state);
bool thme_tab_pane_inside(qrect pane, qrect clip, bool active);

/* A ThemeTrackDrawInfo's slider, progress or indeterminate bar (kind 2-4),
 * with the thumb where the Appearance code puts it. */
typedef struct {
    int kind;
    qrect bounds, thumb;
    bool horizontal;
    int32_t min, max, value;
    int enable, press, thumb_dir, phase;
} thme_track_info;
bool thme_track(const thme_track_info *t);
bool thme_tick_mark(bool horizontal, bool active, int x, int y);

/* Menus: the bar, a title's background, a menu's (0 pull-down, 1 pop-up, 2
 * hierarchical), an item's, a divider. */
bool thme_menu_bar(qrect r);
bool thme_menu_title(qrect bar, qrect r, bool selected, bool enabled);
bool thme_menu_background(qrect r, int type);
bool thme_menu_extent(qrect r, int type, qrect *out);
bool thme_menu_item(qrect r, bool selected, bool enabled, bool hier);
bool thme_menu_separator(qrect r);
/* The content's insets in a menu item: top, left, bottom, right. */
bool thme_menu_item_insets(int out[4]);

/* The scene: a string item ('name', 'snds' the sound set, 'patn', fonts),
 * the highlight colour, the desktop pattern painted over an area. */
bool thme_scene_string(const char *tag, char *out, size_t n);
bool thme_scene_int(const char *tag, int *out);
bool thme_scene_bool(const char *tag, bool *out);
/* A theme file draws the screen (loaded, and the appearance chosen). */
bool thme_in_use(void);
/* A scene font ('lgsf', 'smsf', 'vfnt') that is installed: its family and
 * name; else -1.  The system font's family (Charcoal when the theme's
 * isn't installed). */
int thme_scene_font(const char *tag, char *out, size_t n);
int thme_system_family(void);
bool thme_highlight_rgb(uint16_t out[3]);
bool thme_paint_desk(const hrgn *area);
/* The sound set: its 'snd ' for a ThemeSoundKind or ThemeDragSoundKind
 * (an OSType), and the kind's class (the sound mask's bit). */
bool thme_load_sounds(const char *path, const uint8_t *fork, size_t len, char *err, size_t errlen);
void thme_unload_sounds(void);
const char *thme_sound_set(void);
uint32_t thme_sound_mask(void);
const uint8_t *thme_sound(uint32_t kind, int *cls, int16_t *id, uint32_t *len);
/* The theme's icons ('tic#'): the resource ID of its icon for a system
 * icon type, whether an ID is one of them, and its resources. */
bool thme_icon_id(uint32_t type, int16_t *id);
bool thme_icon_listed(int16_t id);
const uint8_t *thme_resource(uint32_t type, int16_t id, uint32_t *len);
/* A ThemeCursor's 'crsr' (the step'th of an animated one), or NULL. */
const uint8_t *thme_cursor(int cursor, int step, uint32_t *len);

#endif
