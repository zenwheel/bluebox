/*
 * The Image Compression Manager's compression side, on our compressors
 * (qt_encode.c): CompressImage and FCompressImage, compression sequences
 * (their ids 'CS' and a slot, the decompression ones' 'QS'), the sizes and
 * times programs ask about first, compressed pictures, image descriptions
 * for pixel maps, and the list of compressors.
 *
 * Pixels come from a PixMap and a rectangle in its bounds; they are read
 * at the codec's depth, through RGB when that differs from the source's.
 * The ImageDescription written (see qt_icm.c) holds the source's colour
 * table at 86 for the indexed depths, clutID 0; clutID -1 for the rest.
 *
 * A compressed picture is a version 2 picture: the extended header, then
 * opcode $8200 holding its length, a matrix taking the image to the frame,
 * no matte, the mode, the image's srcRect, the accuracy, no mask, then the
 * ImageDescription and the data; then $00FF.  qd_picture.c draws it.
 */
#include <stdlib.h>
#include <string.h>

#include "qd_internal.h"
#include "qt_codec.h"
#include "tb_internal.h"

enum {
    codecErr = -8960, noCodecErr = -8961,
    RAW = 0x72617720, RLE = 0x726C6520, JPEG = 0x6A706567,
    codecFlagForceKeyFrame = 1 << 7,
    CSEQ_BASE = 0x43530001, /* 'CS' and the slot */
};

/* ---- Source pixels ---- */

typedef struct {
    pixdst d;
    qrect r; /* the part to compress, in the bounds' coordinates */
} source;

/* A PixMapHandle (or a BitMap's handle) and a rectangle in it; nil for the whole. */
static bool source_of(gaddr pmh, gaddr rect, source *s)
{
    if (!pmh || !rd32(pmh))
        return false;
    gaddr pm = rd32(pmh);
    uint16_t rb = (uint16_t)rd16(pm + 4);
    s->d.base = qd_pixmap_base(pm, true);
    s->d.row_bytes = rb & 0x3FFF;
    s->d.bounds = qrect_get(pm + 6);
    s->d.depth = rb & 0x8000 ? PixMap_pixelSize(pm) : 1;
    s->d.ctab = rb & 0x8000 ? PixMap_pmTable(pm) : 0;
    if (s->d.depth < 1)
        s->d.depth = 1;
    pixdst_clamp(&s->d);
    qrect r = rect ? qrect_get(rect) : s->d.bounds, b = s->d.bounds;
    if (r.top < b.top)
        r.top = b.top;
    if (r.left < b.left)
        r.left = b.left;
    if (r.bottom > b.bottom)
        r.bottom = b.bottom;
    if (r.right > b.right)
        r.right = b.right;
    s->r = r;
    return s->d.base && !qrect_empty(r);
}

static int bits_of(int depth) { return depth > 32 ? depth - 32 : depth; }
static int width_of(qrect r) { return r.right - r.left; }
static int height_of(qrect r) { return r.bottom - r.top; }

/* A colour as a pixel value at depth (an index in ctab at 1-8 bits). */
static uint32_t value_of(const uint16_t rgb[3], int depth, gaddr ctab)
{
    if (depth == 16)
        return (uint32_t)(rgb[0] >> 11) << 10 | (uint32_t)(rgb[1] >> 11) << 5 | (uint32_t)(rgb[2] >> 11);
    if (depth == 24 || depth == 32)
        return (uint32_t)(rgb[0] >> 8) << 16 | (uint32_t)(rgb[1] >> 8) << 8 | (uint32_t)(rgb[2] >> 8);
    if (depth > 32) { /* grey, white first */
        unsigned n = 1u << (depth - 32), lum = (rgb[0] * 30u + rgb[1] * 59u + rgb[2] * 11u) / 100u >> 8;
        return ((255 - lum) * (n - 1) + 127) / 255;
    }
    return qd_rgb_px(depth, ctab, rgb);
}

/* The source's pixels as values at depth (indices in ctab at 1-8 bits). */
static uint32_t *read_values(const source *s, int depth, gaddr ctab)
{
    int w = width_of(s->r), h = height_of(s->r), sd = s->d.depth;
    uint32_t *px = malloc((size_t)w * (size_t)h * sizeof *px);
    if (!px)
        return NULL;
    bool same = (sd == 16 && depth == 16) || (sd == 32 && (depth == 24 || depth == 32)) ||
                (sd <= 8 && depth == sd && ctab == s->d.ctab);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t v = get_px(&s->d, s->r.left - s->d.bounds.left + x, s->r.top - s->d.bounds.top + y);
            if (!same) {
                uint16_t rgb[3];
                qd_px_rgb(sd, s->d.ctab, v, rgb);
                v = value_of(rgb, depth, ctab);
            }
            px[(size_t)y * (size_t)w + (size_t)x] = v;
        }
    return px;
}

/* The depth a codec compresses at, for a requested depth (0: the source's). */
static int pick_depth(uint32_t type, int req, int src)
{
    int d = req ? req : src;
    if (type == JPEG)
        d = d == 40 ? 40 : 24;
    return qt_venc_supported(type, d) ? d : 0;
}

/* The colour table indices refer to: the caller's, else the source's when
 * the depth stays, else the standard one (0). */
static gaddr pick_ctab(int depth, const source *s, gaddr ctable)
{
    if (depth > 8)
        return 0;
    if (ctable)
        return ctable;
    return s && s->d.depth == depth ? s->d.ctab : 0;
}

static const char *compressor_name(uint32_t type)
{
    return type == RAW ? "None" : type == RLE ? "Animation" : type == JPEG ? "Photo - JPEG" : "";
}

/* Fill (or make, when h is 0) an ImageDescriptionHandle. */
static gaddr write_desc(gaddr h, uint32_t type, int w, int hgt, int depth, gaddr ctab, int32_t spatial,
                        int32_t temporal, uint32_t size)
{
    int n = depth <= 8 ? 1 << depth : 0;
    uint32_t total = 86 + (n ? 8 + (uint32_t)n * 8 : 0);
    if (!h)
        h = tb_NewHandle((int32_t)total, false, true);
    else if ((uint32_t)tb_GetHandleSize(h) != total)
        tb_SetHandleSize(h, (int32_t)total);
    if (!h || !rd32(h) || (uint32_t)tb_GetHandleSize(h) < total)
        return 0;
    gaddr p = rd32(h);
    gmemset(p, 0, total);
    wr32(p, total);
    wr32(p + 4, type);
    wr16(p + 16, 1);          /* version */
    wr16(p + 18, 1);          /* revisionLevel */
    wr32(p + 20, 0x6170706C); /* 'appl' */
    wr32(p + 24, (uint32_t)temporal);
    wr32(p + 28, (uint32_t)spatial);
    wr16(p + 32, (uint16_t)w);
    wr16(p + 34, (uint16_t)hgt);
    wr32(p + 36, 72u << 16);
    wr32(p + 40, 72u << 16);
    wr32(p + 44, size);
    wr16(p + 48, 1);
    const char *name = compressor_name(type);
    size_t len = strlen(name);
    wr8(p + 50, (uint8_t)len);
    gput(p + 51, name, (uint32_t)len);
    wr16(p + 82, (uint16_t)depth);
    wr16(p + 84, n ? 0 : 0xFFFF);
    if (n) {
        gaddr c = p + 86;
        wr16(c + 6, (uint16_t)(n - 1));
        for (int k = 0; k < n; k++) {
            uint16_t rgb[3];
            qd_px_rgb(depth, ctab, (uint32_t)k, rgb);
            gaddr e = c + 8 + (uint32_t)k * 8;
            wr16(e, (uint16_t)k);
            wr16(e + 2, rgb[0]);
            wr16(e + 4, rgb[1]);
            wr16(e + 6, rgb[2]);
        }
    }
    return h;
}

/* One image: its values at the codec's depth, compressed. */
static int16_t compress_values(const qt_venc_format *f, const uint32_t *px, const uint32_t *prev, gaddr data,
                               uint32_t *size)
{
    uint8_t *out = NULL;
    size_t len = qt_venc_encode(f, px, prev, &out);
    if (!len)
        return codecErr;
    if (data)
        gput(data, out, (uint32_t)len);
    free(out);
    *size = (uint32_t)len;
    return noErr;
}

int16_t tb_FCompressImage(int32_t src, gaddr srcRect, int16_t colorDepth, int32_t quality, int32_t cType,
                          int32_t codec, int32_t ctable, int16_t flags, int32_t bufferSize, int32_t flushProc,
                          int32_t progressProc, int32_t desc, int32_t data)
{
    source s;
    if (!source_of((gaddr)src, srcRect, &s) || !desc || !data)
        return paramErr;
    int depth = pick_depth((uint32_t)cType, colorDepth, s.d.depth);
    if (!depth)
        return noCodecErr;
    gaddr ctab = pick_ctab(depth, &s, (gaddr)ctable);
    uint32_t *px = read_values(&s, depth, ctab);
    if (!px)
        return memFullErr;
    qt_venc_format f = { (uint32_t)cType, width_of(s.r), height_of(s.r), depth, quality };
    uint32_t size = 0;
    int16_t err = compress_values(&f, px, NULL, (gaddr)data, &size);
    free(px);
    if (!err && !write_desc((gaddr)desc, (uint32_t)cType, f.width, f.height, depth, ctab, quality, 0, size))
        err = memFullErr;
    return err;
}

int16_t tb_CompressImage(int32_t src, gaddr srcRect, int32_t quality, int32_t cType, int32_t desc, int32_t data)
{
    return tb_FCompressImage(src, srcRect, 0, quality, cType, 0, 0, 0, 0, 0, 0, desc, data);
}

int16_t tb_GetMaxCompressionSize(int32_t src, gaddr srcRect, int16_t colorDepth, int32_t quality, int32_t cType,
                                 int32_t codec, gaddr size)
{
    source s;
    bool have = source_of((gaddr)src, srcRect, &s);
    qrect r = have ? s.r : srcRect ? qrect_get(srcRect) : (qrect){ 0, 0, 0, 0 };
    int depth = pick_depth((uint32_t)cType, colorDepth, have ? s.d.depth : 32);
    if (!size || qrect_empty(r))
        return paramErr;
    if (!depth)
        return noCodecErr;
    qt_venc_format f = { (uint32_t)cType, width_of(r), height_of(r), depth, quality };
    wr32(size, (uint32_t)qt_venc_max_size(&f));
    return noErr;
}

int16_t tb_GetCompressionTime(int32_t src, gaddr srcRect, int16_t colorDepth, int32_t cType, int32_t codec,
                              gaddr spatialQuality, gaddr temporalQuality, gaddr compressTime)
{
    if (!qt_venc_supported((uint32_t)cType, 24) && !qt_venc_supported((uint32_t)cType, 8))
        return noCodecErr;
    if (spatialQuality && rd32(spatialQuality) > 0x400)
        wr32(spatialQuality, 0x400);
    if (temporalQuality && !qt_venc_temporal((uint32_t)cType))
        wr32(temporalQuality, 0);
    if (compressTime)
        wr32(compressTime, 0); /* no time to speak of */
    return noErr;
}

/* ---- Sequences ---- */

#define MAX_CSEQS 8

typedef struct {
    bool used;
    qt_venc_format f;
    int depth_req;
    qrect rect;
    gaddr ctab;
    int32_t temporal, key_rate, frame, since_key;
    gaddr desc;
    uint32_t *prev;
    gaddr prev_gw;
} cseq;

static cseq cseqs[MAX_CSEQS];

static cseq *cseq_of(int32_t id)
{
    int i = id - CSEQ_BASE;
    return i >= 0 && i < MAX_CSEQS && cseqs[i].used ? &cseqs[i] : NULL;
}

static void cseq_free(cseq *s)
{
    free(s->prev);
    if (s->prev_gw)
        tb_DisposeGWorld(s->prev_gw);
    memset(s, 0, sizeof *s);
}

void icm_compress_reset(void)
{
    for (int i = 0; i < MAX_CSEQS; i++)
        free(cseqs[i].prev); /* the GWorlds went with the heap */
    memset(cseqs, 0, sizeof cseqs);
}

bool icm_csequence_exists(int32_t seqID) { return cseq_of(seqID) != NULL; }

bool icm_csequence_end(int32_t seqID)
{
    cseq *s = cseq_of(seqID);
    if (!s)
        return false;
    cseq_free(s);
    return true;
}

int16_t tb_CompressSequenceBegin(gaddr seqID, int32_t src, int32_t prev, gaddr srcRect, gaddr prevRect,
                                 int16_t colorDepth, int32_t cType, int32_t codec, int32_t spatialQuality,
                                 int32_t temporalQuality, int32_t keyFrameRate, int32_t ctable, int16_t flags,
                                 int32_t desc)
{
    if (seqID)
        wr32(seqID, 0);
    source s;
    bool have = source_of((gaddr)src, srcRect, &s);
    qrect r = have ? s.r : srcRect ? qrect_get(srcRect) : (qrect){ 0, 0, 0, 0 };
    if (!seqID || qrect_empty(r))
        return paramErr;
    int depth = pick_depth((uint32_t)cType, colorDepth, have ? s.d.depth : 32);
    if (!depth)
        return noCodecErr;
    cseq *q = NULL;
    for (int i = 0; i < MAX_CSEQS && !q; i++)
        if (!cseqs[i].used)
            q = &cseqs[i];
    if (!q)
        return memFullErr;
    memset(q, 0, sizeof *q);
    q->f = (qt_venc_format){ (uint32_t)cType, width_of(r), height_of(r), depth, spatialQuality };
    q->depth_req = colorDepth;
    q->rect = r;
    q->ctab = pick_ctab(depth, have ? &s : NULL, (gaddr)ctable);
    q->temporal = qt_venc_temporal((uint32_t)cType) ? temporalQuality : 0;
    q->key_rate = keyFrameRate;
    q->desc = (gaddr)desc;
    if (desc && !write_desc((gaddr)desc, (uint32_t)cType, q->f.width, q->f.height, depth, q->ctab, spatialQuality,
                            q->temporal, 0))
        return memFullErr;
    q->used = true;
    wr32(seqID, (uint32_t)(CSEQ_BASE + (q - cseqs)));
    if (prev) /* a previous frame of the caller's own */
        tb_SetCSequencePrev((int32_t)rd32(seqID), prev, prevRect);
    return noErr;
}

/* Tell the caller's completion routine (ICMCompletionProcRecord) it's done. */
static void complete(gaddr record, int16_t err)
{
    if (!record || !rd32(record))
        return;
    uint32_t args[3] = { (uint16_t)err, 3 /* codecCompletionSource | codecCompletionDest */, rd32(record + 4) };
    mm_call(rd32(record), 0x00000E80 /* pascal (short, short, long) */, args, 3);
}

int16_t tb_CompressSequenceFrame(int32_t seqID, int32_t src, gaddr srcRect, int16_t flags, int32_t data,
                                 gaddr dataSize, gaddr similarity, int32_t asyncCompletionProc)
{
    cseq *q = cseq_of(seqID);
    if (!q)
        return paramErr;
    source s;
    gaddr tmp = 0;
    if (!srcRect) { /* the sequence's rectangle */
        tmp = tb_NewPtr(8, false, false);
        qrect_put(tmp, q->rect);
    }
    bool have = source_of((gaddr)src, srcRect ? srcRect : tmp, &s);
    if (tmp)
        tb_DisposePtr(tmp);
    if (!have || !data || width_of(s.r) != q->f.width || height_of(s.r) != q->f.height)
        return paramErr;
    uint32_t *px = read_values(&s, q->f.depth, q->ctab);
    if (!px)
        return memFullErr;
    bool key = !q->prev || !q->temporal || (flags & codecFlagForceKeyFrame) ||
               (q->key_rate > 0 && q->since_key >= q->key_rate);
    uint32_t size = 0;
    int16_t err = compress_values(&q->f, px, key ? NULL : q->prev, (gaddr)data, &size);
    if (!err) {
        size_t n = (size_t)q->f.width * (size_t)q->f.height, same = 0;
        if (!key)
            for (size_t i = 0; i < n; i++)
                same += px[i] == q->prev[i];
        if (dataSize)
            wr32(dataSize, size);
        if (similarity) /* 0 for a key frame, up to 255 for one unchanged */
            wr8(similarity, key ? 0 : (uint8_t)(1 + same * 254 / (n ? n : 1)));
        if (q->desc && rd32(q->desc) && tb_GetHandleSize(q->desc) >= 86)
            wr32(rd32(q->desc) + 44, size);
        free(q->prev);
        q->prev = px;
        q->since_key = key ? 1 : q->since_key + 1;
        q->frame++;
    } else {
        free(px);
    }
    complete((gaddr)asyncCompletionProc, err);
    return err;
}

int16_t tb_SetCSequencePrev(int32_t seqID, int32_t prev, gaddr prevRect)
{
    cseq *q = cseq_of(seqID);
    source s;
    if (!q || !source_of((gaddr)prev, prevRect, &s))
        return paramErr;
    if (width_of(s.r) != q->f.width || height_of(s.r) != q->f.height)
        return paramErr;
    uint32_t *px = read_values(&s, q->f.depth, q->ctab);
    if (!px)
        return memFullErr;
    free(q->prev);
    q->prev = px;
    return noErr;
}

/* The previous frame, in a GWorld of the sequence's (kept until it ends). */
int16_t tb_GetCSequencePrevBuffer(int32_t seqID, gaddr gworld)
{
    cseq *q = cseq_of(seqID);
    if (!q || !gworld || q->f.depth > 32)
        return paramErr;
    if (!q->prev_gw) {
        gaddr tmp = tb_NewPtr(12, false, true);
        qrect_put(tmp + 4, (qrect){ 0, 0, (int16_t)q->f.height, (int16_t)q->f.width });
        int16_t depth = (int16_t)(q->f.depth == 24 ? 32 : q->f.depth);
        int16_t err = tb_NewGWorld(tmp, depth, tmp + 4, q->ctab, 0, 0);
        q->prev_gw = err ? 0 : rd32(tmp);
        tb_DisposePtr(tmp);
        if (err)
            return err;
    }
    if (q->prev) {
        gaddr pm = rd32(tb_GetGWorldPixMap(q->prev_gw));
        pixdst d = { qd_pixmap_base(pm, true), (uint16_t)PixMap_rowBytes(pm) & 0x3FFF, qrect_get(pm + PixMap_bounds_off),
                     PixMap_pixelSize(pm), PixMap_pmTable(pm) };
        for (int y = 0; y < q->f.height; y++)
            for (int x = 0; x < q->f.width; x++) {
                uint32_t v = q->prev[(size_t)y * (size_t)q->f.width + (size_t)x];
                put_px(&d, x, y, d.depth == 16 ? v & 0x7FFF : v);
            }
    }
    wr32(gworld, q->prev_gw);
    return noErr;
}

int16_t tb_GetCSequenceMaxCompressionSize(int32_t seqID, int32_t src, gaddr size)
{
    cseq *q = cseq_of(seqID);
    if (!q || !size)
        return paramErr;
    wr32(size, (uint32_t)qt_venc_max_size(&q->f));
    return noErr;
}

int16_t tb_SetCSequenceQuality(int32_t seqID, int32_t spatialQuality, int32_t temporalQuality)
{
    cseq *q = cseq_of(seqID);
    if (!q)
        return paramErr;
    q->f.quality = spatialQuality;
    q->temporal = qt_venc_temporal(q->f.format) ? temporalQuality : 0;
    return noErr;
}

int16_t tb_SetCSequenceKeyFrameRate(int32_t seqID, int32_t keyFrameRate)
{
    cseq *q = cseq_of(seqID);
    if (!q)
        return paramErr;
    q->key_rate = keyFrameRate;
    return noErr;
}

int16_t tb_GetCSequenceKeyFrameRate(int32_t seqID, gaddr keyFrameRate)
{
    cseq *q = cseq_of(seqID);
    if (!q || !keyFrameRate)
        return paramErr;
    wr32(keyFrameRate, (uint32_t)q->key_rate);
    return noErr;
}

int16_t tb_GetCSequenceFrameNumber(int32_t seqID, gaddr frameNumber)
{
    cseq *q = cseq_of(seqID);
    if (!q || !frameNumber)
        return paramErr;
    wr32(frameNumber, (uint32_t)q->frame);
    return noErr;
}

int16_t tb_SetCSequenceFrameNumber(int32_t seqID, int32_t frameNumber)
{
    cseq *q = cseq_of(seqID);
    if (!q)
        return paramErr;
    q->frame = frameNumber;
    return noErr;
}

/* DataRateParams: dataRate, dataOverrun, frameDuration, keyFrameRate,
 * minSpatialQuality, minTemporalQuality.  No rate is kept to. */
int16_t tb_SetCSequenceDataRateParams(int32_t seqID, int32_t params) { return cseq_of(seqID) ? noErr : paramErr; }

int16_t tb_GetCSequenceDataRateParams(int32_t seqID, int32_t params)
{
    cseq *q = cseq_of(seqID);
    if (!q || !params)
        return paramErr;
    gmemset((gaddr)params, 0, 24);
    wr32((gaddr)params + 12, (uint32_t)q->key_rate);
    return noErr;
}

int16_t tb_SetCSequenceFlushProc(int32_t seqID, int32_t flushProc, int32_t bufferSize)
{
    return cseq_of(seqID) ? noErr : paramErr;
}

int16_t tb_SetCSequencePreferredPacketSize(int32_t seqID, int32_t preferredPacketSizeInBytes)
{
    return cseq_of(seqID) ? noErr : paramErr;
}

/* ---- Pictures ---- */

static void be16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void be32(uint8_t *p, uint32_t v)
{
    be16(p, v >> 16);
    be16(p + 2, v & 0xFFFF);
}

int16_t tb_FCompressPicture(int32_t srcPicture, int32_t dstPicture, int16_t colorDepth, int32_t ctable,
                            int32_t quality, int16_t doDither, int16_t compressAgain, int32_t progressProc,
                            int32_t cType, int32_t codec)
{
    gaddr pic = (gaddr)srcPicture, dst = (gaddr)dstPicture;
    if (!pic || !rd32(pic) || !dst)
        return paramErr;
    qrect frame = qrect_get(rd32(pic) + 2);
    int w = width_of(frame), h = height_of(frame);
    if (w <= 0 || h <= 0)
        return paramErr;
    int depth = pick_depth((uint32_t)cType, colorDepth, 24);
    if (!depth)
        return noCodecErr;

    /* Draw the picture into a 32-bit GWorld. */
    gaddr tmp = tb_NewPtr(24, false, true); /* gworld, rect, saved port and device */
    if (!tmp)
        return memFullErr;
    qrect_put(tmp + 4, (qrect){ 0, 0, (int16_t)h, (int16_t)w });
    int16_t err = tb_NewGWorld(tmp, 32, tmp + 4, 0, 0, 0);
    if (err) {
        tb_DisposePtr(tmp);
        return err;
    }
    gaddr gw = rd32(tmp);
    tb_GetGWorld(tmp + 12, tmp + 16);
    tb_SetGWorld(gw, 0);
    tb_LockPixels(tb_GetGWorldPixMap(gw));
    tb_EraseRect(tmp + 4);
    tb_DrawPicture(pic, tmp + 4);
    tb_SetGWorld(rd32(tmp + 12), rd32(tmp + 16));

    source s;
    uint32_t *px = NULL;
    gaddr ctab = 0;
    if (source_of(tb_GetGWorldPixMap(gw), tmp + 4, &s)) {
        ctab = pick_ctab(depth, NULL, (gaddr)ctable);
        px = read_values(&s, depth, ctab);
    }
    tb_DisposeGWorld(gw);
    tb_DisposePtr(tmp);
    if (!px)
        return memFullErr;
    qt_venc_format f = { (uint32_t)cType, w, h, depth, quality };
    uint8_t *data = NULL;
    size_t len = qt_venc_encode(&f, px, NULL, &data);
    free(px);
    if (!len)
        return codecErr;
    gaddr desc = write_desc(0, (uint32_t)cType, w, h, depth, ctab, quality, 0, (uint32_t)len);
    if (!desc) {
        free(data);
        return memFullErr;
    }
    uint32_t dsize = rd32(rd32(desc));

    /* The picture: header, $8200, end. */
    uint32_t body = 66 + dsize + (uint32_t)len;
    uint32_t total = 10 + 4 + 2 + 24 + 2 + 4 + body + (body & 1) + 2;
    tb_SetHandleSize(dst, (int32_t)total);
    if ((uint32_t)tb_GetHandleSize(dst) < total) {
        tb_DisposeHandle(desc);
        free(data);
        return memFullErr;
    }
    uint8_t *p = calloc(1, total);
    if (!p) {
        tb_DisposeHandle(desc);
        free(data);
        return memFullErr;
    }
    be16(p, total & 0xFFFF);
    be16(p + 2, (uint16_t)frame.top);
    be16(p + 4, (uint16_t)frame.left);
    be16(p + 6, (uint16_t)frame.bottom);
    be16(p + 8, (uint16_t)frame.right);
    be32(p + 10, 0x001102FF);
    uint8_t *q = p + 14;
    be16(q, 0x0C00); /* extended version 2: version -2, hRes, vRes, the frame */
    be16(q + 2, 0xFFFE);
    be32(q + 6, 72u << 16);
    be32(q + 10, 72u << 16);
    be16(q + 14, (uint16_t)frame.top);
    be16(q + 16, (uint16_t)frame.left);
    be16(q + 18, (uint16_t)frame.bottom);
    be16(q + 20, (uint16_t)frame.right);
    q += 26;
    be16(q, 0x8200);
    be32(q + 2, body);
    uint8_t *b = q + 6;
    be32(b, 0x10000); /* the matrix: the image at the frame's corner */
    be32(b + 16, 0x10000);
    be32(b + 24, (uint32_t)(int32_t)frame.left << 16);
    be32(b + 28, (uint32_t)(int32_t)frame.top << 16);
    be32(b + 32, 0x40000000);
    /* matteSize, matteRect and mode (srcCopy) are 0 */
    be16(b + 54, (uint16_t)h); /* srcRect */
    be16(b + 56, (uint16_t)w);
    /* accuracy and maskSize are 0 */
    gget(b + 66, rd32(desc), dsize);
    memcpy(b + 66 + dsize, data, len);
    be16(b + body + (body & 1), 0x00FF);
    gput(rd32(dst), p, total);
    free(p);
    free(data);
    tb_DisposeHandle(desc);
    return noErr;
}

int16_t tb_CompressPicture(int32_t srcPicture, int32_t dstPicture, int32_t quality, int32_t cType)
{
    return tb_FCompressPicture(srcPicture, dstPicture, 0, 0, quality, 0, 0, 0, cType, 0);
}

/* ---- Descriptions and the list of compressors ---- */

int16_t tb_MakeImageDescriptionForPixMap(int32_t pixmap, gaddr idh)
{
    source s;
    if (!idh || !source_of((gaddr)pixmap, 0, &s))
        return paramErr;
    gaddr h = write_desc(0, RAW, width_of(s.r), height_of(s.r), s.d.depth, s.d.ctab, 0, 0,
                         s.d.row_bytes * (uint32_t)height_of(s.r));
    if (!h)
        return memFullErr;
    wr32(idh, h);
    return noErr;
}

/* CodecNameSpecList: a count, then per codec its component, type, type
 * name (Str31) and a handle to its name; 44 bytes each, from offset 2.
 * Every compressor registered, ours and programs'. */
int16_t tb_GetCodecNameList(gaddr list, int16_t showAll)
{
    if (!list)
        return paramErr;
    uint32_t comps[64];
    int n = 0;
    for (uint32_t c = components_find_next(0, fourcc("imco"), 0, 0); c && n < 64;
         c = components_find_next(c, fourcc("imco"), 0, 0))
        comps[n++] = c;
    gaddr p = tb_NewPtr(2 + n * 44, false, true);
    if (!p)
        return memFullErr;
    wr16(p, (uint16_t)n);
    gaddr e = p + 2;
    for (int i = 0; i < n; i++) {
        uint32_t t, type = 0, m;
        components_describe(comps[i], &t, &type, &m);
        const char *name = qt_codec_type_name(type);
        char own[5] = { (char)(type >> 24), (char)(type >> 16), (char)(type >> 8), (char)type, 0 };
        if (!name)
            name = own;
        uint32_t len = (uint32_t)strlen(name);
        wr32(e, comps[i]);
        wr32(e + 4, type);
        wr8(e + 8, (uint8_t)len);
        gput(e + 9, name, len);
        gaddr h = tb_NewHandle((int32_t)len + 1, false, true);
        if (h) {
            wr8(rd32(h), (uint8_t)len);
            gput(rd32(h) + 1, name, len);
        }
        wr32(e + 40, h);
        e += 44;
    }
    wr32(list, p);
    return noErr;
}

int16_t tb_DisposeCodecNameList(int32_t list)
{
    gaddr p = (gaddr)list;
    if (!p)
        return noErr;
    int n = (int16_t)rd16(p);
    for (int i = 0; i < n; i++)
        if (rd32(p + 2 + (uint32_t)i * 44 + 40))
            tb_DisposeHandle(rd32(p + 2 + (uint32_t)i * 44 + 40));
    tb_DisposePtr(p);
    return noErr;
}
