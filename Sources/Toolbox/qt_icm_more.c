/*
 * The rest of the Image Compression Manager: image description extensions
 * and colour tables, similarity, trimming and converting images,
 * transcoders, picture files, thumbnails, StdPix and compressed pixel
 * maps, pixel format information, QuadToQuadMatrix, and the sequences'
 * remaining calls: their memory, data sources and information.
 *
 * Effects: a decompression sequence for an effect type ('dslv', 'smpt',
 * 'blur', ...) has no codec; each frame is made from its data sources
 * ('srcA', 'srcB': an image description and data each) at the point the
 * frame time gives in the effect (its virtual start and duration), as
 * QuickTime 3's effects components did.  Filters take one source,
 * transitions two.  Their parameters (the frame's atom container) are
 * taken at their defaults.  Not done: the generators (fire, clouds,
 * ripple), chroma key, implode, explode, gradient wipes and travelling
 * mattes; their sequences fail with noCodecErr as on a Mac without them.
 *
 * Image fields (ImageFieldSequence*) need a codec that splits fields, which
 * none of ours does: they answer noCodecErr, as QuickTime did for codecs
 * other than Motion JPEG's.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "qd_internal.h"
#include "qt_atoms.h"
#include "qt_codec.h"
#include "qt_errors.h"
#include "qt_movie_internal.h"
#include "tb_internal.h"
#include "trap.h"

enum { codecExtensionNotFound = -8971 };

/* Guest bytes moved (the ranges may overlap). */
static void gmemmove(gaddr dst, gaddr src, uint32_t n)
{
    uint8_t *b = malloc(n ? n : 1);
    if (!b)
        return;
    gget(b, src, n);
    gput(dst, b, n);
    free(b);
}

/* A handle's size set; false if it couldn't be. */
static bool resize(gaddr h, uint32_t n)
{
    tb_SetHandleSize(h, (int32_t)n);
    return (uint32_t)tb_GetHandleSize(h) == n;
}

static gaddr desc_ptr(int32_t h) { return h && rd32((gaddr)h) ? rd32((gaddr)h) : 0; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- Pixels in and out of GWorlds ---- */

/* A 32-bit GWorld of the pixels; 0 if it can't be made. */
static gaddr gworld_of(const uint8_t *rgba, int w, int h)
{
    gaddr out = tb_NewPtr(4, false, true), r = tb_NewPtr(8, false, true);
    qrect_put(r, (qrect){ 0, 0, (int16_t)h, (int16_t)w });
    gaddr gw = tb_NewGWorld(out, 32, r, 0, 0, 0) == noErr ? rd32(out) : 0;
    if (gw && rgba) {
        gaddr pm = rd32(tb_GetGWorldPixMap(gw));
        tb_LockPixels(tb_GetGWorldPixMap(gw));
        qt_codec_frame_to_pixmap(rgba, w, h, pm, (qrect){ 0, 0, (int16_t)h, (int16_t)w });
    }
    tb_DisposePtr(out);
    tb_DisposePtr(r);
    return gw;
}

/* A picture drawn at w by h (RGBA, malloc'd). */
static uint8_t *picture_rgba(gaddr pic, int w, int h)
{
    gaddr gw = gworld_of(NULL, w, h);
    if (!gw)
        return NULL;
    gaddr saved = tb_NewPtr(8, false, true), r = tb_NewPtr(8, false, true);
    tb_GetGWorld(saved, saved + 4);
    tb_SetGWorld(gw, 0);
    qrect_put(r, (qrect){ 0, 0, (int16_t)h, (int16_t)w });
    tb_EraseRect(r);
    tb_DrawPicture(pic, r);
    tb_SetGWorld(rd32(saved), rd32(saved + 4));
    uint8_t *rgba = qt_codec_pixmap_to_rgba(rd32(tb_GetGWorldPixMap(gw)), (qrect){ 0, 0, (int16_t)h, (int16_t)w });
    tb_DisposeGWorld(gw);
    tb_DisposePtr(saved);
    tb_DisposePtr(r);
    return rgba;
}

/* Pixels compressed with a codec into a description and data (the
 * description handle given, or a new one in *descOut). */
static int16_t compress_rgba(const uint8_t *rgba, int w, int h, int16_t depth, gaddr ctable, int32_t quality,
                             uint32_t type, uint32_t codec, gaddr desc, gaddr data)
{
    gaddr gw = gworld_of(rgba, w, h);
    if (!gw)
        return memFullErr;
    gaddr r = tb_NewPtr(8, false, true);
    qrect_put(r, (qrect){ 0, 0, (int16_t)h, (int16_t)w });
    int16_t err = tb_FCompressImage((int32_t)tb_GetGWorldPixMap(gw), r, depth, quality, (int32_t)type, (int32_t)codec,
                                    (int32_t)ctable, 0, 0, 0, 0, (int32_t)desc, (int32_t)data);
    tb_DisposePtr(r);
    tb_DisposeGWorld(gw);
    return err;
}

/* The most bytes a w by h image takes in a codec. */
static int32_t max_size(uint32_t type, int w, int h, int16_t depth)
{
    qt_venc_format f = { type, w, h, depth ? depth : 24, 0x200 };
    size_t n = qt_venc_supported(type, f.depth) ? qt_venc_max_size(&f) : 0;
    return n ? (int32_t)n : w * h * 4 + 1024;
}

/* ---- Image descriptions ---- */

/* The offset of a description's extensions: after the colour table, if any. */
static uint32_t ext_start(gaddr d)
{
    int depth = (int16_t)rd16(d + 82);
    if ((int16_t)rd16(d + 84) == 0 && depth >= 1 && depth <= 8 && rd32(d) >= 94)
        return 86 + 8 + ((uint32_t)rd16(d + 86 + 6) + 1) * 8;
    return 86;
}

/* The index'th (1-based) extension of a type (0: any): its offset, or 0. */
static uint32_t find_ext(gaddr d, uint32_t type, int index)
{
    uint32_t n = rd32(d), at = ext_start(d);
    int k = 0;
    while (at + 8 <= n) {
        uint32_t sz = rd32(d + at);
        if (sz < 8 || at + sz > n)
            break;
        if ((!type || rd32(d + at + 4) == type) && ++k == index)
            return at;
        at += sz;
    }
    return 0;
}

int16_t tb_GetImageDescriptionExtension(int32_t desc, gaddr extension, int32_t idType, int32_t index)
{
    gaddr d = desc_ptr(desc);
    if (!d || !extension)
        return paramErr;
    wr32(extension, 0);
    uint32_t at = find_ext(d, (uint32_t)idType, index ? index : 1);
    if (!at)
        return codecExtensionNotFound;
    uint32_t n = rd32(d + at) - 8;
    gaddr h = tb_NewHandle((int32_t)n, false, false);
    if (!h)
        return memFullErr;
    gmemmove(rd32(h), rd32((gaddr)desc) + at + 8, n);
    wr32(extension, h);
    return noErr;
}

int16_t tb_AddImageDescriptionExtension(int32_t desc, int32_t extension, int32_t idType)
{
    gaddr h = (gaddr)desc;
    if (!desc_ptr(desc) || !extension || !rd32((gaddr)extension))
        return paramErr;
    uint32_t n = rd32(rd32(h)), add = (uint32_t)tb_GetHandleSize((gaddr)extension);
    if (!resize(h, n + 8 + add))
        return memFullErr;
    gaddr d = rd32(h);
    wr32(d + n, 8 + add);
    wr32(d + n + 4, (uint32_t)idType);
    gmemmove(d + n + 8, rd32((gaddr)extension), add);
    wr32(d, n + 8 + add);
    return noErr;
}

int16_t tb_RemoveImageDescriptionExtension(int32_t desc, int32_t idType, int32_t index)
{
    gaddr h = (gaddr)desc;
    if (!desc_ptr(desc))
        return paramErr;
    bool any = false;
    for (;;) { /* index 0: every one of the type */
        gaddr d = rd32(h);
        uint32_t at = find_ext(d, (uint32_t)idType, index ? index : 1);
        if (!at)
            break;
        uint32_t n = rd32(d), sz = rd32(d + at);
        gmemmove(d + at, d + at + sz, n - at - sz);
        wr32(d, n - sz);
        tb_SetHandleSize(h, (int32_t)(n - sz));
        any = true;
        if (index)
            break;
    }
    return any ? noErr : codecExtensionNotFound;
}

int16_t tb_CountImageDescriptionExtensionType(int32_t desc, int32_t idType, gaddr count)
{
    gaddr d = desc_ptr(desc);
    if (!d || !count)
        return paramErr;
    int k = 0;
    while (find_ext(d, (uint32_t)idType, k + 1))
        k++;
    wr32(count, (uint32_t)k);
    return noErr;
}

/* The type of the extension after the first of *idType (0: the first). */
int16_t tb_GetNextImageDescriptionExtensionType(int32_t desc, gaddr idType)
{
    gaddr d = desc_ptr(desc);
    if (!d || !idType)
        return paramErr;
    uint32_t want = rd32(idType), at = want ? find_ext(d, want, 1) : 0;
    uint32_t next = at ? at + rd32(d + at) : ext_start(d);
    wr32(idType, next + 8 <= rd32(d) ? rd32(d + next + 4) : 0);
    return noErr;
}

/* A colour table put in (or taken out of) a description, its extensions kept. */
int16_t tb_SetImageDescriptionCTable(int32_t desc, int32_t ctable)
{
    gaddr h = (gaddr)desc, d = desc_ptr(desc);
    if (!d)
        return paramErr;
    uint32_t n = rd32(d), start = ext_start(d), extn = n > start ? n - start : 0;
    uint8_t *ext = malloc(extn ? extn : 1);
    gget(ext, d + start, extn);
    uint32_t ctn = 0;
    gaddr ct = ctable ? rd32((gaddr)ctable) : 0;
    if (ct)
        ctn = 8 + ((uint32_t)rd16(ct + 6) + 1) * 8;
    uint32_t total = 86 + ctn + extn;
    if (!resize(h, total)) {
        free(ext);
        return memFullErr;
    }
    d = rd32(h);
    if (ct)
        gmemmove(d + 86, ct, ctn);
    gput(d + 86 + ctn, ext, extn);
    free(ext);
    wr32(d, total);
    wr16(d + 84, ct ? 0 : 0xFFFF);
    return noErr;
}

/* ---- Images ---- */

int16_t tb_GetSimilarity(int32_t src, gaddr srcRect, int32_t desc, int32_t data, gaddr similarity)
{
    if (!src || !rd32((gaddr)src) || !similarity)
        return paramErr;
    int w, h;
    int16_t err;
    uint8_t *img = icm_decode(desc_ptr(desc), (gaddr)data, 0, &w, &h, &err);
    if (!img)
        return err;
    gaddr pm = rd32((gaddr)src);
    qrect r = srcRect ? qrect_get(srcRect) : qrect_get(pm + 6);
    uint8_t *s = qt_codec_pixmap_to_rgba(pm, r);
    int sw = r.right - r.left, sh = r.bottom - r.top;
    double diff = 0;
    long n = 0;
    for (int y = 0; s && y < sh; y++)
        for (int x = 0; x < sw; x++) {
            const uint8_t *a = s + ((size_t)y * (size_t)sw + (size_t)x) * 4;
            const uint8_t *b = img + ((size_t)(y * h / sh) * (size_t)w + (size_t)(x * w / sw)) * 4;
            diff += abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]);
            n += 3;
        }
    free(s);
    free(img);
    /* Fixed: 1 the same, 0 nothing alike */
    wr32(similarity, (uint32_t)(n ? (1.0 - diff / (n * 255.0)) * 65536.0 : 0));
    return noErr;
}

/* An image trimmed to a rectangle (its own coordinates) and compressed
 * again with its codec; the rectangle given back is what was kept. */
int16_t tb_TrimImage(int32_t desc, int32_t inData, int32_t inBufferSize, int32_t dataProc, int32_t outData,
                     int32_t outBufferSize, int32_t flushProc, gaddr trimRect, int32_t progressProc)
{
    gaddr d = desc_ptr(desc);
    if (!d || !inData || !outData || !trimRect)
        return paramErr;
    uint32_t type = rd32(d + 4);
    if (!qt_venc_supported(type, 24) && !qt_venc_supported(type, (int16_t)rd16(d + 82)))
        return codecUnimpErr;
    int w, h;
    int16_t err;
    uint8_t *img = icm_decode(d, (gaddr)inData, inBufferSize, &w, &h, &err);
    if (!img)
        return err;
    qrect r = qrect_get(trimRect);
    r.left = (int16_t)clampi(r.left, 0, w);
    r.right = (int16_t)clampi(r.right, r.left, w);
    r.top = (int16_t)clampi(r.top, 0, h);
    r.bottom = (int16_t)clampi(r.bottom, r.top, h);
    int tw = r.right - r.left, th = r.bottom - r.top;
    if (tw <= 0 || th <= 0) {
        free(img);
        return paramErr;
    }
    uint8_t *part = malloc((size_t)tw * (size_t)th * 4);
    for (int y = 0; y < th; y++)
        memcpy(part + (size_t)y * (size_t)tw * 4, img + ((size_t)(r.top + y) * (size_t)w + (size_t)r.left) * 4,
               (size_t)tw * 4);
    free(img);
    gaddr buf = tb_NewPtr(max_size(type, tw, th, 32), false, false);
    gaddr tmp = tb_NewHandle(0, false, false);
    err = buf && tmp ? compress_rgba(part, tw, th, (int16_t)rd16(d + 82), 0, (int32_t)rd32(d + 28), type, 0, tmp, buf)
                     : memFullErr;
    free(part);
    if (!err) {
        uint32_t n = rd32(rd32(tmp) + 44);
        if (outBufferSize > 0 && n > (uint32_t)outBufferSize) {
            err = codecSizeErr;
        } else {
            gmemmove((gaddr)outData, buf, n);
            d = desc_ptr(desc);
            wr16(d + 32, (uint16_t)tw);
            wr16(d + 34, (uint16_t)th);
            wr32(d + 44, n);
            qrect_put(trimRect, r);
        }
    }
    if (buf)
        tb_DisposePtr(buf);
    if (tmp)
        tb_DisposeHandle(tmp);
    return err;
}

/* An image in one codec made into another (the destination description
 * filled in, its data at dstData). */
int16_t tb_ConvertImage(int32_t srcDD, int32_t srcData, int16_t colorDepth, int32_t ctable, int32_t accuracy,
                        int32_t quality, int32_t cType, int32_t codec, int32_t dstDD, int32_t dstData)
{
    if (!desc_ptr(srcDD) || !srcData || !dstDD || !dstData)
        return paramErr;
    int w, h;
    int16_t err;
    uint8_t *img = icm_decode(desc_ptr(srcDD), (gaddr)srcData, 0, &w, &h, &err);
    if (!img)
        return err;
    err = compress_rgba(img, w, h, colorDepth, (gaddr)ctable, quality, (uint32_t)cType, (uint32_t)codec,
                        (gaddr)dstDD, (gaddr)dstData);
    free(img);
    return err;
}

/* ---- Transcoders: an image sequence from one codec to another ---- */

#define MAX_TRANSCODERS 8
static struct {
    bool used;
    gaddr src_desc, dst_desc; /* copies of the source's, and ours */
    uint32_t dst_type;
} transcoders[MAX_TRANSCODERS];

int16_t tb_ImageTranscodeSequenceBegin(gaddr its, int32_t srcDesc, int32_t destType, gaddr dstDesc, gaddr data,
                                       int32_t dataSize)
{
    if (!its || !desc_ptr(srcDesc))
        return paramErr;
    wr32(its, 0);
    gaddr s = desc_ptr(srcDesc);
    uint32_t st = rd32(s + 4);
    if (!qt_codec_decodes(st) || !qt_venc_supported((uint32_t)destType, 24))
        return noCodecErr; /* no transcoder between these */
    int i = 0;
    while (i < MAX_TRANSCODERS && transcoders[i].used)
        i++;
    if (i == MAX_TRANSCODERS)
        return memFullErr;
    gaddr copy = tb_NewHandle((int32_t)rd32(s), false, false), out = tb_NewHandle(86, false, true);
    if (!copy || !out)
        return memFullErr;
    gmemmove(rd32(copy), s, rd32(s));
    /* the destination's description: the frame size, made right by the first frame */
    gaddr o = rd32(out);
    wr32(o, 86);
    wr32(o + 4, (uint32_t)destType);
    wr32(o + 20, fourcc("appl"));
    gmemmove(o + 32, s + 32, 4);
    wr32(o + 36, 72u << 16);
    wr32(o + 40, 72u << 16);
    wr16(o + 48, 1);
    wr16(o + 82, 24);
    wr16(o + 84, 0xFFFF);
    transcoders[i].used = true;
    transcoders[i].src_desc = copy;
    transcoders[i].dst_desc = out;
    transcoders[i].dst_type = (uint32_t)destType;
    if (dstDesc)
        wr32(dstDesc, out);
    wr32(its, 0x54430001u + (uint32_t)i); /* 'TC' and the slot */
    return noErr;
}

static int transcoder(int32_t its)
{
    uint32_t i = (uint32_t)its - 0x54430001u;
    return i < MAX_TRANSCODERS && transcoders[i].used ? (int)i : -1;
}

int16_t tb_ImageTranscodeFrame(int32_t its, gaddr srcData, int32_t srcDataSize, gaddr dstData, gaddr dstDataSize)
{
    int i = transcoder(its);
    if (i < 0 || !srcData || !dstData)
        return paramErr;
    int w, h;
    int16_t err;
    uint8_t *img = icm_decode(rd32(transcoders[i].src_desc), srcData, srcDataSize, &w, &h, &err);
    if (!img)
        return err;
    gaddr buf = tb_NewPtr(max_size(transcoders[i].dst_type, w, h, 24), false, false);
    err = buf ? compress_rgba(img, w, h, 0, 0, 0x200, transcoders[i].dst_type, 0, transcoders[i].dst_desc, buf)
              : memFullErr;
    free(img);
    if (err) {
        if (buf)
            tb_DisposePtr(buf);
        return err;
    }
    wr32(dstData, buf);
    if (dstDataSize)
        wr32(dstDataSize, rd32(rd32(transcoders[i].dst_desc) + 44));
    return noErr;
}

int16_t tb_ImageTranscodeDisposeFrameData(int32_t its, gaddr dstData)
{
    if (transcoder(its) < 0)
        return paramErr;
    if (dstData)
        tb_DisposePtr(dstData);
    return noErr;
}

int16_t tb_ImageTranscodeSequenceEnd(int32_t its)
{
    int i = transcoder(its);
    if (i < 0)
        return paramErr;
    tb_DisposeHandle(transcoders[i].src_desc);
    transcoders[i].used = false; /* the destination description is the caller's */
    return noErr;
}

/* Field operations: no codec of ours splits fields. */
int16_t tb_ImageFieldSequenceBegin(gaddr ifs, int32_t desc1, int32_t desc2, int32_t descOut)
{
    if (ifs)
        wr32(ifs, 0);
    return noCodecErr;
}
int16_t tb_ImageFieldSequenceExtractCombine(int32_t ifs, int32_t fieldFlags, gaddr data1, int32_t dataSize1,
                                            gaddr data2, int32_t dataSize2, gaddr outputData, gaddr outDataSize)
{
    return paramErr;
}
int16_t tb_ImageFieldSequenceEnd(int32_t ifs) { return paramErr; }

/* ---- Picture files ---- */

/* A file's data fork from offset 512 (a PICT file's picture) in a handle. */
static gaddr read_picture_file(int16_t ref)
{
    gaddr pb = tb_NewPtr(80, false, true);
    wr16(pb + 24, (uint16_t)ref);
    gaddr h = 0;
    if (tb_PBGetEOF(pb, false) == noErr) {
        int32_t eof = (int32_t)rd32(pb + 28);
        if (eof > 512 + 10) {
            h = tb_NewHandle(eof - 512, false, false);
            if (h) {
                wr16(pb + 24, (uint16_t)ref);
                wr32(pb + 32, rd32(h));
                wr32(pb + 36, (uint32_t)(eof - 512));
                wr16(pb + 44, 1); /* fsFromStart */
                wr32(pb + 46, 512);
                if (tb_PBRead(pb, false) != noErr && (int32_t)rd32(pb + 40) != eof - 512) {
                    tb_DisposeHandle(h);
                    h = 0;
                }
            }
        }
    }
    tb_DisposePtr(pb);
    return h;
}

/* A picture written as a PICT file: 512 bytes of zeros, then the picture. */
static int16_t write_picture_file(int16_t ref, gaddr pic)
{
    uint32_t n = (uint32_t)tb_GetHandleSize(pic);
    gaddr pb = tb_NewPtr(80, false, true), zeros = tb_NewPtr(512, false, true);
    wr16(pb + 24, (uint16_t)ref);
    wr32(pb + 28, 0);
    tb_PBSetEOF(pb, false);
    wr32(pb + 32, zeros);
    wr32(pb + 36, 512);
    wr16(pb + 44, 1);
    wr32(pb + 46, 0);
    int16_t err = tb_PBWrite(pb, false);
    if (!err) {
        tb_HLock(pic);
        wr32(pb + 32, rd32(pic));
        wr32(pb + 36, n);
        wr16(pb + 44, 1);
        wr32(pb + 46, 512);
        err = tb_PBWrite(pb, false);
        tb_HUnlock(pic);
    }
    tb_DisposePtr(pb);
    tb_DisposePtr(zeros);
    return err;
}

int16_t tb_FCompressPictureFile(int16_t srcRefNum, int16_t dstRefNum, int16_t colorDepth, int32_t ctable,
                                int32_t quality, int16_t doDither, int16_t compressAgain, int32_t progressProc,
                                int32_t cType, int32_t codec)
{
    gaddr src = read_picture_file(srcRefNum);
    if (!src)
        return paramErr;
    gaddr dst = tb_NewHandle(0, false, false);
    int16_t err = tb_FCompressPicture((int32_t)src, (int32_t)dst, colorDepth, ctable, quality, doDither, compressAgain,
                                      progressProc, cType, codec);
    if (!err)
        err = write_picture_file(dstRefNum, dst);
    tb_DisposeHandle(src);
    tb_DisposeHandle(dst);
    return err;
}

int16_t tb_CompressPictureFile(int16_t srcRefNum, int16_t dstRefNum, int32_t quality, int32_t cType)
{
    return tb_FCompressPictureFile(srcRefNum, dstRefNum, 0, 0, quality, 0, 0, 0, cType, 0);
}

/* OpenCPicParams: srcRect, hRes, vRes, version, reserved1, reserved2. */
int16_t tb_GetPictureFileHeader(int16_t refNum, gaddr frame, gaddr header)
{
    gaddr pic = read_picture_file(refNum);
    if (!pic)
        return paramErr;
    gaddr p = rd32(pic);
    if (frame)
        gmemmove(frame, p + 2, 8);
    if (header) {
        gmemset(header, 0, 24);
        gmemmove(header, p + 2, 8);
        wr32(header + 8, 72u << 16);
        wr32(header + 12, 72u << 16);
        bool v2 = rd16(p + 10) == 0x0011 && rd16(p + 12) == 0x02FF;
        wr16(header + 16, v2 ? 0xFFFE : 1);
        if (v2 && rd16(p + 14) == 0x0C00 && (int16_t)rd16(p + 16) == -2) { /* the extended header's resolution */
            wr32(header + 8, rd32(p + 20));
            wr32(header + 12, rd32(p + 24));
            gmemmove(header, p + 28, 8);
        }
    }
    tb_DisposeHandle(pic);
    return noErr;
}

int16_t tb_DrawTrimmedPicture(int32_t srcPicture, gaddr frame, int32_t trimMask, int16_t doDither,
                              int32_t progressProc)
{
    if (!srcPicture || !frame)
        return paramErr;
    gaddr saved = 0;
    if (trimMask) { /* the mask as part of the clip */
        saved = tb_NewRgn();
        tb_GetClip(saved);
        gaddr clip = tb_NewRgn();
        tb_SectRgn(saved, (gaddr)trimMask, clip);
        tb_SetClip(clip);
        tb_DisposeRgn(clip);
    }
    tb_DrawPicture((gaddr)srcPicture, frame);
    if (saved) {
        tb_SetClip(saved);
        tb_DisposeRgn(saved);
    }
    return noErr;
}

int16_t tb_DrawTrimmedPictureFile(int16_t srcRefnum, gaddr frame, int32_t trimMask, int16_t doDither,
                                  int32_t progressProc)
{
    gaddr pic = read_picture_file(srcRefnum);
    if (!pic)
        return paramErr;
    gaddr r = frame;
    if (!r) {
        r = tb_NewPtr(8, false, false);
        gmemmove(r, rd32(pic) + 2, 8);
    }
    int16_t err = tb_DrawTrimmedPicture((int32_t)pic, r, trimMask, doDither, progressProc);
    if (r != frame)
        tb_DisposePtr(r);
    tb_DisposeHandle(pic);
    return err;
}

int16_t tb_DrawPictureFile(int16_t refNum, gaddr frame, int32_t progressProc)
{
    return tb_DrawTrimmedPictureFile(refNum, frame, 0, 0, progressProc);
}

/* ---- Thumbnails: 80 by 80 at most, the aspect kept ---- */

static int16_t thumbnail_of(const uint8_t *rgba, int w, int h, int16_t depth, int32_t thumbnail)
{
    if (!thumbnail)
        return paramErr;
    int tw = 80, th = 80;
    if (w > h)
        th = clampi(80 * h / w, 1, 80);
    else
        tw = clampi(80 * w / h, 1, 80);
    uint8_t *small = malloc((size_t)tw * (size_t)th * 4);
    if (!small)
        return memFullErr;
    for (int y = 0; y < th; y++) /* averaging the pixels each covers */
        for (int x = 0; x < tw; x++) {
            int x0 = x * w / tw, x1 = (x + 1) * w / tw, y0 = y * h / th, y1 = (y + 1) * h / th;
            if (x1 <= x0)
                x1 = x0 + 1;
            if (y1 <= y0)
                y1 = y0 + 1;
            unsigned sum[3] = { 0 }, n = 0;
            for (int yy = y0; yy < y1 && yy < h; yy++)
                for (int xx = x0; xx < x1 && xx < w; xx++, n++)
                    for (int c = 0; c < 3; c++)
                        sum[c] += rgba[((size_t)yy * (size_t)w + (size_t)xx) * 4 + (size_t)c];
            uint8_t *o = small + ((size_t)y * (size_t)tw + (size_t)x) * 4;
            for (int c = 0; c < 3; c++)
                o[c] = (uint8_t)(n ? sum[c] / n : 0);
            o[3] = 255;
        }
    gaddr pic = qt_picture_of_rgba(small, tw, th);
    free(small);
    if (!pic)
        return memFullErr;
    (void)depth; /* drawn at the depth of the port it goes to */
    int32_t n = tb_GetHandleSize(pic);
    if (!resize((gaddr)thumbnail, (uint32_t)n)) {
        tb_KillPicture(pic);
        return memFullErr;
    }
    gmemmove(rd32((gaddr)thumbnail), rd32(pic), (uint32_t)n);
    tb_KillPicture(pic);
    return noErr;
}

static int16_t thumbnail_of_picture(gaddr pic, int16_t depth, int32_t thumbnail)
{
    qrect f = qrect_get(rd32(pic) + 2);
    int w = f.right - f.left, h = f.bottom - f.top;
    if (w <= 0 || h <= 0)
        return paramErr;
    /* drawn no bigger than needed for the thumbnail's averaging */
    int dw = w > 320 ? 320 : w, dh = w > 320 ? h * 320 / w : h;
    uint8_t *rgba = picture_rgba(pic, dw, dh > 0 ? dh : 1);
    if (!rgba)
        return memFullErr;
    int16_t err = thumbnail_of(rgba, dw, dh > 0 ? dh : 1, depth, thumbnail);
    free(rgba);
    return err;
}

int16_t tb_MakeThumbnailFromPicture(int32_t picture, int16_t colorDepth, int32_t thumbnail, int32_t progressProc)
{
    if (!picture || !rd32((gaddr)picture))
        return paramErr;
    return thumbnail_of_picture((gaddr)picture, colorDepth, thumbnail);
}

int16_t tb_MakeThumbnailFromPictureFile(int16_t refNum, int16_t colorDepth, int32_t thumbnail, int32_t progressProc)
{
    gaddr pic = read_picture_file(refNum);
    if (!pic)
        return paramErr;
    int16_t err = thumbnail_of_picture(pic, colorDepth, thumbnail);
    tb_DisposeHandle(pic);
    return err;
}

int16_t tb_MakeThumbnailFromPixMap(int32_t src, gaddr srcRect, int16_t colorDepth, int32_t thumbnail,
                                   int32_t progressProc)
{
    if (!src || !rd32((gaddr)src))
        return paramErr;
    gaddr pm = rd32((gaddr)src);
    qrect r = srcRect ? qrect_get(srcRect) : qrect_get(pm + 6);
    uint8_t *rgba = qt_codec_pixmap_to_rgba(pm, r);
    if (!rgba)
        return paramErr;
    int16_t err = thumbnail_of(rgba, r.right - r.left, r.bottom - r.top, colorDepth, thumbnail);
    free(rgba);
    return err;
}

/* ---- Compressed pixel maps and StdPix ---- */

#define MAX_CPIX 16
static struct {
    gaddr pix;
    int32_t desc, data, size, data_proc, progress_proc;
} cpix[MAX_CPIX];

int16_t tb_SetCompressedPixMapInfo(int32_t pix, int32_t desc, int32_t data, int32_t bufferSize, int32_t dataProc,
                                   int32_t progressProc)
{
    if (!pix)
        return paramErr;
    int free_slot = -1;
    for (int i = 0; i < MAX_CPIX; i++) {
        if (cpix[i].pix == (gaddr)pix) {
            free_slot = i;
            break;
        }
        if (!cpix[i].pix && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0)
        return memFullErr;
    cpix[free_slot].pix = (gaddr)pix;
    cpix[free_slot].desc = desc;
    cpix[free_slot].data = data;
    cpix[free_slot].size = bufferSize;
    cpix[free_slot].data_proc = dataProc;
    cpix[free_slot].progress_proc = progressProc;
    return noErr;
}

int16_t tb_GetCompressedPixMapInfo(int32_t pix, gaddr desc, gaddr data, gaddr bufferSize, gaddr dataProc,
                                   gaddr progressProc)
{
    for (int i = 0; i < MAX_CPIX; i++)
        if (pix && cpix[i].pix == (gaddr)pix) {
            if (desc)
                wr32(desc, (uint32_t)cpix[i].desc);
            if (data)
                wr32(data, (uint32_t)cpix[i].data);
            if (bufferSize)
                wr32(bufferSize, (uint32_t)cpix[i].size);
            if (dataProc)
                gmemset(dataProc, 0, 8);
            if (progressProc)
                gmemset(progressProc, 0, 8);
            return noErr;
        }
    return paramErr;
}

/* The pixel map (compressed, if SetCompressedPixMapInfo said so) drawn
 * through the matrix into the current port. */
void tb_StdPix(int32_t src, gaddr srcRect, int32_t matrix, int16_t mode, int32_t mask, int32_t matte,
               gaddr matteRect, int16_t flags)
{
    if (!src)
        return;
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    qrect r = { 0 };
    for (int i = 0; i < MAX_CPIX && !rgba; i++)
        if (cpix[i].pix == (gaddr)src) {
            int16_t err;
            rgba = icm_decode(desc_ptr(cpix[i].desc), (gaddr)cpix[i].data, cpix[i].size, &w, &h, &err);
            r = (qrect){ 0, 0, (int16_t)h, (int16_t)w };
        }
    if (!rgba) {
        r = srcRect ? qrect_get(srcRect) : qrect_get((gaddr)src + 6);
        rgba = qt_codec_pixmap_to_rgba((gaddr)src, r);
        w = r.right - r.left;
        h = r.bottom - r.top;
        r = (qrect){ 0, 0, (int16_t)h, (int16_t)w };
    }
    if (!rgba)
        return;
    qrect dst = r;
    if (matrix) {
        gaddr m = (gaddr)matrix;
        double sx = (int32_t)rd32(m) / 65536.0, sy = (int32_t)rd32(m + 16) / 65536.0;
        double tx = (int32_t)rd32(m + 24) / 65536.0, ty = (int32_t)rd32(m + 28) / 65536.0;
        dst = (qrect){ (int16_t)(r.top * sy + ty), (int16_t)(r.left * sx + tx), (int16_t)(r.bottom * sy + ty),
                       (int16_t)(r.right * sx + tx) };
    }
    gaddr saved = 0;
    if (mask) {
        saved = tb_NewRgn();
        tb_GetClip(saved);
        gaddr clip = tb_NewRgn();
        tb_SectRgn(saved, (gaddr)mask, clip);
        tb_SetClip(clip);
        tb_DisposeRgn(clip);
    }
    qd_draw_rgba(rgba, w, h, dst);
    if (saved) {
        tb_SetClip(saved);
        tb_DisposeRgn(saved);
    }
    free(rgba);
}

/* A region through a matrix's scale and translation. */
int16_t tb_TransformRgn(int32_t matrix, int32_t rgn)
{
    if (!rgn)
        return paramErr;
    if (!matrix)
        return noErr;
    gaddr m = (gaddr)matrix;
    if (rd32(m + 4) || rd32(m + 12)) /* rotation and skew: not a region's shape any more */
        return codecUnimpErr;
    double sx = (int32_t)rd32(m) / 65536.0, sy = (int32_t)rd32(m + 16) / 65536.0;
    double tx = (int32_t)rd32(m + 24) / 65536.0, ty = (int32_t)rd32(m + 28) / 65536.0;
    if (sx != 1.0 || sy != 1.0) {
        gaddr a = tb_NewPtr(8, false, true), b = tb_NewPtr(8, false, true);
        qrect_put(a, (qrect){ 0, 0, 1000, 1000 });
        qrect_put(b, (qrect){ 0, 0, (int16_t)(1000 * sy), (int16_t)(1000 * sx) });
        tb_MapRgn((gaddr)rgn, a, b);
        tb_DisposePtr(a);
        tb_DisposePtr(b);
    }
    tb_OffsetRgn((gaddr)rgn, (int16_t)lround(tx), (int16_t)lround(ty));
    return noErr;
}

int32_t tb_DragAlignedGrayRgn(int32_t theRgn, int32_t startPt, gaddr boundsRect, gaddr slopRect, int16_t axis,
                              int32_t actionProc, gaddr alignmentRect, int32_t alignmentProc)
{
    /* every place draws as fast here: no alignment to make */
    return tb_DragGrayRgn((gaddr)theRgn, (uint32_t)startPt, boundsRect, slopRect, axis, (gaddr)actionProc);
}

/* The main screen's rectangle less the menu bar. */
int16_t tb_GetBestDeviceRect(gaddr gdh, gaddr rp)
{
    gaddr dev = tb_GetMainDevice();
    if (gdh)
        wr32(gdh, dev);
    if (rp && dev) {
        qrect r = qrect_get(rd32(dev) + 34); /* gdRect */
        r.top += (int16_t)rd16(0x0BAA);      /* MBarHeight */
        qrect_put(rp, r);
    }
    return dev ? noErr : paramErr;
}

/* ---- Pixel formats ---- */

#define MAX_PIXFMT 16
static struct {
    uint32_t format;
    uint8_t info[48];
} pixfmts[MAX_PIXFMT];

/* ICMPixelFormatInfo: size 0, formatFlags 4, bitsPerPixel[14] 8, defaultGammaLevel 36. */
int16_t tb_ICMGetPixelFormatInfo(int32_t PixelFormat, int32_t theInfo)
{
    gaddr info = (gaddr)theInfo;
    if (!info)
        return paramErr;
    uint32_t size = rd32(info), f = (uint32_t)PixelFormat;
    if (size < 36)
        return paramErr;
    for (int i = 0; i < MAX_PIXFMT; i++)
        if (pixfmts[i].format == f && f) {
            gput(info + 4, pixfmts[i].info + 4, (size > 40 ? 40 : size) - 4);
            return noErr;
        }
    int bits = 0;
    uint32_t flags = 0;
    switch (f) {
    case 1: case 2: case 4: case 8: case 16: case 24: case 32: bits = (int)f; break;
    case 33: case 34: case 36: case 40: bits = (int)f - 32; break;
    case 0x62313667: /* 'b16g' */ case 0x6231364C: /* 'b16L' */ case 0x62353635: bits = 16; break;
    case 0x62323467: /* 'b24g' */ bits = 24; break;
    case 0x62333261: /* 'b32a' */ case 0x41424752: /* 'ABGR' */ case 0x42475241: /* 'BGRA' */
    case 0x52474241: /* 'RGBA' */ case 0x72343038: /* 'r408' */ case 0x76343038: /* 'v408' */ bits = 32; break;
    case 0x32767579: /* '2vuy' */ case 0x79757673: /* 'yuvs' */ case 0x79757632: /* 'yuv2' */ case 0x79757975:
        bits = 16;
        break;
    case 0x79343230: /* 'y420' */
        flags = 1; /* kICMPixelFormatIsPlanarMask: three planes */
        gmemset(info + 4, 0, 32);
        wr32(info + 4, flags);
        wr16(info + 8, 8);
        wr16(info + 10, 2);
        wr16(info + 12, 2);
        return noErr;
    default: return paramErr;
    }
    gmemset(info + 4, 0, 32);
    wr32(info + 4, flags);
    wr16(info + 8, (uint16_t)bits);
    if (size >= 40)
        wr32(info + 36, 0);
    return noErr;
}

int16_t tb_ICMSetPixelFormatInfo(int32_t PixelFormat, int32_t theInfo)
{
    gaddr info = (gaddr)theInfo;
    if (!info || rd32(info) < 36)
        return paramErr;
    int slot = -1;
    for (int i = 0; i < MAX_PIXFMT; i++)
        if (pixfmts[i].format == (uint32_t)PixelFormat || (!pixfmts[i].format && slot < 0))
            slot = i;
    if (slot < 0)
        return memFullErr;
    pixfmts[slot].format = (uint32_t)PixelFormat;
    memset(pixfmts[slot].info, 0, sizeof pixfmts[slot].info);
    gget(pixfmts[slot].info, info, rd32(info) > 40 ? 40 : rd32(info));
    return noErr;
}

/* ---- QuadToQuadMatrix: the projective map of one quadrilateral onto another ---- */

/* The map of the unit square onto a quadrilateral (Heckbert's). */
static void square_to_quad(const double q[4][2], double m[3][3])
{
    double x0 = q[0][0], y0 = q[0][1], x1 = q[1][0], y1 = q[1][1], x2 = q[2][0], y2 = q[2][1], x3 = q[3][0],
           y3 = q[3][1];
    double sx = x0 - x1 + x2 - x3, sy = y0 - y1 + y2 - y3;
    double g = 0, h = 0;
    if (fabs(sx) > 1e-12 || fabs(sy) > 1e-12) {
        double dx1 = x1 - x2, dx2 = x3 - x2, dy1 = y1 - y2, dy2 = y3 - y2, den = dx1 * dy2 - dx2 * dy1;
        g = (sx * dy2 - dx2 * sy) / den;
        h = (dx1 * sy - sx * dy1) / den;
    }
    /* row vectors: [x y 1] * m, as QuickTime's matrices */
    m[0][0] = x1 - x0 + g * x1; m[0][1] = y1 - y0 + g * y1; m[0][2] = g;
    m[1][0] = x3 - x0 + h * x3; m[1][1] = y3 - y0 + h * y3; m[1][2] = h;
    m[2][0] = x0;               m[2][1] = y0;               m[2][2] = 1;
}

static bool invert3(const double a[3][3], double o[3][3])
{
    double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                 a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (fabs(det) < 1e-12)
        return false;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            int r0 = (j + 1) % 3, r1 = (j + 2) % 3, c0 = (i + 1) % 3, c1 = (i + 2) % 3;
            o[i][j] = (a[r0][c0] * a[r1][c1] - a[r0][c1] * a[r1][c0]) / det;
        }
    return true;
}

int16_t tb_QuadToQuadMatrix(gaddr source, gaddr dest, gaddr map)
{
    if (!source || !dest || !map)
        return paramErr;
    double s[4][2], d[4][2];
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 2; k++) {
            s[i][k] = (int32_t)rd32(source + (uint32_t)(i * 8 + k * 4)) / 65536.0;
            d[i][k] = (int32_t)rd32(dest + (uint32_t)(i * 8 + k * 4)) / 65536.0;
        }
    double a[3][3], b[3][3], ai[3][3], m[3][3];
    square_to_quad(s, a);
    square_to_quad(d, b);
    if (!invert3(a, ai))
        return paramErr;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            m[i][j] = ai[i][0] * b[0][j] + ai[i][1] * b[1][j] + ai[i][2] * b[2][j];
    double norm = m[2][2] != 0 ? m[2][2] : 1;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double v = m[i][j] / norm; /* the last column is Fract (2.30), the rest Fixed */
            wr32(map + (uint32_t)(i * 12 + j * 4), (uint32_t)(int32_t)lround(v * (j == 2 ? 1073741824.0 : 65536.0)));
        }
    return noErr;
}

/* ---- Sequences: information, memory, completions ---- */

static bool any_seq(int32_t id) { return icm_dseq_exists(id) || icm_csequence_exists(id); }

int16_t tb_ICMSequenceGetInfo(int32_t seqID, int32_t which, gaddr data)
{
    if (!any_seq(seqID) || !data)
        return paramErr;
    wr32(data, 0); /* task weight and name: none */
    return noErr;
}

int16_t tb_ICMSequenceSetInfo(int32_t seqID, int32_t which, gaddr data, int32_t dataSize)
{
    return any_seq(seqID) ? noErr : paramErr;
}

/* We draw straight into the destination: no image or screen buffer. */
int16_t tb_GetDSequenceImageBuffer(int32_t seqID, gaddr gworld)
{
    if (gworld)
        wr32(gworld, 0);
    return icm_dseq_exists(seqID) ? noErr : paramErr;
}

int16_t tb_GetDSequenceScreenBuffer(int32_t seqID, gaddr gworld) { return tb_GetDSequenceImageBuffer(seqID, gworld); }

static bool same_image(const qt_video_format *f, gaddr d)
{
    return d && f && rd32(d + 4) == f->format && (int16_t)rd16(d + 32) == f->width &&
           (int16_t)rd16(d + 34) == f->height && (int16_t)rd16(d + 82) == f->depth;
}

int16_t tb_CDSequenceEquivalentImageDescription(int32_t seqID, int32_t newDesc, gaddr equivalent)
{
    const qt_video_format *f = icm_dseq_format(seqID);
    if (!f)
        return paramErr;
    if (equivalent)
        wr8(equivalent, same_image(f, desc_ptr(newDesc)));
    return noErr;
}

int16_t tb_CDSequenceEquivalentImageDescriptionS(int32_t seqID, int32_t newDesc, gaddr equivalent, gaddr canSwitch)
{
    const qt_video_format *f = icm_dseq_format(seqID);
    if (!f)
        return paramErr;
    gaddr d = desc_ptr(newDesc);
    if (equivalent)
        wr8(equivalent, same_image(f, d));
    if (canSwitch) /* any codec we decode */
        wr8(canSwitch, d && (qt_codec_decodes(rd32(d + 4)) || icm_is_effect(rd32(d + 4))));
    return noErr;
}

int16_t tb_ReplaceDSequenceImageDescription(int32_t seqID, int32_t newDesc)
{
    return icm_dseq_replace(seqID, desc_ptr(newDesc));
}

int16_t tb_SetSequenceProgressProc(int32_t seqID, gaddr progressProc) { return any_seq(seqID) ? noErr : paramErr; }
int16_t tb_ICMShieldSequenceCursor(int32_t seqID) { return any_seq(seqID) ? noErr : paramErr; }
int16_t tb_ICMSequenceLockBits(int32_t seqID, int32_t dst, int32_t flags) { return any_seq(seqID) ? noErr : paramErr; }
int16_t tb_ICMSequenceUnlockBits(int32_t seqID, int32_t flags) { return any_seq(seqID) ? noErr : paramErr; }
int16_t tb_SetDSequenceTimeCode(int32_t seqID, gaddr timeCodeFormat, gaddr timeCodeTime)
{
    return icm_dseq_exists(seqID) ? noErr : paramErr;
}
int16_t tb_CDSequenceInvalidate(int32_t seqID, int32_t invalRgn) { return any_seq(seqID) ? noErr : paramErr; }
int16_t tb_CDSequenceSetTimeBase(int32_t seqID, gaddr base) { return any_seq(seqID) ? noErr : paramErr; }

/* No chains of sequences here: a sequence is its own only member. */
int16_t tb_ICMSequenceGetChainMember(int32_t seqID, gaddr retSeqID, int32_t flags)
{
    if (retSeqID)
        wr32(retSeqID, 0);
    return any_seq(seqID) ? noErr : paramErr;
}

/* Our codecs don't hit-test their images (only sprites' did). */
int16_t tb_PtInDSequenceData(int32_t seqID, gaddr data, int32_t dataSize, int32_t where, gaddr hit)
{
    if (hit)
        wr8(hit, 0);
    return icm_dseq_exists(seqID) ? noErr : paramErr;
}

int16_t tb_HitTestDSequenceData(int32_t seqID, gaddr data, int32_t dataSize, int32_t where, gaddr hit, int32_t hitFlags)
{
    if (hit)
        wr32(hit, 0);
    return icm_dseq_exists(seqID) ? noErr : paramErr;
}

int16_t tb_CDSequenceNewMemory(int32_t seqID, gaddr data, int32_t dataSize, int32_t dataUse, int32_t memoryGoneProc,
                               gaddr refCon)
{
    if (!data)
        return paramErr;
    gaddr p = tb_NewPtr(dataSize, false, true);
    wr32(data, p);
    return p ? noErr : memFullErr;
}

int16_t tb_CDSequenceDisposeMemory(int32_t seqID, int32_t data)
{
    if (data)
        tb_DisposePtr((gaddr)data);
    return noErr;
}

/* ICMCompletionProcRecord: proc, refcon; the proc is
 * pascal void (OSErr result, short flags, long refcon). */
static void complete(int16_t err, int16_t flag, gaddr rec)
{
    if (!rec || !rd32(rec))
        return;
    cpu68k *c = g_cpu;
    cpu_push16(c, (uint16_t)err);
    cpu_push16(c, (uint16_t)flag);
    cpu_push32(c, rd32(rec + 4));
    guest_call(rd32(rec));
}

void tb_ICMDecompressComplete(int32_t seqID, int16_t err, int16_t flag, int32_t completionRtn)
{
    complete(err, flag, (gaddr)completionRtn);
}

int16_t tb_ICMDecompressCompleteS(int32_t seqID, int16_t err, int16_t flag, int32_t completionRtn)
{
    complete(err, flag, (gaddr)completionRtn);
    return noErr;
}

/* ---- Data sources ---- */

/* CDSequenceDataSource: recordSize 0, next 4, seqID 8, sourceID 12,
 * sourceType 16, sourceInputNumber 20, dataPtr 24, dataDescription 28,
 * changeSeed 32, transferProc 36, transferRefcon 40, dataSize 44,
 * dataQueue 48, originalDataPtr 52, originalDataSize 56,
 * originalDataDescription 60, originalDataDescriptionSeed 64. */
#define MAX_SOURCES 32
static gaddr sources[MAX_SOURCES];

static int source_slot(int32_t id)
{
    for (int i = 0; i < MAX_SOURCES; i++)
        if (sources[i] && sources[i] == (gaddr)id)
            return i;
    return -1;
}

int16_t tb_CDSequenceNewDataSource(int32_t seqID, gaddr sourceID, int32_t sourceType, int32_t sourceInputNumber,
                                   int32_t dataDescription, int32_t transferProc, gaddr refCon)
{
    if (!sourceID || !any_seq(seqID))
        return paramErr;
    wr32(sourceID, 0);
    int i = 0;
    while (i < MAX_SOURCES && sources[i])
        i++;
    if (i == MAX_SOURCES)
        return memFullErr;
    gaddr r = tb_NewPtr(68, true, true);
    if (!r)
        return memFullErr;
    wr32(r, 68);
    wr32(r + 8, (uint32_t)seqID);
    wr32(r + 12, r);
    wr32(r + 16, (uint32_t)sourceType);
    wr32(r + 20, (uint32_t)sourceInputNumber);
    wr32(r + 28, (uint32_t)dataDescription);
    wr32(r + 36, (uint32_t)transferProc);
    wr32(r + 40, refCon);
    wr32(r + 60, (uint32_t)dataDescription);
    sources[i] = r;
    wr32(sourceID, r);
    return noErr;
}

int16_t tb_CDSequenceDisposeDataSource(int32_t sourceID)
{
    int i = source_slot(sourceID);
    if (i < 0)
        return paramErr;
    tb_DisposePtr(sources[i]);
    sources[i] = 0;
    return noErr;
}

int16_t tb_CDSequenceSetSourceData(int32_t sourceID, gaddr data, int32_t dataSize)
{
    int i = source_slot(sourceID);
    if (i < 0)
        return paramErr;
    gaddr r = sources[i];
    wr32(r + 24, data);
    wr32(r + 44, (uint32_t)dataSize);
    wr32(r + 52, data);
    wr32(r + 56, (uint32_t)dataSize);
    wr32(r + 32, rd32(r + 32) + 1);
    return noErr;
}

int16_t tb_CDSequenceChangedSourceData(int32_t sourceID)
{
    int i = source_slot(sourceID);
    if (i < 0)
        return paramErr;
    wr32(sources[i] + 32, rd32(sources[i] + 32) + 1);
    return noErr;
}

int16_t tb_CDSequenceSetSourceDataQueue(int32_t sourceID, int32_t dataQueue)
{
    int i = source_slot(sourceID);
    if (i < 0)
        return paramErr;
    wr32(sources[i] + 48, (uint32_t)dataQueue);
    return noErr;
}

int16_t tb_CDSequenceGetDataSource(int32_t seqID, gaddr sourceID, int32_t sourceType, int32_t sourceInputNumber)
{
    if (!sourceID)
        return paramErr;
    wr32(sourceID, 0);
    for (int i = 0; i < MAX_SOURCES; i++) {
        gaddr r = sources[i];
        if (r && (int32_t)rd32(r + 8) == seqID && (int32_t)rd32(r + 16) == sourceType &&
            (int32_t)rd32(r + 20) == sourceInputNumber) {
            wr32(sourceID, r);
            return noErr;
        }
    }
    return paramErr;
}

void icm_sources_drop(int32_t seq)
{
    for (int i = 0; i < MAX_SOURCES; i++)
        if (sources[i] && (int32_t)rd32(sources[i] + 8) == seq) {
            tb_DisposePtr(sources[i]);
            sources[i] = 0;
        }
}

void icm_more_reset(void)
{
    memset(sources, 0, sizeof sources);
    memset(transcoders, 0, sizeof transcoders);
    memset(cpix, 0, sizeof cpix);
    memset(pixfmts, 0, sizeof pixfmts);
}

/* ---- Effects ---- */

static const struct {
    uint32_t type;
    const char *name;
    int sources;
} effects[] = {
    { 0x64736C76, "Cross Fade", 2 },    /* 'dslv' */
    { 0x626C6E64, "Alpha Compositor", 2 }, /* 'blnd' */
    { 0x736D7074, "Wipe", 2 },          /* 'smpt' */
    { 0x736D7032, "Iris", 2 },          /* 'smp2' */
    { 0x736D7033, "Radial", 2 },        /* 'smp3' */
    { 0x736D7034, "Matrix Wipe", 2 },   /* 'smp4' */
    { 0x70757368, "Push", 2 },          /* 'push' */
    { 0x736C6964, "Slide", 2 },         /* 'slid' */
    { 0x7A6F6F6D, "Zoom", 2 },          /* 'zoom' */
    { 0x626C7572, "Blur", 1 },          /* 'blur' */
    { 0x73687270, "Sharpen", 1 },       /* 'shrp' */
    { 0x65646765, "Edge Detection", 1 }, /* 'edge' */
    { 0x656D6273, "Emboss", 1 },        /* 'embs' */
    { 0x74696E74, "Color Tint", 1 },    /* 'tint' */
    { 0x736F6C72, "Solarize", 1 },      /* 'solr' */
    { 0x666D6E73, "Film Noise", 1 },    /* 'fmns' */
    { 0x6272636F, "Brightness and Contrast", 1 }, /* 'brco' */
    { 0x6761696E, "Alpha Gain", 1 },    /* 'gain' */
    { 0x72676262, "RGB Balance", 1 },   /* 'rgbb' */
    { 0x68736C62, "HSL Balance", 1 },   /* 'hslb' */
};
#define NEFFECTS (sizeof effects / sizeof effects[0])

static int effect_index(uint32_t type)
{
    for (size_t i = 0; i < NEFFECTS; i++)
        if (effects[i].type == type)
            return (int)i;
    return -1;
}

bool icm_is_effect(uint32_t type) { return effect_index(type) >= 0; }

int16_t tb_MakeImageDescriptionForEffect(int32_t effectType, gaddr idh)
{
    if (!idh)
        return paramErr;
    wr32(idh, 0);
    gaddr h = tb_NewHandle(86, false, true);
    if (!h)
        return memFullErr;
    gaddr d = rd32(h);
    wr32(d, 86);
    wr32(d + 4, (uint32_t)effectType);
    wr32(d + 20, fourcc("appl"));
    wr32(d + 36, 72u << 16);
    wr32(d + 40, 72u << 16);
    wr16(d + 48, 1);
    int k = effect_index((uint32_t)effectType);
    const char *name = k >= 0 ? effects[k].name : "";
    wr8(d + 50, (uint8_t)strlen(name));
    gput(d + 51, name, (uint32_t)strlen(name));
    wr16(d + 82, 24);
    wr16(d + 84, 0xFFFF);
    wr32(idh, h);
    return noErr;
}

/* QTGetEffectsList: an atom container of 'name' (C strings) and 'type'
 * atoms, ID 1 on, the effects with sources in the range asked. */
int16_t tb_QTGetEffectsList(gaddr returnedList, int32_t minSources, int32_t maxSources, int32_t getOptions)
{
    if (!returnedList)
        return paramErr;
    gaddr c = qt_atoms_new();
    if (!c)
        return memFullErr;
    uint32_t id = 1;
    int16_t err = noErr;
    for (size_t i = 0; i < NEFFECTS && !err; i++) {
        if (effects[i].sources < minSources || (maxSources >= 0 && effects[i].sources > maxSources))
            continue;
        qt_atoms_insert(c, 0, fourcc("name"), id, 0, effects[i].name, (uint32_t)strlen(effects[i].name) + 1, &err);
        uint8_t t[4] = { (uint8_t)(effects[i].type >> 24), (uint8_t)(effects[i].type >> 16),
                         (uint8_t)(effects[i].type >> 8), (uint8_t)effects[i].type };
        if (!err)
            qt_atoms_insert(c, 0, fourcc("type"), id, 0, t, 4, &err);
        id++;
    }
    wr32(returnedList, err ? 0 : c);
    if (err)
        tb_DisposeHandle(c);
    return err;
}

int16_t tb_QTGetEffectsListExtended(gaddr returnedList, int32_t minSources, int32_t maxSources, int32_t getOptions,
                                    int32_t majorClass, int32_t minorClass, int32_t filterProc, gaddr filterRefCon)
{
    return tb_QTGetEffectsList(returnedList, minSources, maxSources, getOptions);
}

/* Our effects all render in real time. */
int16_t tb_QTGetEffectSpeed(int32_t parameters, gaddr pFPS)
{
    if (pFPS)
        wr32(pFPS, 0x7FFFFFFF); /* effectIsRealtime */
    return noErr;
}

/* A source's image at w by h (*w, *h 0: its own size). */
static uint8_t *source_image(int32_t seq, uint32_t type, int *w, int *h)
{
    for (int i = 0; i < MAX_SOURCES; i++) {
        gaddr r = sources[i];
        if (!r || (int32_t)rd32(r + 8) != seq || rd32(r + 16) != type || !rd32(r + 24))
            continue;
        int iw, ih;
        int16_t err;
        uint8_t *img = icm_decode(desc_ptr((int32_t)rd32(r + 28)), rd32(r + 24), (int32_t)rd32(r + 44), &iw, &ih, &err);
        if (!img)
            return NULL;
        if (*w <= 0 || *h <= 0 || (iw == *w && ih == *h)) {
            *w = iw;
            *h = ih;
            return img;
        }
        uint8_t *s = malloc((size_t)*w * (size_t)*h * 4);
        for (int y = 0; s && y < *h; y++)
            for (int x = 0; x < *w; x++)
                memcpy(s + ((size_t)y * (size_t)*w + (size_t)x) * 4,
                       img + ((size_t)(y * ih / *h) * (size_t)iw + (size_t)(x * iw / *w)) * 4, 4);
        free(img);
        return s;
    }
    return NULL;
}

static const uint8_t *px(const uint8_t *img, int w, int h, int x, int y)
{
    x = clampi(x, 0, w - 1);
    y = clampi(y, 0, h - 1);
    return img + ((size_t)y * (size_t)w + (size_t)x) * 4;
}

/* A 3x3 kernel (sum div, plus bias) over the image. */
static void convolve(const uint8_t *a, uint8_t *o, int w, int h, const int k[9], int div, int bias)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            for (int c = 0; c < 3; c++) {
                int s = 0;
                for (int j = 0; j < 9; j++)
                    s += k[j] * px(a, w, h, x + j % 3 - 1, y + j / 3 - 1)[c];
                o[((size_t)y * (size_t)w + (size_t)x) * 4 + (size_t)c] = (uint8_t)clampi(s / div + bias, 0, 255);
            }
            o[((size_t)y * (size_t)w + (size_t)x) * 4 + 3] = 255;
        }
}

uint8_t *icm_effect_render(int32_t seq, uint32_t type, gaddr params, int32_t size, gaddr frameTime, int *w, int *h)
{
    int k = effect_index(type);
    if (k < 0)
        return NULL;
    /* the point in the effect: the frame's time within its virtual span */
    double t = 0;
    if (frameTime) {
        int64_t value = (int64_t)((uint64_t)rd32(frameTime) << 32 | rd32(frameTime + 4));
        int32_t rsize = (int32_t)rd32(frameTime + 20), vdur = rsize >= 48 ? (int32_t)rd32(frameTime + 44) : 0;
        int64_t vstart = rsize >= 48 ? (int64_t)((uint64_t)rd32(frameTime + 36) << 32 | rd32(frameTime + 40)) : 0;
        int32_t dur = (int32_t)rd32(frameTime + 12);
        if (vdur > 0)
            t = (double)(value - vstart) / vdur;
        else if (dur > 0)
            t = (double)value / dur;
    }
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    uint8_t *a = source_image(seq, fourcc("srcA"), w, h);
    if (!a)
        return NULL;
    uint8_t *b = effects[k].sources > 1 ? source_image(seq, fourcc("srcB"), w, h) : NULL;
    int W = *w, H = *h;
    size_t n = (size_t)W * (size_t)H;
    uint8_t *o = malloc(n * 4);
    if (!o || (effects[k].sources > 1 && !b)) {
        free(a);
        free(b);
        free(o);
        return NULL;
    }
    memcpy(o, a, n * 4);
    double cx = W / 2.0, cy = H / 2.0;
    switch (type) {
    case 0x64736C76: /* cross fade */
    case 0x626C6E64: /* the compositor: B over A, as far as the effect has gone */
        for (size_t i = 0; i < n * 4; i++)
            o[i] = (uint8_t)(a[i] + (b[i] - a[i]) * t + 0.5);
        break;
    case 0x736D7074: /* wipe: B from the left */
        for (int y = 0; y < H; y++)
            for (int x = 0; x < (int)(W * t) && x < W; x++)
                memcpy(o + ((size_t)y * W + x) * 4, b + ((size_t)y * W + x) * 4, 4);
        break;
    case 0x736D7032: { /* iris: B in a circle growing from the centre */
        double r = t * sqrt(cx * cx + cy * cy);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if ((x + 0.5 - cx) * (x + 0.5 - cx) + (y + 0.5 - cy) * (y + 0.5 - cy) <= r * r)
                    memcpy(o + ((size_t)y * W + x) * 4, b + ((size_t)y * W + x) * 4, 4);
        break;
    }
    case 0x736D7033: /* radial: B swept clockwise from twelve o'clock */
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                double ang = atan2(x + 0.5 - cx, cy - (y + 0.5));
                if (ang < 0)
                    ang += 2 * M_PI;
                if (ang <= t * 2 * M_PI)
                    memcpy(o + ((size_t)y * W + x) * 4, b + ((size_t)y * W + x) * 4, 4);
            }
        break;
    case 0x736D7034: /* matrix: B in an 8 by 8 grid of tiles, row by row */
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if ((y * 8 / H) * 8 + x * 8 / W < (int)(t * 64 + 0.5))
                    memcpy(o + ((size_t)y * W + x) * 4, b + ((size_t)y * W + x) * 4, 4);
        break;
    case 0x70757368: /* push: B pushes A out to the left */
    case 0x736C6964: { /* slide: B slides in over A */
        int off = (int)(W * t);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                uint8_t *d = o + ((size_t)y * W + x) * 4;
                int bx = x - (W - off);
                if (bx >= 0)
                    memcpy(d, b + ((size_t)y * W + bx) * 4, 4);
                else if (type == 0x70757368)
                    memcpy(d, a + ((size_t)y * W + x + off) * 4, 4);
            }
        break;
    }
    case 0x7A6F6F6D: { /* zoom: B grows from the centre */
        int zw = (int)(W * t), zh = (int)(H * t);
        int x0 = (W - zw) / 2, y0 = (H - zh) / 2;
        for (int y = 0; y < zh; y++)
            for (int x = 0; x < zw; x++)
                memcpy(o + ((size_t)(y0 + y) * W + x0 + x) * 4, px(b, W, H, x * W / zw, y * H / zh), 4);
        break;
    }
    case 0x626C7572: { /* blur */
        static const int kb[9] = { 1, 2, 1, 2, 4, 2, 1, 2, 1 };
        convolve(a, o, W, H, kb, 16, 0);
        break;
    }
    case 0x73687270: { /* sharpen */
        static const int ks[9] = { 0, -1, 0, -1, 5, -1, 0, -1, 0 };
        convolve(a, o, W, H, ks, 1, 0);
        break;
    }
    case 0x65646765: { /* edges: light lines on black */
        static const int ke[9] = { -1, -1, -1, -1, 8, -1, -1, -1, -1 };
        convolve(a, o, W, H, ke, 1, 0);
        break;
    }
    case 0x656D6273: { /* emboss: grey, lit from the top left */
        static const int km[9] = { -2, -1, 0, -1, 1, 1, 0, 1, 2 };
        convolve(a, o, W, H, km, 1, 0);
        for (size_t i = 0; i < n; i++) {
            int g = (o[i * 4] * 30 + o[i * 4 + 1] * 59 + o[i * 4 + 2] * 11) / 100;
            o[i * 4] = o[i * 4 + 1] = o[i * 4 + 2] = (uint8_t)g;
        }
        break;
    }
    case 0x74696E74: /* tint: sepia, the default's colours */
        for (size_t i = 0; i < n; i++) {
            int g = (a[i * 4] * 30 + a[i * 4 + 1] * 59 + a[i * 4 + 2] * 11) / 100;
            o[i * 4] = (uint8_t)clampi(g + 40, 0, 255);
            o[i * 4 + 1] = (uint8_t)clampi(g + 20, 0, 255);
            o[i * 4 + 2] = (uint8_t)g;
        }
        break;
    case 0x736F6C72: /* solarize: the bright half inverted */
        for (size_t i = 0; i < n * 4; i++)
            if ((i & 3) != 3 && a[i] > 127)
                o[i] = (uint8_t)(255 - a[i]);
        break;
    case 0x666D6E73: { /* film noise: dust */
        uint32_t seed = (uint32_t)(t * 1000) * 2654435761u + 1;
        for (size_t i = 0; i < n; i++) {
            seed = seed * 1103515245u + 12345u;
            if ((seed >> 16) % 500 == 0)
                o[i * 4] = o[i * 4 + 1] = o[i * 4 + 2] = (uint8_t)((seed >> 8) & 1 ? 0 : 255);
        }
        break;
    }
    default: /* brightness and contrast, gain, balances: at their defaults, the image as it is */
        break;
    }
    free(a);
    free(b);
    (void)params, (void)size;
    return o;
}
