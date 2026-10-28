/* QuickTime sprite tracks, drawn natively (qt_sprite.c). */
#ifndef QT_SPRITE_H
#define QT_SPRITE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct qt_sprite qt_sprite;

/* A sprite track of w x h pixels from its sample description (the whole
 * entry, from its size field; track properties may follow the header). */
qt_sprite *qt_sprite_new(const uint8_t *desc, uint32_t len, int w, int h);
void qt_sprite_free(qt_sprite *s);

/* A key frame sample: its images and sprites become the scene.  False if
 * the sample is not one (it has no shared image data). */
bool qt_sprite_key(qt_sprite *s, const uint8_t *data, uint32_t len);
/* Back to the key frame's sprite properties. */
void qt_sprite_reset(qt_sprite *s);
/* An override sample's property changes, on the current state. */
void qt_sprite_override(qt_sprite *s, const uint8_t *data, uint32_t len);
/* Whether overrides add up from the key frame (kKeyFrameAndAllOverrides)
 * rather than each applying to the key frame alone. */
bool qt_sprite_all_overrides(const qt_sprite *s);
/* The scene over the background colour into rgba (w * h * 4, opaque);
 * false if the track is invisible. */
bool qt_sprite_draw(const qt_sprite *s, uint8_t *rgba);
/* The sprites alone, over what rgba holds. */
void qt_sprite_draw_over(const qt_sprite *s, uint8_t *rgba);

/* At run time (the sprite media handler): sprites by index (1-based) and
 * ID; their properties as atoms hold them (big-endian: matrix 36 bytes,
 * visible, layer and image index 2, graphics mode 10); sprites added and
 * removed; images; hit testing in track coordinates. */
int qt_sprite_count(const qt_sprite *s);
uint32_t qt_sprite_id(const qt_sprite *s, int index);
int qt_sprite_index(const qt_sprite *s, uint32_t id);
bool qt_sprite_get(const qt_sprite *s, uint32_t id, uint32_t prop, uint8_t *out, uint32_t *len);
bool qt_sprite_set(qt_sprite *s, uint32_t id, uint32_t prop, const uint8_t *val, uint32_t len);
bool qt_sprite_add(qt_sprite *s, uint32_t id, uint32_t image, const double m[6], bool visible, int16_t layer,
                   int32_t mode, const uint16_t op[3]);
bool qt_sprite_remove(qt_sprite *s, uint32_t id);
int qt_sprite_image_count(const qt_sprite *s);
bool qt_sprite_image_info(const qt_sprite *s, int index, int *w, int *h, double *rx, double *ry);
bool qt_sprite_hits(const qt_sprite *s, uint32_t id, double x, double y, bool image_test, bool invisible_too);
uint32_t qt_sprite_hit(const qt_sprite *s, double x, double y, bool image_test, bool invisible_too);
bool qt_sprite_track_visible(const qt_sprite *s);
void qt_sprite_set_track_visible(qt_sprite *s, bool visible);
void qt_sprite_background(const qt_sprite *s, uint8_t rgb[3]);
void qt_sprite_set_background(qt_sprite *s, const uint8_t rgb[3]);

/* The Sprite Toolbox's worlds: an empty scene of w x h; an image (an
 * ImageDescription then its data) given or replaced; a sprite's bounds
 * (top, left, bottom, right). */
qt_sprite *qt_sprite_world(int w, int h);
bool qt_sprite_set_image(qt_sprite *s, uint32_t id, const uint8_t *desc_and_data, uint32_t len);
void qt_sprite_forget_image(qt_sprite *s, uint32_t id);
bool qt_sprite_bounds(const qt_sprite *s, uint32_t id, int r[4]);

#endif
