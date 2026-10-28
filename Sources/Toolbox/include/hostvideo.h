/*
 * Video and sound decoders of the host's, for the codecs Blue Box has none
 * of its own for (H.264, MPEG-4, H.263, Motion JPEG, MP3, AAC, ...).
 *
 * macOS: VideoToolbox, ImageIO and AudioToolbox (hostvideo_apple.m).
 * Elsewhere: hostvideo_none.c, which decodes nothing yet; a GStreamer
 * implementation fits the same calls (a pipeline per decoder, appsrc in,
 * an RGBA or S16 appsink out).
 *
 * Decoders take samples in decoding order, a key frame first, and give
 * the picture each sample makes (no reordering of B-frames: QuickTime 6
 * era movies that use them show frames in decoding order).
 */
#ifndef HOSTVIDEO_H
#define HOSTVIDEO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hostvideo_dec hostvideo_dec;

/* Whether the host decodes this codec at all (cheap: asked often). */
bool hostvideo_supported(uint32_t format);
/* A decoder for a sample description: the whole ImageDescription (big
 * endian, its extensions such as 'avcC' and 'esds' after the colour table
 * if any), width by height. */
hostvideo_dec *hostvideo_open(uint32_t format, int width, int height, const uint8_t *desc, size_t desc_len);
/* One sample into frame (width * height RGBA, alpha 255). */
bool hostvideo_decode(hostvideo_dec *d, const uint8_t *data, size_t len, uint8_t *frame);
void hostvideo_close(hostvideo_dec *d);

typedef struct hostaudio_dec hostaudio_dec;

bool hostaudio_supported(uint32_t format);
/* A decoder for a sound description's format, with its magic cookie (an
 * 'esds' for AAC, nothing for MP3). */
hostaudio_dec *hostaudio_open(uint32_t format, int channels, double rate, const uint8_t *cookie, size_t cookie_len);
/* Packets (of the sizes given, or self-framing when sizes is NULL) into
 * 16-bit PCM of the channels given at open (*pcm malloc'd); the frames. */
uint32_t hostaudio_decode(hostaudio_dec *d, const uint8_t *data, size_t len, const uint32_t *sizes, int npackets,
                          int16_t **pcm);
void hostaudio_close(hostaudio_dec *d);

/* Still images written by the host's encoders (graphics exporters): file
 * types 'JPEG', 'PNGf', 'TIFF', 'BMPf', 'GIFf', '8BPS', 'TPIC', 'SGI ',
 * 'jp2 '.  Quality 0 to 1 for the lossy ones; *out malloc'd. */
bool hostimage_can_encode(uint32_t file_type);
bool hostimage_encode(uint32_t file_type, const uint8_t *rgba, int w, int h, double quality, double dpi, uint8_t **out,
                      size_t *len);

#endif
