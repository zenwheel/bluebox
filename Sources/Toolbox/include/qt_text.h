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

/* The description's display flags, justification, background and text
 * box, as changed at run time. */
uint32_t qt_text_flags(const qt_text *t);
void qt_text_set_flags(qt_text *t, uint32_t flags);
int16_t qt_text_justification(const qt_text *t);
void qt_text_set_justification(qt_text *t, int16_t just);
void qt_text_background(const qt_text *t, uint16_t rgb[3]);
void qt_text_set_background(qt_text *t, const uint16_t rgb[3]);
void qt_text_box(const qt_text *t, int16_t r[4]);
void qt_text_set_box(qt_text *t, const int16_t r[4]);

#endif
