/*
 * Image codecs as components: a decompressor ('imdc') for each codec we
 * decode (ours, qt_codec*.c, and the host's, hostvideo.h) and a compressor
 * ('imco') for each we encode (qt_encode.c), so programs find them with
 * FindNextComponent, open them and call them as they did Apple's.  They
 * answer the classic band calls (PreDecompress, BandDecompress, PreCompress,
 * BandCompress) and the base codec's calls a subcodec answers (Initialize,
 * Preflight, BeginBand, DrawBand, EndBand).
 *
 * The other way round: a codec a program brings (a 68k or PowerPC 'imdc' of
 * its own) decodes what ours can't, called as the Image Compression Manager
 * called it, PreDecompress then BandDecompress into a 32-bit pixel map.
 *
 * CodecDecompressParams (mac68k alignment): sequenceID 0, imageDescription
 * 4, data 8, bufferSize 12, frameNumber 16, startLine 20, stopLine 24,
 * conditionFlags 28, callerFlags 32, capabilities 34, progressProcRecord 38,
 * completionProcRecord 46, dataProcRecord 54, port 62, dstPixMap 66 (a
 * PixMap, 50 bytes), maskBits 116, mattePixMap 120, srcRect 124, matrix 132,
 * accuracy 136, transferMode 140, frameTime 142, reserved 146, matrixFlags
 * 150, matrixType 151, dstRect 152.  CodecCompressParams: the same to
 * capabilities, then progress 38, completion 46, flush 54, srcPixMap 62,
 * prevPixMap 112, spatialQuality 162, temporalQuality 166, similarity 170.
 * CodecCapabilities: flags 0, wantedPixelSize 4, extendWidth 6,
 * extendHeight 8, bandMin 10, bandInc 12, pad 14, time 16, flags2 20.
 * ImageSubCodecDecompressRecord: baseAddr 0, rowBytes 4, codecData 8.
 */
#include <stdlib.h>
#include <string.h>

#include "hostvideo.h"
#include "qd_internal.h"
#include "qt_codec.h"
#include "qt_comp.h"
#include "qt_errors.h"
#include "tb_internal.h"

enum {
    IMDC = 0x696D6463, IMCO = 0x696D636F, APPL = 0x6170706C,
    /* codecInfo flags: the depths done, and temporal compression */
    codecInfoDoes1 = 1, codecInfoDoes2 = 2, codecInfoDoes4 = 4, codecInfoDoes8 = 8, codecInfoDoes16 = 16,
    codecInfoDoes32 = 32, codecInfoDoesTemporal = 1 << 10, codecInfoDoesSpool = 1 << 2 << 8,
    codecInfoDepth24 = 1 << 3 << 8, codecInfoDepth32 = 1 << 4 << 8,
};

/* The codec types we may have, and their names. */
static const struct {
    uint32_t type;
    const char *name;
} known[] = {
    { 0x72617720, "None" },             /* 'raw ' */
    { 0x726C6520, "Animation" },        /* 'rle ' */
    { 0x736D6320, "Graphics" },         /* 'smc ' */
    { 0x72707A61, "Video" },            /* 'rpza' */
    { 0x63766964, "Cinepak" },          /* 'cvid' */
    { 0x6A706567, "Photo - JPEG" },     /* 'jpeg' */
    { 0x6D6A7061, "Motion JPEG A" },    /* 'mjpa' */
    { 0x6D6A7062, "Motion JPEG B" },    /* 'mjpb' */
    { 0x68323633, "H.263" },            /* 'h263' */
    { 0x6D703476, "MPEG-4 Video" },     /* 'mp4v' */
    { 0x61766331, "H.264" },            /* 'avc1' */
    { 0x6D703176, "MPEG-1 Video" },     /* 'mp1v' */
    { 0x6D703276, "MPEG-2 Video" },     /* 'mp2v' */
    { 0x53565131, "Sorenson Video" },   /* 'SVQ1' */
    { 0x53565133, "Sorenson Video 3" }, /* 'SVQ3' */
    { 0x64766320, "DV - NTSC" },        /* 'dvc ' */
    { 0x64766370, "DV - PAL" },         /* 'dvcp' */
};
#define NKNOWN (sizeof known / sizeof known[0])

const char *qt_codec_type_name(uint32_t type)
{
    for (size_t i = 0; i < NKNOWN; i++)
        if (known[i].type == type)
            return known[i].name;
    return NULL;
}

bool qt_codec_decodes(uint32_t type)
{
    return qt_vdec_supported(type, 24) || qt_vdec_supported(type, 32) || qt_vdec_supported(type, 8) ||
           qt_vdec_supported(type, 16);
}

bool qt_codec_encodes(uint32_t type) { return qt_venc_supported(type, 24) || qt_venc_supported(type, 8); }

/* CodecInfo: typeName 0 (Str31), version 32, revisionLevel 34, vendor 36,
 * decompressFlags 40, compressFlags 44, formatFlags 48, accuracies 52-53,
 * speeds 54-57, compressionLevel 58, minimum height and width 60-63,
 * latencies 64-67, privateData 68. */
void qt_codec_fill_info(gaddr info, uint32_t type, bool decompress, bool compress)
{
    const char *name = qt_codec_type_name(type);
    gmemset(info, 0, 72);
    size_t n = name ? strlen(name) : 0;
    wr8(info, (uint8_t)n);
    gput(info + 1, name ? name : "", (uint32_t)n);
    wr16(info + 32, 1);
    wr16(info + 34, 1);
    wr32(info + 36, APPL);
    if (decompress)
        wr32(info + 40, codecInfoDoes1 | codecInfoDoes2 | codecInfoDoes4 | codecInfoDoes8 | codecInfoDoes16 |
                            codecInfoDoes32);
    if (compress) {
        uint32_t flags = 0;
        static const int depths[6] = { 1, 2, 4, 8, 16, 32 };
        for (int i = 0; i < 6; i++)
            if (qt_venc_supported(type, depths[i]) || (depths[i] == 32 && qt_venc_supported(type, 24)))
                flags |= 1u << i;
        if (qt_venc_temporal(type))
            flags |= codecInfoDoesTemporal;
        wr32(info + 44, flags);
        wr8(info + 52, 128);
    }
    wr32(info + 48, codecInfoDepth24 | codecInfoDepth32);
    wr8(info + 53, 128);
    if (type == 0x63766964 || type == 0x72707A61) { /* 4x4 blocks */
        wr16(info + 60, 4);
        wr16(info + 62, 4);
    } else {
        wr16(info + 60, 1);
        wr16(info + 62, 1);
    }
}

/* ---- Writing a frame into a pixel map ---- */

/* A PixMap (the record itself) as a destination. */
static bool pixmap_dst(gaddr pm, pixdst *d)
{
    if (!pm)
        return false;
    uint16_t rb = (uint16_t)rd16(pm + 4);
    d->base = qd_pixmap_base(pm, true); /* through the handle a GWorld's may be */
    d->row_bytes = rb & 0x3FFF;
    d->bounds = qrect_get(pm + 6);
    d->depth = rb & 0x8000 ? (int16_t)rd16(pm + 32) : 1;
    d->ctab = rb & 0x8000 ? rd32(pm + 42) : 0;
    pixdst_clamp(d);
    return d->base != 0;
}

/* The frame (w by h RGBA) scaled into rect r of the pixel map (bounds
 * coordinates), clipped to its bounds. */
void qt_codec_frame_to_pixmap(const uint8_t *rgba, int w, int h, gaddr pm, qrect r)
{
    pixdst d;
    if (!pixmap_dst(pm, &d) || w <= 0 || h <= 0)
        return;
    int dw = r.right - r.left, dh = r.bottom - r.top;
    if (dw <= 0 || dh <= 0)
        return;
    for (int y = 0; y < dh; y++) {
        int ty = r.top + y;
        if (ty < d.bounds.top || ty >= d.bounds.bottom)
            continue;
        int sy = y * h / dh;
        for (int x = 0; x < dw; x++) {
            int tx = r.left + x;
            if (tx < d.bounds.left || tx >= d.bounds.right)
                continue;
            const uint8_t *p = rgba + ((size_t)sy * (size_t)w + (size_t)(x * w / dw)) * 4;
            uint32_t v;
            if (d.depth == 32) {
                v = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
            } else {
                uint16_t c[3] = { (uint16_t)(p[0] * 0x101), (uint16_t)(p[1] * 0x101), (uint16_t)(p[2] * 0x101) };
                v = qd_rgb_px(d.depth, d.ctab, c);
            }
            put_px(&d, tx - d.bounds.left, ty - d.bounds.top, v);
        }
    }
}

/* A pixel map's rect (bounds coordinates) as RGBA (malloc'd). */
uint8_t *qt_codec_pixmap_to_rgba(gaddr pm, qrect r)
{
    pixdst d;
    int w = r.right - r.left, h = r.bottom - r.top;
    if (!pixmap_dst(pm, &d) || w <= 0 || h <= 0)
        return NULL;
    uint8_t *out = calloc((size_t)w * (size_t)h, 4);
    if (!out)
        return NULL;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int sx = r.left + x - d.bounds.left, sy = r.top + y - d.bounds.top;
            uint8_t *o = out + ((size_t)y * (size_t)w + (size_t)x) * 4;
            o[3] = 255;
            if (sx < 0 || sy < 0 || sx >= d.bounds.right - d.bounds.left || sy >= d.bounds.bottom - d.bounds.top)
                continue;
            uint32_t v = get_px(&d, sx, sy);
            if (d.depth == 32) {
                o[0] = (uint8_t)(v >> 16);
                o[1] = (uint8_t)(v >> 8);
                o[2] = (uint8_t)v;
            } else {
                uint16_t c[3];
                qd_px_rgb(d.depth, d.ctab, v, c);
                o[0] = (uint8_t)(c[0] >> 8);
                o[1] = (uint8_t)(c[1] >> 8);
                o[2] = (uint8_t)(c[2] >> 8);
            }
        }
    return out;
}

/* ---- Our codecs as components ---- */

typedef struct {
    qt_vdec *dec;
    uint8_t *desc;          /* the description the decoder was made for */
    uint32_t desc_len;
    int w, h;
    uint8_t *frame;
    int32_t data_size;      /* BeginBand's buffer size, for DrawBand */
    gaddr settings;
} codec;

static void drop_decoder(codec *c)
{
    qt_vdec_free(c->dec);
    free(c->desc);
    free(c->frame);
    c->dec = NULL;
    c->desc = NULL;
    c->frame = NULL;
}

/* The decoder for an ImageDescriptionHandle (kept while it is the same). */
static int16_t decoder_for(codec *c, gaddr idh)
{
    gaddr d = idh ? rd32(idh) : 0;
    if (!d)
        return paramErr;
    uint32_t n = rd32(d);
    if (n < 86 || n > 1 << 20)
        return badImageDescription;
    uint8_t *b = malloc(n);
    gget(b, d, n);
    if (c->dec && c->desc_len == n && !memcmp(c->desc, b, n)) {
        free(b);
        return noErr;
    }
    drop_decoder(c);
    qt_video_format f;
    uint32_t palette[256];
    if (!qt_video_format_of_desc(b, n, &f, palette)) {
        free(b);
        return badImageDescription;
    }
    c->dec = qt_vdec_new(&f);
    if (!c->dec) {
        free(b);
        return noCodecErr;
    }
    c->desc = b;
    c->desc_len = n;
    c->w = f.width;
    c->h = f.height;
    c->frame = calloc((size_t)f.width * (size_t)f.height, 4);
    return c->frame ? noErr : memFullErr;
}

static bool decode_into_frame(codec *c, gaddr data, int32_t size)
{
    if (!data || size <= 0)
        return false;
    uint8_t *b = malloc((size_t)size);
    if (!b)
        return false;
    gget(b, data, (uint32_t)size);
    bool ok = qt_vdec_decode(c->dec, b, (size_t)size, c->frame);
    free(b);
    return ok;
}

static int32_t data_size_of(codec *c, int32_t given)
{
    if (given > 0)
        return given;
    int32_t n = c->desc_len >= 48 ? (int32_t)(c->desc[44] << 24 | c->desc[45] << 16 | c->desc[46] << 8 | c->desc[47])
                                  : 0;
    return n > 0 ? n : 1 << 20;
}

static void set_capabilities(gaddr cap, int w, int h)
{
    if (!cap)
        return;
    wr16(cap + 4, 32);   /* wantedPixelSize */
    wr16(cap + 6, 0);
    wr16(cap + 8, 0);
    wr16(cap + 10, (uint16_t)h); /* the whole image as one band */
    wr16(cap + 12, (uint16_t)h);
    wr32(cap + 16, 0);
    (void)w;
}

static int32_t decompress_call(codec *c, qtc_call *k)
{
    uint32_t *a = k->a;
    switch (k->what) {
    case 0x0005:   /* PreDecompress(params) */
    case 0x0200: { /* Preflight(params) */
        gaddr p = a[0];
        int16_t err = decoder_for(c, rd32(p + 4));
        if (err)
            return err;
        set_capabilities(rd32(p + 34), c->w, c->h);
        return noErr;
    }
    case 0x0006: { /* BandDecompress(params) */
        gaddr p = a[0];
        int16_t err = decoder_for(c, rd32(p + 4));
        if (err)
            return err;
        if (!decode_into_frame(c, rd32(p + 8), data_size_of(c, (int32_t)rd32(p + 12))))
            return codecBadDataErr;
        qrect r = qrect_get(p + 152);
        if (qrect_empty(r)) {
            qrect b = qrect_get(p + 66 + 6);
            r = (qrect){ b.top, b.left, (int16_t)(b.top + c->h), (int16_t)(b.left + c->w) };
        }
        qt_codec_frame_to_pixmap(c->frame, c->w, c->h, p + 66, r);
        return noErr;
    }
    case 0x0201: { /* Initialize(ImageSubCodecDecompressCapabilities *) */
        gaddr cap = a[0];
        if (cap) {
            wr32(cap + 4, 0); /* no record of our own */
            wr8(cap + 8, 0);  /* not asynchronous */
        }
        return noErr;
    }
    case 0x0202: { /* BeginBand(params, drp, flags) */
        gaddr p = a[0];
        int16_t err = decoder_for(c, rd32(p + 4));
        if (err)
            return err;
        c->data_size = (int32_t)rd32(p + 12);
        return noErr;
    }
    case 0x0203: { /* DrawBand(drp): 32-bit ARGB rows at baseAddr */
        gaddr drp = a[0];
        if (!c->dec)
            return codecErr;
        if (!decode_into_frame(c, rd32(drp + 8), data_size_of(c, c->data_size)))
            return codecBadDataErr;
        gaddr base = rd32(drp);
        uint32_t rb = rd32(drp + 4);
        for (int y = 0; y < c->h; y++)
            for (int x = 0; x < c->w; x++) {
                const uint8_t *s = c->frame + ((size_t)y * (size_t)c->w + (size_t)x) * 4;
                wr32(base + (uint32_t)y * rb + (uint32_t)x * 4,
                     0xFF000000u | (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2]);
            }
        return noErr;
    }
    case 0x0204: /* EndBand */
        return noErr;
    case 0x0008: { /* GetCompressedImageSize(desc, data, bufferSize, dataProc, dataSize *) */
        gaddr d = a[0] ? rd32(a[0]) : 0;
        if (!d || !a[4])
            return paramErr;
        wr32(a[4], rd32(d + 44));
        return noErr;
    }
    case 0x0010: { /* IsImageDescriptionEquivalent(newDesc, equivalent *) */
        gaddr d = a[0] ? rd32(a[0]) : 0;
        if (a[1])
            wr8(a[1], d && c->desc && c->desc_len >= 86 && rd32(d + 4) == (uint32_t)(c->desc[4] << 24 |
                      c->desc[5] << 16 | c->desc[6] << 8 | c->desc[7]) &&
                          rd16(d + 32) == (uint16_t)(c->desc[32] << 8 | c->desc[33]) &&
                          rd16(d + 34) == (uint16_t)(c->desc[34] << 8 | c->desc[35]) &&
                          rd16(d + 82) == (uint16_t)(c->desc[82] << 8 | c->desc[83]));
        return noErr;
    }
    case 0x002B: /* GetDecompressLatency(TimeRecord *): none */
        if (a[0]) {
            gmemset(a[0], 0, 16);
            wr32(a[0] + 8, 600);
        }
        return noErr;
    }
    return qtcBadSelector;
}

static int32_t compress_call(codec *c, qtc_call *k, uint32_t type)
{
    (void)c;
    uint32_t *a = k->a;
    switch (k->what) {
    case 0x0002: /* GetMaxCompressionSize(src, srcRect, depth, quality, size *) */
        return tb_GetMaxCompressionSize((int32_t)a[0], a[1], (int16_t)a[2], (int32_t)a[3], (int32_t)type, 0, a[4]);
    case 0x0016: /* ...WithSources(src, srcRect, depth, quality, sourceData, size *) */
        return tb_GetMaxCompressionSize((int32_t)a[0], a[1], (int16_t)a[2], (int32_t)a[3], (int32_t)type, 0, a[5]);
    case 0x0001: /* GetCompressionTime(src, srcRect, depth, spatialQuality *, temporalQuality *, time *) */
        return tb_GetCompressionTime((int32_t)a[0], a[1], (int16_t)a[2], (int32_t)type, 0, a[3], a[4], a[5]);
    case 0x0003: { /* PreCompress(params) */
        gaddr p = a[0], d = rd32(p + 4) ? rd32(rd32(p + 4)) : 0;
        int depth = d ? (int16_t)rd16(d + 82) : 32;
        if (!qt_venc_supported(type, depth) && !(depth == 32 && qt_venc_supported(type, 24)))
            return codecConditionErr;
        gaddr cap = rd32(p + 34);
        if (cap) {
            wr16(cap + 4, (uint16_t)(depth == 24 ? 32 : depth > 32 ? 8 : depth));
            wr16(cap + 10, d ? rd16(d + 34) : 0);
            wr16(cap + 12, d ? rd16(d + 34) : 0);
        }
        return noErr;
    }
    case 0x0004: { /* BandCompress(params): the source pixel map compressed into data */
        gaddr p = a[0], idh = rd32(p + 4);
        gaddr d = idh ? rd32(idh) : 0;
        int16_t depth = d ? (int16_t)rd16(d + 82) : 0;
        gaddr mp = tb_NewPtr(4, false, true); /* a handle to the record in the parameters */
        if (!mp)
            return memFullErr;
        wr32(mp, p + 62);
        gaddr rect = tb_NewPtr(8, false, true);
        qrect_put(rect, qrect_get(p + 62 + 6));
        int16_t err = tb_FCompressImage((int32_t)mp, rect, depth, (int32_t)rd32(p + 162), (int32_t)type, 0, 0, 0,
                                        (int32_t)rd32(p + 12), 0, 0, (int32_t)idh, (int32_t)rd32(p + 8));
        tb_DisposePtr(rect);
        tb_DisposePtr(mp);
        return err;
    }
    }
    return qtcBadSelector;
}

static int32_t codec_fn(qtc_call *k)
{
    codec *c = k->self;
    uint32_t type = k->cls->subtype;
    bool comp = k->cls->type == IMCO;
    if (k->what == kQTCClose) {
        drop_decoder(c);
        if (c->settings)
            tb_DisposeHandle(c->settings);
        return noErr;
    }
    if (k->what < 0 || !k->call)
        return qtcBadSelector;
    uint32_t *a = k->a;
    switch (k->what) {
    case 0x0000: /* GetCodecInfo(info) */
        if (!a[0])
            return paramErr;
        qt_codec_fill_info(a[0], type, !comp, comp);
        return noErr;
    case 0x0007: /* Busy(seq) */
        return 0;
    case 0x000E: /* Flush */
    case 0x0019: /* FlushFrame */
    case 0x0207: /* DroppingFrame */
    case 0x0205: /* QueueStarting */
    case 0x0206: /* QueueStopping */
    case 0x0017: /* SetTimeBase */
    case 0x000F: /* SetTimeCode */
    case 0x0018: /* SourceChanged */
        return noErr;
    case 0x000B: /* RequestSettings(settings, rp, filterProc): nothing to ask */
        return codecUnimpErr;
    case 0x000C: /* GetSettings(settings): none */
        if (a[0])
            tb_SetHandleSize(a[0], 0);
        return noErr;
    case 0x000D: /* SetSettings(settings) */
        return noErr;
    case 0x0011: { /* NewMemory(data **, dataSize, dataUse, memoryGoneProc, refCon) */
        gaddr p = tb_NewPtr((int32_t)a[1], false, true);
        if (a[0])
            wr32(a[0], p);
        return p ? noErr : memFullErr;
    }
    case 0x0012: /* DisposeMemory(data) */
        if (a[0])
            tb_DisposePtr(a[0]);
        return noErr;
    case 0x0029: /* GetSourceDataGammaLevel(gamma *): the default */
        if (a[0])
            wr32(a[0], 0);
        return noErr;
    case 0x0028: /* RequestGammaLevel(src, dst, codecCanMatch *) */
        if (a[2])
            wr32(a[2], 0);
        return noErr;
    }
    return comp ? compress_call(c, k, type) : decompress_call(c, k);
}

#define MAX_CLASSES (2 * NKNOWN)
static qtc_class classes[MAX_CLASSES];
static int nclasses;
static uint32_t ours[MAX_CLASSES];

bool qt_codec_component_is_ours(uint32_t component)
{
    for (int i = 0; i < nclasses; i++)
        if (ours[i] == component)
            return true;
    return false;
}

void qt_codec_components_register(void)
{
    nclasses = 0;
    for (size_t i = 0; i < NKNOWN; i++)
        for (int comp = 0; comp < 2; comp++) {
            uint32_t type = known[i].type;
            /* Photo-JPEG goes through the host's image codecs, which the
             * host may install after the components are registered. */
            bool host_jpeg = type == 0x6A706567;
            if (comp ? !qt_codec_encodes(type) && !host_jpeg : !qt_codec_decodes(type) && !host_jpeg)
                continue;
            qtc_class *k = &classes[nclasses];
            *k = (qtc_class){ comp ? IMCO : IMDC, type, APPL, 0, known[i].name, known[i].name, 0x00010001,
                              { "ImageCodec" }, sizeof(codec), codec_fn, 0 };
            k->flags = comp ? 0x2F : 0x3F; /* the depths, as their codecInfo flags */
            ours[nclasses++] = qtc_register(k);
        }
    qt_gexport_register(); /* and the graphics exporters (qt_gexport.c) */
}

/* ---- A program's own decompressor ---- */

typedef struct {
    uint32_t ci;
    gaddr desc_h, params, cap, pixels;
    int w, h;
    bool prepared;
} prog_codec;

static uint32_t program_component(uint32_t format)
{
    for (uint32_t c = components_find_next(0, IMDC, format, 0); c; c = components_find_next(c, IMDC, format, 0))
        if (!qt_codec_component_is_ours(c))
            return c;
    return 0;
}

bool qt_codec_program_supports(uint32_t format) { return program_component(format) != 0; }
static bool prog_supports(int depth) { return depth > 0; }

static void prog_close(void *s)
{
    prog_codec *p = s;
    if (!p)
        return;
    if (p->ci)
        components_close(p->ci);
    if (p->desc_h)
        tb_DisposeHandle(p->desc_h);
    if (p->params)
        tb_DisposePtr(p->params);
    if (p->cap)
        tb_DisposePtr(p->cap);
    if (p->pixels)
        tb_DisposePtr(p->pixels);
    free(p);
}

static void *prog_open(const qt_video_format *f)
{
    uint32_t comp = program_component(f->format);
    if (!comp || !f->desc || f->desc_len < 86)
        return NULL;
    prog_codec *p = calloc(1, sizeof *p);
    int16_t err;
    p->ci = components_open_component(comp, &err);
    p->w = f->width;
    p->h = f->height;
    p->desc_h = tb_NewHandle((int32_t)f->desc_len, false, true);
    p->params = tb_NewPtr(256, false, true);
    p->cap = tb_NewPtr(32, false, true);
    p->pixels = tb_NewPtr(p->w * p->h * 4, false, true);
    if (!p->ci || !p->desc_h || !p->params || !p->cap || !p->pixels) {
        prog_close(p);
        return NULL;
    }
    gput(rd32(p->desc_h), f->desc, f->desc_len);
    gaddr q = p->params;
    wr32(q + 4, p->desc_h);
    wr32(q + 34, p->cap);
    wr32(q + 62, qd_port());
    gaddr pm = q + 66; /* a 32-bit pixel map over our pixels */
    wr32(pm, p->pixels);
    wr16(pm + 4, (uint16_t)(0x8000 | (p->w * 4)));
    qrect_put(pm + 6, (qrect){ 0, 0, (int16_t)p->h, (int16_t)p->w });
    wr32(pm + 22, 72u << 16);
    wr32(pm + 26, 72u << 16);
    wr16(pm + 30, 16); /* RGBDirect */
    wr16(pm + 32, 32);
    wr16(pm + 34, 3);
    wr16(pm + 36, 8);
    qrect_put(q + 124, (qrect){ 0, 0, (int16_t)p->h, (int16_t)p->w });
    wr32(q + 136, 0x300); /* codecNormalQuality */
    qrect_put(q + 152, (qrect){ 0, 0, (int16_t)p->h, (int16_t)p->w });
    return p;
}

static bool prog_decode(void *s, const qt_video_format *f, const uint32_t *palette, const uint8_t *data, size_t len,
                        uint8_t *frame)
{
    (void)f, (void)palette;
    prog_codec *p = s;
    gaddr buf = tb_NewPtr((int32_t)len, false, false);
    if (!buf)
        return false;
    gput(buf, data, (uint32_t)len);
    gaddr q = p->params;
    wr32(q + 8, buf);
    wr32(q + 12, (uint32_t)len);
    wr32(q + 62, qd_port());
    uint32_t args[1] = { q };
    int32_t r = noErr;
    if (!p->prepared) {
        r = qtc_call_named(p->ci, "ImageCodecPreDecompress", args);
        p->prepared = r == noErr;
    }
    if (r == noErr)
        r = qtc_call_named(p->ci, "ImageCodecBandDecompress", args);
    wr32(q + 16, rd32(q + 16) + 1); /* frameNumber */
    tb_DisposePtr(buf);
    if (r != noErr)
        return false;
    for (int y = 0; y < p->h; y++)
        for (int x = 0; x < p->w; x++) {
            uint32_t v = rd32(p->pixels + (uint32_t)(y * p->w + x) * 4);
            uint8_t *o = frame + ((size_t)y * (size_t)p->w + (size_t)x) * 4;
            o[0] = (uint8_t)(v >> 16);
            o[1] = (uint8_t)(v >> 8);
            o[2] = (uint8_t)v;
            o[3] = 255;
        }
    return true;
}

const qt_video_codec qt_codec_program = { 0, prog_supports, prog_open, prog_decode, prog_close };
