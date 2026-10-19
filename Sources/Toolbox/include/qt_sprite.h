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

#endif
