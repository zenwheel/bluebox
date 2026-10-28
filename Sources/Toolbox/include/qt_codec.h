/* QuickTime's classic codecs, decoded natively (qt_codec_*.c). */
#ifndef QT_CODEC_H
#define QT_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mem.h"

/* ---- Video ----
 *
 * A decoder turns a video track's samples into a frame of RGBA bytes
 * (R, G, B, 255 per pixel, rows top down, width * 4 bytes each).  The frame
 * belongs to the caller and is kept between samples: the classic codecs
 * code most frames as changes to the one before.  Samples come in decoding
 * order, a key frame first.
 */
typedef struct qt_vdec qt_vdec;

typedef struct {
    uint32_t format;       /* the sample description's codec: 'cvid', 'rle ', 'smc ', 'rpza', 'raw ' */
    int width, height;     /* the sample description's size */
    int depth;             /* its depth: 1-32, or 33-40 for 1-8-bit grey */
    const uint32_t *palette; /* depths 1-8: 1 << depth colours, 0x00RRGGBB; NULL for the standard Mac ones */
    const uint8_t *desc;   /* the whole ImageDescription when known (for the host's and programs' codecs, */
    uint32_t desc_len;     /* which want its extensions); only while the decoder is made */
} qt_video_format;

/* An ImageDescription's (big-endian bytes) format; palette gets its colour
 * table, if any.  desc stays pointing into d. */
bool qt_video_format_of_desc(const uint8_t *d, uint32_t len, qt_video_format *f, uint32_t palette[256]);

/* Whether there is a decoder for this codec at this depth. */
bool qt_vdec_supported(uint32_t format, int depth);
qt_vdec *qt_vdec_new(const qt_video_format *f);
/* Decode one sample into frame (f->width * f->height * 4 bytes); false if
 * the sample is damaged (the frame may be partly updated). */
bool qt_vdec_decode(qt_vdec *d, const uint8_t *data, size_t len, uint8_t *frame);
void qt_vdec_free(qt_vdec *d);

/* The standard Mac colours for depths 1, 2, 4 and 8 (0x00RRGGBB), and the
 * grey ramps for depths 33-40 (white first, as QuickDraw's grey tables). */
const uint32_t *qt_standard_palette(int depth);

/* One codec's entry points, for qt_codec.c's table. */
typedef struct {
    uint32_t format;
    bool (*supports)(int depth);
    void *(*open)(const qt_video_format *f);
    bool (*decode)(void *state, const qt_video_format *f, const uint32_t *palette, const uint8_t *data,
                   size_t len, uint8_t *frame);
    void (*close)(void *state);
} qt_video_codec;

extern const qt_video_codec qt_codec_cvid;  /* Cinepak (qt_codec_cvid.c) */
extern const qt_video_codec qt_codec_rle;   /* Animation (qt_codec_apple.c) */
extern const qt_video_codec qt_codec_smc;   /* Graphics */
extern const qt_video_codec qt_codec_rpza;  /* Apple Video (Road Pizza) */
extern const qt_video_codec qt_codec_raw;   /* None */
extern const qt_video_codec qt_codec_program; /* a program's own 'imdc' (qt_codec_comp.c) */
bool qt_codec_program_supports(uint32_t format);

/* ---- Codecs as components (qt_codec_comp.c) ---- */

void qt_codec_components_register(void);
bool qt_codec_component_is_ours(uint32_t component);
/* A codec type's name ("Cinepak"), NULL for one we don't know. */
const char *qt_codec_type_name(uint32_t type);
bool qt_codec_decodes(uint32_t type);
bool qt_codec_encodes(uint32_t type);
/* A CodecInfo record (72 bytes) for one of ours. */
void qt_codec_fill_info(gaddr info, uint32_t type, bool decompress, bool compress);

/* ---- Video compression (qt_encode.c) ----
 *
 * A frame to compress is width * height pixel values at the codec depth:
 * colour indices at 1-8 bits and 33-40 (grey), xRGB 1-5-5-5 at 16,
 * 0x00RRGGBB at 24 and 32.  Formats: 'raw ', 'rle ', 'jpeg' (with the
 * host's encoder).  Quality is a CodecQ (0 to 0x400, lossless).
 */
typedef struct {
    uint32_t format;
    int width, height, depth;
    int32_t quality;
} qt_venc_format;

bool qt_venc_supported(uint32_t format, int depth);
/* Whether the format codes frames as changes to the one before. */
bool qt_venc_temporal(uint32_t format);
/* The most bytes a frame can take. */
size_t qt_venc_max_size(const qt_venc_format *f);
/* Compress one frame into *out (malloc'd); prev, when not NULL, is the
 * frame before for a difference frame.  Returns the size, 0 on failure. */
size_t qt_venc_encode(const qt_venc_format *f, const uint32_t *px, const uint32_t *prev, uint8_t **out);

/* ---- The Image Compression Manager's compression side (qt_icm_compress.c) ---- */

void icm_compress_reset(void);
/* CDSequenceEnd for a compression sequence: false if seqID is not one. */
bool icm_csequence_end(int32_t seqID);
bool icm_csequence_exists(int32_t seqID);
/* The Standard Image Compression dialog component ('scdi', qt_scdi.c),
 * registered again after the Component Manager is reset. */
void icm_components_reset(void);
/* A still image through the host's decoder (qt_codec.c): RGBA, malloc'd. */
bool qt_decode_image(const uint8_t *data, size_t len, uint8_t **rgba, int *w, int *h);
/* The graphics importer component (qt_gimport.c): a new run. */
void qt_gimport_register(void);
/* A PowerPC program's component call through ComponentDispatch: the
 * instance in r3, then arguments of the sizes given, "4" a long and "2" a
 * word (qt_gimport.c). */
struct ppc_cpu;
void qt_component_call_ppc(struct ppc_cpu *c, int16_t what, const char *sizes);
/* QuickTime's clock component (vam.c): a new run; an instance for a sound
 * channel's getClockComponentCmd. */
void qt_clock_register(void);
uint32_t qt_clock_open(void); /* a ComponentInstance */

/* ---- Sound ----
 *
 * A sound track's samples as 16-bit PCM, interleaved when stereo.
 */
typedef struct {
    uint32_t format;       /* 'raw ' (8-bit offset), 'twos', 'sowt', 'ima4', 'MAC3', 'MAC6', 'ulaw', 'alaw',
                              'in24', 'in32', 'fl32', 'fl64', and the host's ('.mp3', 'mp4a', ...) */
    int channels;          /* 1 or 2 */
    int bits;              /* 8 or 16 for the uncompressed formats */
    double rate;           /* frames per second */
    /* a version 1 sound description's packets: frames in one, and bytes
     * in one for all channels (0: not known) */
    uint32_t frames_per_packet, bytes_per_packet;
    bool little;           /* 'in24'...'fl64' little-endian (an 'enda' atom) */
    const uint8_t *cookie; /* the decoder's magic cookie (an 'esds'), owned by the caller */
    uint32_t cookie_len;
} qt_sound_format;

bool qt_adec_supported(const qt_sound_format *f);
/* Whether the host decodes it (in packets rather than frames). */
bool qt_adec_host(const qt_sound_format *f);
/* Bytes of coded data holding n frames (whole packets for 'ima4' and MACE). */
size_t qt_adec_bytes(const qt_sound_format *f, uint32_t frames);
/* Decode len bytes into *pcm (malloc'd); returns the frames decoded. */
uint32_t qt_adec_decode(const qt_sound_format *f, const uint8_t *data, size_t len, int16_t **pcm);
/* Packets of the sizes given (the host's formats with a size per sample). */
uint32_t qt_adec_decode_packets(const qt_sound_format *f, const uint8_t *data, size_t len, const uint32_t *sizes,
                                int npackets, int16_t **pcm);
/* A sound description's packet fields, endianness and cookie: the entry
 * (from its size field) of len bytes. */
void qt_sound_desc_extras(const uint8_t *entry, uint32_t len, qt_sound_format *f);

#endif
