/* QuickTime text tracks, drawn with the Toolbox's TextEdit (qt_text.c). */
#ifndef QT_TEXT_H
#define QT_TEXT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct qt_text qt_text;

/* A text track of w x h pixels from its sample description (the whole
 * entry, from its size field). */
qt_text *qt_text_new(const uint8_t *desc, uint32_t len, int w, int h);
void qt_text_free(qt_text *t);

/* A sample drawn into rgba (w * h * 4): the background colour, or alpha 0
 * where keyed text leaves what is behind it.  False if the track is not
 * displayed.  Draws with QuickDraw in guest memory: call from the Toolbox. */
bool qt_text_draw(qt_text *t, const uint8_t *sample, uint32_t len, uint8_t *rgba);

#endif
