/*
 * QuickTime's graphics exporters ('grex' components): an image from a
 * graphics importer, a picture, a GWorld, a pixel map or the bytes of an
 * image file, written as a file of another kind to a file, a handle or a
 * data reference.  JPEG, PNG, TIFF, BMP, GIF, Photoshop, TGA, SGI and
 * JPEG 2000 are written by the host's image encoders (hostvideo.h), where
 * it has them; PICT and the QuickTime Image format are ours.
 *
 * The calls are QuickTimeComponents.h's GraphicsExport family; the base
 * exporter's own (ReadInputData, WriteOutputData, the marks) work on the
 * input and output as a derived exporter of a program's would use them.
 */
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "hostvideo.h"
#include "qd_internal.h"
#include "qt_atoms.h"
#include "qt_codec.h"
#include "qt_comp.h"
#include "qt_dref.h"
#include "qt_errors.h"
#include "qt_movie_internal.h"
#include "tb_internal.h"

enum {
    GREX = 0x67726578, APPL = 0x6170706C, PICT = 0x50494354, QTIF = 0x71746966, JPEGT = 0x4A504547,
    FSSPEC = 70, userCanceledErr = -128, dupFNErr = -48, eofErr = -39, fsRdWrPerm = 3, codecNormalQuality = 0x200,
    codecMaxQuality = 0x3FF, codecLosslessQuality = 0x400,
};

/* The exporters: file type, extension, MIME type, its name. */
static const struct {
    uint32_t type;
    const char *ext, *mime, *name;
} kinds[] = {
    { 0x50494354, "pct", "image/x-pict", "PICT" },
    { 0x71746966, "qtif", "image/x-quicktime", "QuickTime Image" },
    { 0x4A504547, "jpg", "image/jpeg", "JPEG" },
    { 0x504E4766, "png", "image/png", "PNG" },
    { 0x54494646, "tif", "image/tiff", "TIFF" },
    { 0x424D5066, "bmp", "image/bmp", "BMP" },
    { 0x47494666, "gif", "image/gif", "GIF" },
    { 0x38425053, "psd", "image/x-photoshop", "Photoshop" },
    { 0x54504943, "tga", "image/x-targa", "TGA" },
    { 0x53474920, "sgi", "image/x-sgi", "SGI" },
    { 0x6A703220, "jp2", "image/jp2", "JPEG 2000" },
};
#define NKINDS (sizeof kinds / sizeof kinds[0])

static int kind_of(uint32_t type)
{
    for (size_t i = 0; i < NKINDS; i++)
        if (kinds[i].type == type)
            return (int)i;
    return -1;
}

static bool can_write(uint32_t type) { return type == PICT || type == QTIF || hostimage_can_encode(type); }

enum { IN_NONE, IN_IMPORTER, IN_PICTURE, IN_GWORLD, IN_PIXMAP, IN_HANDLE, IN_FILE, IN_DATAREF, IN_PTR };
enum { OUT_NONE, OUT_FILE, OUT_HANDLE, OUT_DATAREF };

typedef struct {
    int in;
    uint32_t in_obj;          /* the importer, picture, GWorld, pixel map, handle or pointer */
    uint32_t in_gdh;
    gaddr in_spec;            /* FSSpec copies */
    uint8_t *in_ref;
    uint32_t in_ref_len, in_ref_type, in_size;
    uint32_t in_offset, in_limit;
    int out;
    gaddr out_h, out_spec;
    uint8_t *out_ref;
    uint32_t out_ref_len, out_ref_type;
    uint32_t out_offset, out_max, out_mark;
    bool out_truncate;
    uint32_t file_type, creator;
    int32_t method, quality, depth, target_size, interlace;
    int32_t hres, vres;
    bool dont_recompress;
    gaddr metadata, colorsync, progress_proc;
    int32_t progress_refcon;
} grex;

/* ---- Input ---- */

/* The input image file's bytes (handle, pointer, file or data reference). */
static uint8_t *input_bytes(grex *x, size_t *len)
{
    uint8_t *b = NULL;
    *len = 0;
    if (x->in == IN_HANDLE && x->in_obj && rd32(x->in_obj)) {
        *len = (size_t)tb_GetHandleSize(x->in_obj);
        b = malloc(*len ? *len : 1);
        if (b)
            gget(b, rd32(x->in_obj), (uint32_t)*len);
    } else if (x->in == IN_PTR && x->in_obj) {
        *len = x->in_size;
        b = malloc(*len ? *len : 1);
        if (b)
            gget(b, x->in_obj, (uint32_t)*len);
    } else if (x->in == IN_FILE || x->in == IN_DATAREF) {
        qt_src src;
        bool open;
        if (x->in == IN_FILE) {
            char path[PATH_MAX];
            open = fm_host_path((int16_t)rd16(x->in_spec), (int32_t)rd32(x->in_spec + 2), x->in_spec + 6, true, path,
                                sizeof path);
            gaddr ref = open ? qt_dref_for_path(path) : 0;
            uint32_t n = 0;
            uint8_t *rb = ref ? qt_dref_bytes(ref, &n) : NULL;
            open = rb && qt_src_open(fourcc("alis"), 0, rb, n, NULL, false, &src);
            free(rb);
            if (ref)
                tb_DisposeHandle(ref);
        } else {
            open = qt_src_open(x->in_ref_type, 0, x->in_ref, x->in_ref_len, NULL, false, &src);
        }
        if (open) {
            int64_t n = qt_src_size(&src);
            if (n > 0 && n < 256 * 1024 * 1024) {
                *len = (size_t)n;
                b = malloc(*len);
                if (b && !qt_src_read(&src, 0, b, (uint32_t)*len)) {
                    free(b);
                    b = NULL;
                }
            }
            qt_src_close(&src);
        }
    }
    if (b && (x->in_offset || x->in_limit)) { /* SetInputOffsetAndLimit */
        size_t off = x->in_offset < *len ? x->in_offset : *len;
        size_t lim = x->in_limit && x->in_limit < *len ? x->in_limit : *len;
        size_t n = lim > off ? lim - off : 0;
        memmove(b, b + off, n);
        *len = n;
    }
    if (!b)
        *len = 0;
    return b;
}

static bool is_pict_file(const uint8_t *d, size_t n)
{
    return n >= 512 + 12 && ((d[522] == 0x11 && d[523] == 0x01) || (d[522] == 0 && d[523] == 0x11 && d[524] == 2));
}

/* A picture's frame size. */
static void picture_size(gaddr pic, int *w, int *h)
{
    qrect f = qrect_get(rd32(pic) + 2);
    *w = f.right - f.left;
    *h = f.bottom - f.top;
}

/* The input as pixels; its picture too, when it is one (PICT out keeps it). */
static uint8_t *input_rgba(grex *x, int *w, int *h, gaddr *pict)
{
    *pict = 0;
    *w = *h = 0;
    switch (x->in) {
    case IN_IMPORTER: {
        uint8_t *rgba;
        if (qt_gimport_image(x->in_obj, &rgba, w, h, pict)) {
            if (*pict)
                rgba = icm_picture_rgba(*pict, *w, *h);
            return rgba;
        }
        /* a program's importer: drawn into a GWorld of its natural size */
        gaddr r = tb_NewPtr(8, false, true), gwp = tb_NewPtr(4, false, true), saved = tb_NewPtr(8, false, true);
        uint32_t a[2] = { r, 0 };
        uint8_t *out = NULL;
        if (qtc_call_named(x->in_obj, "GraphicsImportGetNaturalBounds", a) == noErr) {
            qrect b = qrect_get(r);
            *w = b.right - b.left;
            *h = b.bottom - b.top;
            if (*w > 0 && *h > 0 && tb_NewGWorld(gwp, 32, r, 0, 0, 0) == noErr) {
                gaddr gw = rd32(gwp);
                tb_LockPixels(tb_GetGWorldPixMap(gw));
                uint32_t g[2] = { gw, 0 }, none[1] = { 0 };
                qtc_call_named(x->in_obj, "GraphicsImportSetGWorld", g);
                qtc_call_named(x->in_obj, "GraphicsImportSetBoundsRect", a);
                qtc_call_named(x->in_obj, "GraphicsImportDraw", none);
                out = qt_codec_pixmap_to_rgba(rd32(tb_GetGWorldPixMap(gw)), b);
                tb_DisposeGWorld(gw);
            }
        }
        tb_DisposePtr(r);
        tb_DisposePtr(gwp);
        tb_DisposePtr(saved);
        return out;
    }
    case IN_PICTURE:
        if (!x->in_obj || !rd32(x->in_obj))
            return NULL;
        picture_size(x->in_obj, w, h);
        return icm_picture_rgba(x->in_obj, *w, *h);
    case IN_GWORLD:
    case IN_PIXMAP: {
        gaddr pmh = x->in == IN_GWORLD ? tb_GetGWorldPixMap(x->in_obj) : x->in_obj;
        if (!pmh || !rd32(pmh))
            return NULL;
        qrect b = qrect_get(rd32(pmh) + 6);
        *w = b.right - b.left;
        *h = b.bottom - b.top;
        return qt_codec_pixmap_to_rgba(rd32(pmh), b);
    }
    case IN_HANDLE:
    case IN_PTR:
    case IN_FILE:
    case IN_DATAREF: {
        size_t n;
        uint8_t *b = input_bytes(x, &n), *rgba = NULL;
        if (!b)
            return NULL;
        if (is_pict_file(b, n)) {
            gaddr pic = tb_NewHandle((int32_t)(n - 512), false, false);
            if (pic) {
                gput(rd32(pic), b + 512, (uint32_t)(n - 512));
                picture_size(pic, w, h);
                rgba = icm_picture_rgba(pic, *w, *h);
                *pict = pic;
            }
        } else if (!qt_decode_image(b, n, &rgba, w, h)) {
            rgba = NULL;
        }
        free(b);
        return rgba;
    }
    }
    return NULL;
}

/* ---- Output ---- */

static int16_t write_out(grex *x, const uint8_t *bytes, size_t n, uint32_t *written)
{
    *written = 0;
    if (x->out_max && n > x->out_max)
        return codecSizeErr;
    uint32_t at = x->out_offset;
    switch (x->out) {
    case OUT_HANDLE: {
        uint32_t need = at + (uint32_t)n;
        if (x->out_truncate || (uint32_t)tb_GetHandleSize(x->out_h) < need)
            tb_SetHandleSize(x->out_h, (int32_t)need);
        if ((uint32_t)tb_GetHandleSize(x->out_h) < need)
            return memFullErr;
        gput(rd32(x->out_h) + at, bytes, (uint32_t)n);
        break;
    }
    case OUT_FILE: {
        int16_t err = tb_FSpCreate(x->out_spec, (int32_t)x->creator, (int32_t)x->file_type, 0);
        if (err && err != dupFNErr)
            return err;
        if (err == dupFNErr) { /* QuickTime made the file the type asked for */
            gaddr info = tb_NewPtr(16, false, true);
            if (tb_FSpGetFInfo(x->out_spec, info) == noErr) {
                wr32(info, x->file_type);
                wr32(info + 4, x->creator);
                tb_FSpSetFInfo(x->out_spec, info);
            }
            tb_DisposePtr(info);
        }
        gaddr ref = tb_NewPtr(2, false, true), pb = tb_NewPtr(80, false, true), buf = tb_NewPtr((int32_t)n, false, false);
        err = buf ? tb_FSpOpenDF(x->out_spec, fsRdWrPerm, ref) : memFullErr;
        if (!err) {
            int16_t rn = (int16_t)rd16(ref);
            wr16(pb + 24, (uint16_t)rn);
            if (x->out_truncate || !at) {
                wr32(pb + 28, at + (uint32_t)n);
                tb_PBSetEOF(pb, false);
            }
            gput(buf, bytes, (uint32_t)n);
            wr32(pb + 32, buf);
            wr32(pb + 36, (uint32_t)n);
            wr16(pb + 44, 1);
            wr32(pb + 46, at);
            err = tb_PBWrite(pb, false);
            tb_FSCloseFork(rn);
        }
        if (buf)
            tb_DisposePtr(buf);
        tb_DisposePtr(ref);
        tb_DisposePtr(pb);
        if (err)
            return err;
        break;
    }
    case OUT_DATAREF: {
        qt_src src;
        if (!qt_src_open(x->out_ref_type, 0, x->out_ref, x->out_ref_len, NULL, true, &src))
            return couldNotResolveDataRef;
        if (x->out_truncate)
            qt_src_set_size(&src, at);
        bool ok = qt_src_write(&src, at, bytes, (uint32_t)n);
        qt_src_close(&src);
        if (!ok)
            return dataNotOpenForWrite;
        break;
    }
    default: return paramErr;
    }
    *written = (uint32_t)n;
    x->out_mark = at + (uint32_t)n;
    return noErr;
}

/* ---- Encoding ---- */

static double quality_of(int32_t q) { return q >= codecLosslessQuality ? 1.0 : q <= 0 ? 0.0 : q / 1023.0; }

/* The picture as a PICT file's bytes (the 512-byte header, then it). */
static uint8_t *pict_file(gaddr pic, size_t *n)
{
    size_t pn = (size_t)tb_GetHandleSize(pic);
    uint8_t *b = calloc(512 + pn, 1);
    if (b)
        gget(b + 512, rd32(pic), (uint32_t)pn);
    *n = b ? 512 + pn : 0;
    return b;
}

static void be32put(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* A QuickTime Image file: an 'idsc' atom (the image description) and an
 * 'idat' atom (the compressed image): Photo-JPEG, else None. */
static uint8_t *qtif_file(const uint8_t *rgba, int w, int h, int32_t quality, size_t *n)
{
    *n = 0;
    gaddr desc = tb_NewHandle(0, false, false), data = tb_NewPtr(w * h * 4 + 4096, false, false);
    gaddr gwp = tb_NewPtr(4, false, true), r = tb_NewPtr(8, false, true);
    uint8_t *out = NULL;
    qrect_put(r, (qrect){ 0, 0, (int16_t)h, (int16_t)w });
    if (desc && data && tb_NewGWorld(gwp, 32, r, 0, 0, 0) == noErr) {
        gaddr gw = rd32(gwp), pmh = tb_GetGWorldPixMap(gw);
        tb_LockPixels(pmh);
        qt_codec_frame_to_pixmap(rgba, w, h, rd32(pmh), (qrect){ 0, 0, (int16_t)h, (int16_t)w });
        uint32_t type = qt_venc_supported(0x6A706567, 24) ? 0x6A706567 : 0x72617720;
        if (tb_FCompressImage((int32_t)pmh, r, type == 0x6A706567 ? 24 : 32, quality, (int32_t)type, 0, 0, 0, 0, 0, 0,
                              (int32_t)desc, (int32_t)data) == noErr) {
            uint32_t dn = rd32(rd32(desc)), in = rd32(rd32(desc) + 44);
            *n = 8 + dn + 8 + in;
            out = malloc(*n);
            if (out) {
                be32put(out, 8 + dn);
                memcpy(out + 4, "idsc", 4);
                gget(out + 8, rd32(desc), dn);
                be32put(out + 8 + dn, 8 + in);
                memcpy(out + 12 + dn, "idat", 4);
                gget(out + 16 + dn, data, in);
            } else {
                *n = 0;
            }
        }
        tb_DisposeGWorld(gw);
    }
    if (desc)
        tb_DisposeHandle(desc);
    if (data)
        tb_DisposePtr(data);
    tb_DisposePtr(gwp);
    tb_DisposePtr(r);
    return out;
}

static int32_t do_export(grex *x, uint32_t type, gaddr written)
{
    if (written)
        wr32(written, 0);
    if (x->out == OUT_NONE)
        return paramErr;
    int w, h;
    gaddr pict;
    uint8_t *rgba = input_rgba(x, &w, &h, &pict);
    if (!rgba && !pict)
        return x->in == IN_NONE ? paramErr : cantFindHandler;
    uint8_t *bytes = NULL;
    size_t n = 0;
    if (type == PICT) {
        gaddr pic = pict ? pict : (rgba ? qt_picture_of_rgba(rgba, w, h) : 0);
        if (pic) {
            bytes = pict_file(pic, &n);
            if (pic != pict)
                tb_KillPicture(pic);
        }
    } else if (rgba && type == QTIF) {
        bytes = qtif_file(rgba, w, h, x->quality, &n);
    } else if (rgba) {
        double dpi = x->hres > 0 ? x->hres / 65536.0 : 72.0;
        if (!hostimage_encode(type, rgba, w, h, quality_of(x->quality), dpi, &bytes, &n))
            bytes = NULL;
    }
    free(rgba);
    if (pict)
        tb_DisposeHandle(pict);
    if (!bytes)
        return codecErr;
    uint32_t done;
    int16_t err = write_out(x, bytes, n, &done);
    free(bytes);
    if (!err && written)
        wr32(written, done);
    return err;
}

/* ---- The calls ---- */

static void free_in(grex *x)
{
    if (x->in_spec)
        tb_DisposePtr(x->in_spec);
    free(x->in_ref);
    x->in_spec = 0;
    x->in_ref = NULL;
    x->in = IN_NONE;
}

static void free_out(grex *x)
{
    if (x->out_spec)
        tb_DisposePtr(x->out_spec);
    free(x->out_ref);
    x->out_spec = 0;
    x->out_ref = NULL;
    x->out = OUT_NONE;
}

static gaddr copy_spec(gaddr spec)
{
    gaddr p = spec ? tb_NewPtr(FSSPEC, false, true) : 0;
    if (p)
        gmemcpy(p, spec, FSSPEC);
    return p;
}

static void put32(gaddr p, uint32_t v)
{
    if (p)
        wr32(p, v);
}

/* The input's natural size and an image description for it. */
static int16_t input_dimensions(grex *x, int *w, int *h)
{
    gaddr pict;
    uint8_t *rgba = input_rgba(x, w, h, &pict);
    free(rgba);
    if (pict)
        tb_DisposeHandle(pict);
    return *w > 0 && *h > 0 ? noErr : paramErr;
}

static int32_t grex_fn(qtc_call *k)
{
    grex *x = k->self;
    uint32_t type = k->cls->subtype;
    uint32_t *a = k->a;
    if (k->what == kQTCOpen) {
        x->quality = codecNormalQuality;
        x->hres = x->vres = 72 << 16;
        x->file_type = type;
        x->creator = fourcc("ogle"); /* PictureViewer's */
        x->out_truncate = true;
        return noErr;
    }
    if (k->what == kQTCClose) {
        free_in(x);
        free_out(x);
        return noErr;
    }
    if (k->what < 0 || !k->call)
        return qtcBadSelector;
    switch (k->what) {
    case 0x0001: return do_export(x, type, a[0]);                              /* DoExport */
    case 0x0002: if (a[0]) wr8(a[0], 0); return noErr;                         /* CanTranscode */
    case 0x0003: return codecUnimpErr;                                         /* DoTranscode */
    case 0x0004: if (a[0]) wr8(a[0], 0); return noErr;                         /* CanUseCompressor */
    case 0x0005: return codecUnimpErr;                                         /* DoUseCompressor */
    case 0x0006: return do_export(x, type, 0);                                 /* DoStandaloneExport */
    case 0x0007:                                                               /* GetDefaultFileTypeAndCreator */
        put32(a[0], type);
        put32(a[1], fourcc("ogle"));
        return noErr;
    case 0x0008: {                                                             /* GetDefaultFileNameExtension */
        int i = kind_of(type);
        uint8_t e[4] = { 0 };
        for (int j = 0; i >= 0 && j < 4 && kinds[i].ext[j]; j++)
            e[j] = (uint8_t)kinds[i].ext[j];
        put32(a[0], (uint32_t)e[0] << 24 | (uint32_t)e[1] << 16 | (uint32_t)e[2] << 8 | e[3]);
        return noErr;
    }
    case 0x0009: {                                                             /* GetMIMETypeList */
        int i = kind_of(type);
        int16_t err = noErr;
        gaddr c = qt_atoms_new();
        if (!c)
            return memFullErr;
        if (i >= 0)
            qt_atoms_insert(c, 0, fourcc("mime"), 1, 0, kinds[i].mime, (uint32_t)strlen(kinds[i].mime), &err);
        put32(a[0], c);
        return err;
    }
    case 0x000B: return noErr;                                                 /* RequestSettings: none to ask */
    case 0x000C: return noErr;                                                 /* SetSettingsFromAtomContainer */
    case 0x000D: {                                                             /* GetSettingsAsAtomContainer */
        gaddr c = qt_atoms_new();
        put32(a[0], c);
        return c ? noErr : memFullErr;
    }
    case 0x000E: {                                                             /* GetSettingsAsText */
        gaddr h = tb_NewHandle(0, false, false);
        put32(a[0], h);
        return h ? noErr : memFullErr;
    }
    case 0x000F: x->dont_recompress = a[0] & 0xFF; return noErr;               /* SetDontRecompress */
    case 0x0010: if (a[0]) wr8(a[0], x->dont_recompress); return noErr;        /* GetDontRecompress */
    case 0x0011: x->interlace = (int32_t)a[0]; return noErr;                   /* SetInterlaceStyle */
    case 0x0012: put32(a[0], (uint32_t)x->interlace); return noErr;
    case 0x0013: x->metadata = a[0]; return noErr;                             /* SetMetaData */
    case 0x0014: return noErr;                                                 /* GetMetaData: none of ours */
    case 0x0015: x->target_size = (int32_t)a[0]; return noErr;                 /* SetTargetDataSize */
    case 0x0016: put32(a[0], (uint32_t)x->target_size); return noErr;
    case 0x0017: x->method = (int32_t)a[0]; return noErr;                      /* SetCompressionMethod */
    case 0x0018: put32(a[0], (uint32_t)x->method); return noErr;
    case 0x0019: x->quality = (int32_t)a[0]; return noErr;                     /* SetCompressionQuality */
    case 0x001A: put32(a[0], (uint32_t)x->quality); return noErr;
    case 0x001B: x->hres = (int32_t)a[0]; x->vres = (int32_t)a[1]; return noErr; /* SetResolution */
    case 0x001C: put32(a[0], (uint32_t)x->hres); put32(a[1], (uint32_t)x->vres); return noErr;
    case 0x001D: x->depth = (int32_t)a[0]; return noErr;                       /* SetDepth */
    case 0x001E: put32(a[0], (uint32_t)(x->depth ? x->depth : 32)); return noErr;
    case 0x0021: x->colorsync = a[0]; return noErr;                            /* SetColorSyncProfile */
    case 0x0022: put32(a[0], 0); return noErr;
    case 0x0023:                                                               /* SetProgressProc */
        x->progress_proc = a[0] ? rd32(a[0]) : 0;
        x->progress_refcon = a[0] ? (int32_t)rd32(a[0] + 4) : 0;
        return noErr;
    case 0x0024:                                                               /* GetProgressProc */
        if (a[0]) {
            wr32(a[0], x->progress_proc);
            wr32(a[0] + 4, (uint32_t)x->progress_refcon);
        }
        return noErr;
    case 0x0025:                                                               /* SetInputDataReference(ref, type, desc) */
        free_in(x);
        x->in_ref = qt_dref_bytes(a[0], &x->in_ref_len);
        x->in_ref_type = a[1];
        x->in = IN_DATAREF;
        return noErr;
    case 0x0026:                                                               /* GetInputDataReference */
        put32(a[0], x->in == IN_DATAREF ? qt_dref_handle(x->in_ref, x->in_ref_len) : 0);
        put32(a[1], x->in == IN_DATAREF ? x->in_ref_type : 0);
        return x->in == IN_DATAREF ? noErr : paramErr;
    case 0x0027:                                                               /* SetInputFile(spec, desc) */
        free_in(x);
        x->in_spec = copy_spec(a[0]);
        x->in = x->in_spec ? IN_FILE : IN_NONE;
        return x->in_spec ? noErr : paramErr;
    case 0x0028:                                                               /* GetInputFile */
        if (x->in != IN_FILE)
            return paramErr;
        if (a[0])
            gmemcpy(a[0], x->in_spec, FSSPEC);
        return noErr;
    case 0x0029: free_in(x); x->in = IN_HANDLE; x->in_obj = a[0]; return noErr; /* SetInputHandle */
    case 0x002A: put32(a[0], x->in == IN_HANDLE ? x->in_obj : 0); return noErr;
    case 0x002B: free_in(x); x->in = IN_PTR; x->in_obj = a[0]; x->in_size = a[1]; return noErr; /* SetInputPtr */
    case 0x002C:
        put32(a[0], x->in == IN_PTR ? x->in_obj : 0);
        put32(a[1], x->in == IN_PTR ? x->in_size : 0);
        return noErr;
    case 0x002D: free_in(x); x->in = IN_IMPORTER; x->in_obj = a[0]; return noErr; /* SetInputGraphicsImporter */
    case 0x002E: put32(a[0], x->in == IN_IMPORTER ? x->in_obj : 0); return noErr;
    case 0x002F: free_in(x); x->in = IN_PICTURE; x->in_obj = a[0]; return noErr; /* SetInputPicture */
    case 0x0030: put32(a[0], x->in == IN_PICTURE ? x->in_obj : 0); return noErr;
    case 0x0031: free_in(x); x->in = IN_GWORLD; x->in_obj = a[0]; return noErr; /* SetInputGWorld */
    case 0x0032: put32(a[0], x->in == IN_GWORLD ? x->in_obj : 0); return noErr;
    case 0x0033: free_in(x); x->in = IN_PIXMAP; x->in_obj = a[0]; return noErr; /* SetInputPixmap */
    case 0x0034: put32(a[0], x->in == IN_PIXMAP ? x->in_obj : 0); return noErr;
    case 0x0035: x->in_offset = a[0]; x->in_limit = a[1]; return noErr;        /* SetInputOffsetAndLimit */
    case 0x0036: put32(a[0], x->in_offset); put32(a[1], x->in_limit); return noErr;
    case 0x0037: if (a[0]) wr8(a[0], x->in >= IN_HANDLE); return noErr;        /* MayExporterReadInputData */
    case 0x0038: {                                                             /* GetInputDataSize */
        size_t n;
        free(input_bytes(x, &n));
        put32(a[0], (uint32_t)n);
        return noErr;
    }
    case 0x0039: {                                                             /* ReadInputData(buf, offset, size) */
        size_t n;
        uint8_t *b = input_bytes(x, &n);
        if (!b)
            return paramErr;
        int16_t err = a[1] + a[2] <= n ? noErr : eofErr;
        if (!err)
            gput(a[0], b + a[1], a[2]);
        free(b);
        return err;
    }
    case 0x003A: {                                                             /* GetInputImageDescription */
        int w, h;
        if (input_dimensions(x, &w, &h))
            return paramErr;
        gaddr d = tb_NewHandle(86, false, true);
        if (!d)
            return memFullErr;
        gaddr p = rd32(d);
        wr32(p, 86);
        wr32(p + 4, 0x72617720);
        wr16(p + 32, (uint16_t)w);
        wr16(p + 34, (uint16_t)h);
        wr32(p + 36, 72u << 16);
        wr32(p + 40, 72u << 16);
        wr16(p + 48, 1);
        wr16(p + 82, 32);
        wr16(p + 84, 0xFFFF);
        put32(a[0], d);
        return noErr;
    }
    case 0x003B: {                                                             /* GetInputImageDimensions */
        int w, h;
        int16_t err = input_dimensions(x, &w, &h);
        if (!err && a[0])
            qrect_put(a[0], (qrect){ 0, 0, (int16_t)h, (int16_t)w });
        return err;
    }
    case 0x003C: put32(a[0], 32); return noErr;                                /* GetInputImageDepth */
    case 0x003D: {                                                             /* DrawInputImage(gw, gd, srcRect, dstRect) */
        int w, h;
        gaddr pict;
        uint8_t *rgba = input_rgba(x, &w, &h, &pict);
        if (pict)
            tb_DisposeHandle(pict);
        if (!rgba)
            return paramErr;
        gaddr saved = tb_NewPtr(8, false, true);
        tb_GetGWorld(saved, saved + 4);
        tb_SetGWorld(a[0], a[1]);
        qrect dst = a[3] ? qrect_get(a[3]) : (qrect){ 0, 0, (int16_t)h, (int16_t)w };
        qd_draw_rgba(rgba, w, h, dst);
        tb_SetGWorld(rd32(saved), rd32(saved + 4));
        tb_DisposePtr(saved);
        free(rgba);
        return noErr;
    }
    case 0x003E:                                                               /* SetOutputDataReference */
        free_out(x);
        x->out_ref = qt_dref_bytes(a[0], &x->out_ref_len);
        x->out_ref_type = a[1];
        x->out = OUT_DATAREF;
        return noErr;
    case 0x003F:
        put32(a[0], x->out == OUT_DATAREF ? qt_dref_handle(x->out_ref, x->out_ref_len) : 0);
        put32(a[1], x->out == OUT_DATAREF ? x->out_ref_type : 0);
        return x->out == OUT_DATAREF ? noErr : paramErr;
    case 0x0040:                                                               /* SetOutputFile */
        free_out(x);
        x->out_spec = copy_spec(a[0]);
        x->out = x->out_spec ? OUT_FILE : OUT_NONE;
        return x->out_spec ? noErr : paramErr;
    case 0x0041:
        if (x->out != OUT_FILE)
            return paramErr;
        if (a[0])
            gmemcpy(a[0], x->out_spec, FSSPEC);
        return noErr;
    case 0x0042: free_out(x); x->out = OUT_HANDLE; x->out_h = a[0]; return noErr; /* SetOutputHandle */
    case 0x0043: put32(a[0], x->out == OUT_HANDLE ? x->out_h : 0); return noErr;
    case 0x0044:                                                               /* SetOutputOffsetAndMaxSize */
        x->out_offset = a[0];
        x->out_max = a[1];
        x->out_truncate = a[2] & 0xFF;
        return noErr;
    case 0x0045:
        put32(a[0], x->out_offset);
        put32(a[1], x->out_max);
        if (a[2])
            wr8(a[2], x->out_truncate);
        return noErr;
    case 0x0046: x->file_type = a[0]; x->creator = a[1]; return noErr;          /* SetOutputFileTypeAndCreator */
    case 0x0047: put32(a[0], x->file_type); put32(a[1], x->creator); return noErr;
    case 0x0048: {                                                             /* WriteOutputData(buffer, size) */
        uint8_t *b = malloc(a[1] ? a[1] : 1);
        if (!b)
            return memFullErr;
        gget(b, a[0], a[1]);
        uint32_t saved = x->out_offset, done;
        bool trunc = x->out_truncate;
        x->out_offset = x->out_mark ? x->out_mark : x->out_offset;
        x->out_truncate = false;
        int16_t err = write_out(x, b, a[1], &done);
        x->out_offset = saved;
        x->out_truncate = trunc;
        free(b);
        return err;
    }
    case 0x0049: x->out_mark = a[0]; return noErr;                             /* SetOutputMark */
    case 0x004A: put32(a[0], x->out_mark); return noErr;                       /* GetOutputMark */
    case 0x004B: {                                                             /* ReadOutputData(buffer, offset, size) */
        if (x->out != OUT_HANDLE)
            return paramErr;
        if (a[1] + a[2] > (uint32_t)tb_GetHandleSize(x->out_h))
            return eofErr;
        gmemcpy(a[0], rd32(x->out_h) + a[1], a[2]);
        return noErr;
    }
    }
    return qtcBadSelector;
}

static qtc_class classes[NKINDS];

void qt_gexport_register(void)
{
    for (size_t i = 0; i < NKINDS; i++) {
        classes[i] = (qtc_class){ GREX, kinds[i].type, APPL, 0, kinds[i].name, kinds[i].name, 0x00010000,
                                  { "GraphicsExport" }, sizeof(grex), grex_fn, 0 };
        if (can_write(kinds[i].type))
            qtc_register(&classes[i]);
        else
            classes[i].component = 0;
    }
}

/* GraphicsImportExportImageFile: the exporter for the type, fed by the importer. */
int32_t qt_gexport_importer(uint32_t gi, uint32_t file_type, uint32_t creator, gaddr spec, int16_t script)
{
    uint32_t c = file_type ? components_find_next(0, GREX, file_type, 0) : 0;
    if (!c)
        return cantFindHandler;
    int16_t err;
    uint32_t ci = components_open_component(c, &err);
    if (!ci)
        return cantOpenHandler;
    uint32_t in[1] = { gi }, out[1] = { spec }, tc[2] = { file_type, creator ? creator : fourcc("ogle") };
    uint32_t done[1] = { 0 };
    int32_t r = qtc_call_named(ci, "GraphicsExportSetInputGraphicsImporter", in);
    if (!r)
        r = qtc_call_named(ci, "GraphicsExportSetOutputFile", out);
    if (!r)
        r = qtc_call_named(ci, "GraphicsExportSetOutputFileTypeAndCreator", tc);
    if (!r)
        r = qtc_call_named(ci, "GraphicsExportDoExport", done);
    components_close(ci);
    (void)script;
    return r;
}

/* DoExportImageFileDialog: Standard File's put dialog for the name; the
 * type from its extension (PICT when it has none we know). */
int32_t qt_gexport_dialog(uint32_t gi, gaddr default_spec, gaddr prompt, gaddr out_type, gaddr out_spec,
                          gaddr out_script)
{
    gaddr reply = tb_NewPtr(88, false, true), name = tb_NewPtr(64, false, true);
    if (!reply || !name)
        return memFullErr;
    if (default_spec)
        gmemcpy(name, default_spec + 6, 64);
    else
        gput(name, "\x0BUntitled.pct", 13);
    gaddr p = prompt;
    if (!p) {
        p = tb_NewPtr(32, false, true);
        gput(p, "\x0FSave image as:", 16);
    }
    tb_StandardPutFile(p, name, reply);
    if (p != prompt)
        tb_DisposePtr(p);
    int32_t r = userCanceledErr;
    if (rd8(reply)) { /* sfGood */
        gaddr spec = reply + 6;
        uint8_t n = rd8(spec + 6);
        char fname[64] = { 0 };
        gget(fname, spec + 7, n);
        const char *dot = strrchr(fname, '.');
        uint32_t type = PICT;
        for (size_t i = 0; dot && i < NKINDS; i++)
            if (!strcasecmp(dot + 1, kinds[i].ext) && can_write(kinds[i].type))
                type = kinds[i].type;
        if (dot && (!strcasecmp(dot + 1, "jpeg")))
            type = JPEGT;
        r = qt_gexport_importer(gi, type, 0, spec, (int16_t)rd16(reply + 76));
        if (!r) {
            put32(out_type, type);
            if (out_spec)
                gmemcpy(out_spec, spec, FSSPEC);
            if (out_script)
                wr16(out_script, rd16(reply + 76));
        }
    }
    tb_DisposePtr(reply);
    tb_DisposePtr(name);
    return r;
}
