/*
 * The Image Compression Manager's decompression side, on our codecs
 * (qt_codec.c): DecompressImage and FDecompressImage, decompression
 * sequences (a decoder and its frame kept between frames), image
 * descriptions' colour tables, and compressed pictures (opcode $8200,
 * through qd_picture.c).  Compression is in qt_icm_compress.c; this file
 * also answers for both sides: the codecs' information, and ending a
 * sequence of either kind.
 *
 * ImageDescription: idSize 0, cType 4, ..., width 32, height 34, hRes 36,
 * vRes 40, dataSize 44, frameCount 48, name 50, depth 82, clutID 84, and a
 * colour table at 86 when clutID is 0.  MatrixRecord: 3x3 Fixed, row major
 * (scale h 0, scale v 16, translate h 24, translate v 28).
 */
#include <stdlib.h>
#include <string.h>

#include "qd_internal.h"
#include "qt_codec.h"
#include "qt_comp.h"
#include "tb_internal.h"

enum { noCodecErr = -8961, codecBadDataErr = -8969, codecUnimpErr = -8962 };

/* An ImageDescriptionHandle's description (0 for a nil handle). */
static gaddr desc_ptr(int32_t h) { return h ? rd32((gaddr)h) : 0; }

/* The image description's format, with its colour table in palette; the
 * description's bytes are in f->desc until desc_done. */
static bool describe_image(gaddr desc, qt_video_format *f, uint32_t palette[256])
{
    if (!desc)
        return false;
    uint32_t n = rd32(desc);
    if (n < 86 || n > 1 << 20)
        n = 86;
    uint8_t *b = malloc(n);
    if (!b)
        return false;
    gget(b, desc, n);
    if (!qt_video_format_of_desc(b, n, f, palette)) {
        free(b);
        f->desc = NULL;
        return false;
    }
    return true;
}

static void desc_done(qt_video_format *f)
{
    free((void *)f->desc);
    f->desc = NULL;
    f->desc_len = 0;
}

/* The rectangle a matrix takes the image's bounds to (scale and translate). */
static qrect apply_matrix(gaddr matrix, qrect r)
{
    if (!matrix)
        return r;
    double sx = (int32_t)rd32(matrix) / 65536.0, sy = (int32_t)rd32(matrix + 16) / 65536.0;
    double tx = (int32_t)rd32(matrix + 24) / 65536.0, ty = (int32_t)rd32(matrix + 28) / 65536.0;
    return (qrect){ (int16_t)(r.top * sy + ty), (int16_t)(r.left * sx + tx), (int16_t)(r.bottom * sy + ty),
                    (int16_t)(r.right * sx + tx) };
}

/* Draw the frame's srcRect part (image coordinates) into dst (local
 * coordinates of the current port), within the port's clip and mask. */
static void draw(const uint8_t *frame, int w, int h, qrect src, qrect dst, gaddr mask)
{
    if (src.right > w)
        src.right = (int16_t)w;
    if (src.bottom > h)
        src.bottom = (int16_t)h;
    int sw = src.right - src.left, sh = src.bottom - src.top;
    if (sw <= 0 || sh <= 0 || src.left < 0 || src.top < 0)
        return;
    uint8_t *part = malloc((size_t)sw * (size_t)sh * 4);
    if (!part)
        return;
    for (int y = 0; y < sh; y++)
        memcpy(part + (size_t)y * (size_t)sw * 4, frame + ((size_t)(src.top + y) * (size_t)w + (size_t)src.left) * 4,
               (size_t)sw * 4);
    gaddr port = qd_port(), saved = 0;
    if (mask) { /* the mask as part of the clip */
        saved = tb_NewRgn();
        tb_GetClip(saved);
        tb_SectRgn(GrafPort_clipRgn(port), mask, GrafPort_clipRgn(port));
    }
    qd_draw_rgba(part, sw, sh, dst);
    if (saved) {
        tb_SetClip(saved);
        tb_DisposeRgn(saved);
    }
    free(part);
}

/* Decode one image into a new frame; NULL if we can't. */
static uint8_t *decode_one(gaddr desc, gaddr data, int32_t len, int *w, int *h, int16_t *err)
{
    qt_video_format f;
    uint32_t palette[256];
    if (!describe_image(desc, &f, palette) || !data) {
        desc_done(&f);
        *err = paramErr;
        return NULL;
    }
    qt_vdec *d = qt_vdec_new(&f);
    desc_done(&f);
    if (!d) {
        *err = noCodecErr;
        return NULL;
    }
    if (len <= 0)
        len = (int32_t)rd32(desc + 44);
    uint8_t *bytes = malloc(len > 0 ? (size_t)len : 1), *frame = calloc((size_t)f.width * (size_t)f.height, 4);
    if (len > 0)
        gget(bytes, data, (uint32_t)len);
    bool ok = bytes && frame && len > 0 && qt_vdec_decode(d, bytes, (size_t)len, frame);
    qt_vdec_free(d);
    free(bytes);
    if (!ok) {
        free(frame);
        *err = codecBadDataErr;
        return NULL;
    }
    *w = f.width;
    *h = f.height;
    *err = noErr;
    return frame;
}

/* A compressed picture's image ($8200), drawn at dst in the current port. */
void icm_draw_compressed(gaddr desc, gaddr data, qrect src, qrect dst, gaddr mask)
{
    int w, h;
    int16_t err;
    uint8_t *frame = decode_one(desc, data, (int32_t)rd32(desc + 44), &w, &h, &err);
    if (!frame) {
        LOGF(LOG_UNIMPL, "compressed picture: codec '%c%c%c%c' not decoded (%d)\n", rd8(desc + 4), rd8(desc + 5),
             rd8(desc + 6), rd8(desc + 7), err);
        return;
    }
    draw(frame, w, h, src, dst, mask);
    free(frame);
}

int16_t tb_DecompressImage(int32_t data, int32_t desc, int32_t dst, gaddr srcRect, gaddr dstRect, int16_t mode,
                           int32_t mask)
{
    int w, h;
    int16_t err;
    uint8_t *frame = decode_one(desc_ptr(desc), (gaddr)data, 0, &w, &h, &err);
    if (!frame)
        return err;
    qrect src = srcRect ? qrect_get(srcRect) : (qrect){ 0, 0, (int16_t)h, (int16_t)w };
    qrect to = dstRect ? qrect_get(dstRect) : src;
    draw(frame, w, h, src, to, (gaddr)mask);
    free(frame);
    return noErr;
}

int16_t tb_FDecompressImage(int32_t data, int32_t desc, int32_t dst, gaddr srcRect, int32_t matrix, int16_t mode,
                            int32_t mask, int32_t matte, gaddr matteRect, int32_t accuracy, int32_t codec,
                            int32_t bufferSize, int32_t dataProc, int32_t progressProc)
{
    int w, h;
    int16_t err;
    uint8_t *frame = decode_one(desc_ptr(desc), (gaddr)data, bufferSize, &w, &h, &err);
    if (!frame)
        return err;
    qrect src = srcRect ? qrect_get(srcRect) : (qrect){ 0, 0, (int16_t)h, (int16_t)w };
    draw(frame, w, h, src, apply_matrix((gaddr)matrix, src), (gaddr)mask);
    free(frame);
    return noErr;
}

/* ---- Sequences ---- */

#define MAX_SEQS 16

typedef struct {
    bool used;
    qt_vdec *dec;
    qt_video_format format;
    uint32_t palette[256];
    uint8_t *frame;
    gaddr port;
    qrect src;
    uint8_t matrix[36];
    bool has_matrix;
    gaddr mask;
    int32_t data_size;   /* the description's, for frames given without one */
} sequence;

static sequence seqs[MAX_SEQS];

static sequence *seq_of(int32_t id)
{
    int i = id - 0x51530001; /* 'QS' and the slot */
    return i >= 0 && i < MAX_SEQS && seqs[i].used ? &seqs[i] : NULL;
}

void icm_reset(void)
{
    for (int i = 0; i < MAX_SEQS; i++)
        if (seqs[i].used) {
            qt_vdec_free(seqs[i].dec);
            free(seqs[i].frame);
        }
    memset(seqs, 0, sizeof seqs);
    icm_compress_reset();
}

int16_t tb_DecompressSequenceBegin(gaddr seqID, int32_t desc, int32_t port, int32_t gdh, gaddr srcRect,
                                   int32_t matrix, int16_t mode, int32_t mask, int16_t flags, int32_t accuracy,
                                   int32_t codec)
{
    if (seqID)
        wr32(seqID, 0);
    sequence *s = NULL;
    for (int i = 0; i < MAX_SEQS && !s; i++)
        if (!seqs[i].used)
            s = &seqs[i];
    if (!s)
        return memFullErr;
    memset(s, 0, sizeof *s);
    if (!describe_image(desc_ptr(desc), &s->format, s->palette))
        return paramErr;
    if (s->format.palette)
        s->format.palette = s->palette;
    s->dec = qt_vdec_new(&s->format);
    desc_done(&s->format);
    if (!s->dec)
        return noCodecErr;
    s->frame = calloc((size_t)s->format.width * (size_t)s->format.height, 4);
    s->port = port ? (gaddr)port : qd_port();
    s->src = srcRect ? qrect_get(srcRect) : (qrect){ 0, 0, (int16_t)s->format.height, (int16_t)s->format.width };
    if (matrix) {
        gget(s->matrix, (gaddr)matrix, 36);
        s->has_matrix = true;
    }
    s->mask = (gaddr)mask;
    s->data_size = (int32_t)rd32(desc_ptr(desc) + 44);
    s->used = true;
    if (seqID)
        wr32(seqID, (uint32_t)(0x51530001 + (s - seqs)));
    return noErr;
}

int16_t tb_DecompressSequenceBeginS(gaddr seqID, int32_t desc, int32_t data, int32_t dataSize, int32_t port,
                                    int32_t gdh, gaddr srcRect, int32_t matrix, int16_t mode, int32_t mask,
                                    int16_t flags, int32_t accuracy, int32_t codec)
{
    return tb_DecompressSequenceBegin(seqID, desc, port, gdh, srcRect, matrix, mode, mask, flags, accuracy, codec);
}

static int16_t sequence_frame(int32_t id, gaddr data, int32_t size, gaddr outFlags)
{
    sequence *s = seq_of(id);
    if (outFlags)
        wr16(outFlags, 0);
    if (!s)
        return paramErr;
    if (!data)
        return paramErr;
    if (size <= 0)
        size = s->data_size > 0 ? s->data_size : 1 << 18; /* the codec stops where the frame ends */
    uint8_t *bytes = malloc((size_t)size);
    gget(bytes, data, (uint32_t)size);
    bool ok = qt_vdec_decode(s->dec, bytes, (size_t)size, s->frame);
    free(bytes);
    gaddr saved = qd_port();
    tb_SetPort(s->port);
    qrect dst = s->src;
    if (s->has_matrix) { /* through a guest copy of the matrix */
        gaddr m = tb_NewPtr(36, false, false);
        gput(m, s->matrix, 36);
        dst = apply_matrix(m, s->src);
        tb_DisposePtr(m);
    }
    draw(s->frame, s->format.width, s->format.height, s->src, dst, s->mask);
    tb_SetPort(saved);
    return ok ? noErr : codecBadDataErr;
}

int16_t tb_DecompressSequenceFrame(int32_t seqID, int32_t data, int16_t inFlags, gaddr outFlags,
                                   int32_t asyncCompletionProc)
{
    return sequence_frame(seqID, (gaddr)data, 0, outFlags);
}

int16_t tb_DecompressSequenceFrameS(int32_t seqID, int32_t data, int32_t dataSize, int16_t inFlags, gaddr outFlags,
                                    int32_t asyncCompletionProc)
{
    return sequence_frame(seqID, (gaddr)data, dataSize, outFlags);
}

int16_t tb_DecompressSequenceFrameWhen(int32_t seqID, int32_t data, int32_t dataSize, int16_t inFlags,
                                       gaddr outFlags, int32_t asyncCompletionProc, gaddr frameTime)
{
    return sequence_frame(seqID, (gaddr)data, dataSize, outFlags);
}

int16_t tb_CDSequenceEnd(int32_t seqID)
{
    sequence *s = seq_of(seqID);
    if (!s)
        return icm_csequence_end(seqID) ? noErr : paramErr;
    qt_vdec_free(s->dec);
    free(s->frame);
    memset(s, 0, sizeof *s);
    return noErr;
}

int16_t tb_CDSequenceBusy(int32_t seqID) { return 0; }
int16_t tb_CDSequenceFlush(int32_t seqID) { return seq_of(seqID) || icm_csequence_exists(seqID) ? noErr : paramErr; }

int16_t tb_SetDSequenceMatrix(int32_t seqID, int32_t matrix)
{
    sequence *s = seq_of(seqID);
    if (!s)
        return paramErr;
    s->has_matrix = matrix != 0;
    if (matrix)
        gget(s->matrix, (gaddr)matrix, 36);
    return noErr;
}

int16_t tb_GetDSequenceMatrix(int32_t seqID, int32_t matrixp)
{
    gaddr matrix = (gaddr)matrixp;
    sequence *s = seq_of(seqID);
    if (!s || !matrix)
        return paramErr;
    if (s->has_matrix) {
        gput(matrix, s->matrix, 36);
    } else { /* identity */
        gmemset(matrix, 0, 36);
        wr32(matrix, 0x10000);
        wr32(matrix + 16, 0x10000);
        wr32(matrix + 32, 0x40000000);
    }
    return noErr;
}

int16_t tb_SetDSequenceSrcRect(int32_t seqID, gaddr srcRect)
{
    sequence *s = seq_of(seqID);
    if (!s)
        return paramErr;
    s->src = srcRect ? qrect_get(srcRect) : (qrect){ 0, 0, (int16_t)s->format.height, (int16_t)s->format.width };
    return noErr;
}

int16_t tb_SetDSequenceMask(int32_t seqID, int32_t mask)
{
    sequence *s = seq_of(seqID);
    if (!s)
        return paramErr;
    s->mask = (gaddr)mask;
    return noErr;
}

int16_t tb_SetDSequenceTransferMode(int32_t seqID, int16_t mode, gaddr opColor) { return seq_of(seqID) ? noErr : paramErr; }
int16_t tb_SetDSequenceFlags(int32_t seqID, int32_t flags, int32_t flagsMask) { return seq_of(seqID) ? noErr : paramErr; }
int16_t tb_SetDSequenceAccuracy(int32_t seqID, int32_t accuracy) { return seq_of(seqID) ? noErr : paramErr; }
int16_t tb_SetDSequenceMatte(int32_t seqID, int32_t matte, gaddr matteRect) { return seq_of(seqID) ? noErr : paramErr; }
int16_t tb_SetDSequenceDataProc(int32_t seqID, int32_t dataProc, int32_t bufferSize) { return seq_of(seqID) ? noErr : paramErr; }

/* ---- Descriptions and codecs ---- */

int16_t tb_GetImageDescriptionCTable(int32_t desc, gaddr ctable)
{
    qt_video_format f;
    uint32_t palette[256];
    if (!ctable || !describe_image(desc_ptr(desc), &f, palette))
        return paramErr;
    desc_done(&f);
    int bits = f.depth > 32 ? f.depth - 32 : f.depth;
    if (bits < 1 || bits > 8) {
        wr32(ctable, 0);
        return noErr;
    }
    const uint32_t *p = f.palette ? f.palette : qt_standard_palette(f.depth);
    int n = 1 << bits;
    gaddr h = tb_NewHandle(8 + n * 8, false, true);
    if (!h)
        return memFullErr;
    gaddr c = rd32(h);
    wr32(c, 0);
    wr16(c + 4, 0);
    wr16(c + 6, (uint16_t)(n - 1));
    for (int k = 0; k < n; k++) {
        gaddr e = c + 8 + (uint32_t)k * 8;
        uint32_t rgb = p ? p[k] : 0;
        wr16(e, (uint16_t)k);
        wr16(e + 2, (uint16_t)((rgb >> 16 & 0xFF) * 0x101));
        wr16(e + 4, (uint16_t)((rgb >> 8 & 0xFF) * 0x101));
        wr16(e + 6, (uint16_t)((rgb & 0xFF) * 0x101));
    }
    wr32(ctable, h);
    return noErr;
}

int16_t tb_GetCompressedImageSize(int32_t desc, int32_t data, int32_t bufferSize, int32_t dataProc, gaddr dataSize)
{
    if (!desc_ptr(desc) || !dataSize)
        return paramErr;
    wr32(dataSize, rd32(desc_ptr(desc) + 44));
    return noErr;
}

int16_t tb_CodecManagerVersion(gaddr version)
{
    if (version)
        wr32(version, 0x00020000); /* the Image Compression Manager of QuickTime 2 */
    return noErr;
}

/* A codec component of the type and kind ('imdc', 'imco'): the one asked
 * for if it is one, else the first registered (ours, or a program's). */
static uint32_t codec_component(uint32_t kind, uint32_t type, uint32_t asked)
{
    uint32_t t, st, m;
    if (asked) {
        uint32_t c = components_component_of(asked) ? components_component_of(asked) : asked;
        if (components_describe(c, &t, &st, &m) && t == kind)
            return c;
    }
    return components_find_next(0, kind, type, 0);
}

/* CodecInfo from the codec component itself (a program's answers for its own). */
int16_t tb_GetCodecInfo(gaddr info, int32_t cType, int32_t codec)
{
    if (!info)
        return paramErr;
    uint32_t c = codec_component(fourcc("imdc"), (uint32_t)cType, (uint32_t)codec);
    uint32_t e = codec_component(fourcc("imco"), (uint32_t)cType, (uint32_t)codec);
    if (!c && !e)
        return noCodecErr;
    if ((!c || qt_codec_component_is_ours(c)) && (!e || qt_codec_component_is_ours(e))) {
        qt_codec_fill_info(info, (uint32_t)cType, c != 0, e != 0);
        return noErr;
    }
    int16_t err;
    uint32_t ci = components_open_component(c ? c : e, &err);
    if (!ci)
        return err ? err : noCodecErr;
    uint32_t args[1] = { info };
    int32_t r = qtc_call_named(ci, "ImageCodecGetCodecInfo", args);
    components_close(ci);
    return (int16_t)r;
}

int16_t tb_FindCodec(int32_t cType, int32_t specCodec, gaddr compressor, gaddr decompressor)
{
    uint32_t type = (uint32_t)cType;
    uint32_t e = codec_component(fourcc("imco"), type, (uint32_t)specCodec);
    uint32_t d = codec_component(fourcc("imdc"), type, (uint32_t)specCodec);
    if (compressor)
        wr32(compressor, e);
    if (decompressor)
        wr32(decompressor, d);
    return d || e ? noErr : noCodecErr;
}

/* Alignment for the fastest drawing: any place is as fast here. */
void tb_AlignWindow(int32_t wp, int8_t front, gaddr alignmentRect, int32_t alignmentProc) {}
void tb_AlignScreenRect(gaddr rp, int32_t alignmentProc) {}

void tb_DragAlignedWindow(int32_t wp, int32_t startPt, gaddr boundsRect, gaddr alignmentRect, int32_t alignmentProc)
{
    tb_DragWindow((gaddr)wp, (uint32_t)startPt, boundsRect);
}
