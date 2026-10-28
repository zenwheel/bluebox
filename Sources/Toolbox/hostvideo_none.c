/*
 * The host's decoders where there are none yet (Linux, until a GStreamer
 * implementation of hostvideo.h): nothing is decoded, so movies in codecs
 * Blue Box lacks fall back to libvlc, as before.
 */
#ifndef __APPLE__
#include "hostvideo.h"

bool hostvideo_supported(uint32_t format) { return (void)format, false; }
hostvideo_dec *hostvideo_open(uint32_t format, int width, int height, const uint8_t *desc, size_t desc_len)
{
    (void)format, (void)width, (void)height, (void)desc, (void)desc_len;
    return NULL;
}
bool hostvideo_decode(hostvideo_dec *d, const uint8_t *data, size_t len, uint8_t *frame)
{
    (void)d, (void)data, (void)len, (void)frame;
    return false;
}
void hostvideo_close(hostvideo_dec *d) { (void)d; }

bool hostaudio_supported(uint32_t format) { return (void)format, false; }
hostaudio_dec *hostaudio_open(uint32_t format, int channels, double rate, const uint8_t *cookie, size_t cookie_len)
{
    (void)format, (void)channels, (void)rate, (void)cookie, (void)cookie_len;
    return NULL;
}
uint32_t hostaudio_decode(hostaudio_dec *d, const uint8_t *data, size_t len, const uint32_t *sizes, int npackets,
                          int16_t **pcm)
{
    (void)d, (void)data, (void)len, (void)sizes, (void)npackets;
    *pcm = NULL;
    return 0;
}
void hostaudio_close(hostaudio_dec *d) { (void)d; }
#endif
