/*
 * QuickTime's graphics importers ('grip' components): a still image from a
 * file or a handle, its size and description, drawn into a port or a
 * GWorld, or made into a picture.  GIF, JPEG, PNG, BMP, TIFF and the like
 * are decoded by the host's image decoder; a PICT file is its picture,
 * drawn by DrawPicture.  One component implements every type, reached by
 * ComponentDispatch from 68k programs and by QuickTimeLib's GraphicsImport
 * calls from PowerPC ones (Super Snake, Cro-Mag Rally).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m68k.h"
#include "machine.h"
#include "ppc.h"
#include "ppc_bridge.h"
#include "qd_internal.h"
#include "qt_codec.h"
#include "tb_internal.h"
#include "trap.h"

enum {
    cantFindHandler = -2003,
    badComponentInstance = (int32_t)0x80008001,
    badComponentSelector = (int32_t)0x80008002,
    srcCopy = 0,
    codecNormalQuality = 0x200,
};

/* An image's kind, from its first bytes. */
enum { KIND_NONE, KIND_PICT, KIND_HOST };

#define MAX_IMPORTERS 32
typedef struct {
    uint32_t ci;          /* 0: free */
    uint8_t *data;        /* the image's bytes */
    size_t len;
    int kind;
    uint32_t type;        /* the codec type it describes itself with */
    gaddr spec;           /* the file (an FSSpec of ours), or 0 */
    gaddr handle;         /* the handle it was given, or 0 */
    uint8_t *rgba;        /* decoded, when drawn through the host */
    int w, h;
    qrect bounds, source; /* where it draws, and the part of it drawn */
    gaddr port, gdh;      /* SetGWorld's, or 0 for the current port */
    gaddr clip;
    int32_t mode;
    uint16_t op[3];
    uint32_t quality;
} importer;
static importer importers[MAX_IMPORTERS];
static uint32_t grip_component;

/* The instance's importer; a free one for 0. */
static importer *importer_of(uint32_t ci)
{
    for (int i = 0; i < MAX_IMPORTERS; i++)
        if (importers[i].ci == ci)
            return &importers[i];
    return NULL;
}

static void forget_data(importer *g)
{
    free(g->data);
    free(g->rgba);
    g->data = g->rgba = NULL;
    g->len = 0;
    g->kind = KIND_NONE;
    g->w = g->h = 0;
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* A PICT file: a 512-byte header, then the picture (size, frame, and the
 * version opcode of either version). */
static bool is_pict(const uint8_t *d, size_t len)
{
    if (len < 512 + 12)
        return false;
    const uint8_t *p = d + 512;
    return (p[10] == 0x11 && p[11] == 0x01) || (be16(p + 10) == 0x0011 && be16(p + 12) == 0x02FF);
}

static uint32_t host_type(const uint8_t *d, size_t len)
{
    if (len >= 4 && !memcmp(d, "GIF8", 4))
        return 0x67696620; /* 'gif ' */
    if (len >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)
        return 0x6A706567; /* 'jpeg' */
    if (len >= 8 && !memcmp(d, "\x89PNG\r\n\x1A\n", 8))
        return 0x706E6720; /* 'png ' */
    if (len >= 2 && d[0] == 'B' && d[1] == 'M')
        return 0x57524C45; /* 'WRLE', QuickTime's BMP codec */
    if (len >= 4 && (!memcmp(d, "II*\0", 4) || !memcmp(d, "MM\0*", 4)))
        return 0x74696666; /* 'tiff' */
    if (len >= 4 && !memcmp(d, "8BPS", 4))
        return 0x38425053; /* '8BPS' */
    return 0;
}

/* Take the image's bytes, recognising its kind; the natural bounds. */
static int16_t set_data(importer *g, uint8_t *data, size_t len)
{
    forget_data(g);
    g->data = data;
    g->len = len;
    if (is_pict(data, len)) {
        g->kind = KIND_PICT;
        g->type = 0x70696374; /* 'pict' */
        const uint8_t *p = data + 512;
        qrect f = { (int16_t)be16(p + 2), (int16_t)be16(p + 4), (int16_t)be16(p + 6), (int16_t)be16(p + 8) };
        g->w = f.right - f.left;
        g->h = f.bottom - f.top;
    } else if ((g->type = host_type(data, len)) != 0 && qt_decode_image(data, len, &g->rgba, &g->w, &g->h)) {
        g->kind = KIND_HOST;
    } else {
        forget_data(g);
        return cantFindHandler;
    }
    g->bounds = g->source = (qrect){ 0, 0, (int16_t)g->h, (int16_t)g->w };
    return noErr;
}

/* The file's data fork, whole. */
static uint8_t *read_spec(gaddr spec, size_t *len)
{
    char path[1024];
    if (!spec || !fm_host_path((int16_t)rd16(spec), (int32_t)rd32(spec + 2), spec + 6, true, path, sizeof path)) {
        LOGF(LOG_TRAPS, "  graphics importer: no file at vref %d dir %d\n", spec ? (int16_t)rd16(spec) : 0,
             spec ? (int32_t)rd32(spec + 2) : 0);
        return NULL;
    }
    LOGF(LOG_TRAPS, "  graphics importer for %s\n", path);
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t *d = NULL;
    if (!fseek(f, 0, SEEK_END)) {
        long n = ftell(f);
        if (n > 0 && n < 64L * 1024 * 1024 && !fseek(f, 0, SEEK_SET) && (d = malloc((size_t)n)) != NULL &&
            fread(d, 1, (size_t)n, f) != (size_t)n) {
            free(d);
            d = NULL;
        }
        *len = d ? (size_t)n : 0;
    }
    fclose(f);
    return d;
}

static int16_t set_file(importer *g, gaddr spec)
{
    size_t len = 0;
    uint8_t *d = read_spec(spec, &len);
    if (!d)
        return fnfErr;
    int16_t err = set_data(g, d, len);
    if (!err) {
        if (!g->spec)
            g->spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
        if (g->spec)
            gmemcpy(g->spec, spec, FSSpec_SIZEOF);
        g->handle = 0;
    }
    return err;
}

static int16_t set_handle(importer *g, gaddr h)
{
    int32_t n = h && rd32(h) ? tb_GetHandleSize(h) : 0;
    if (n <= 0)
        return paramErr;
    uint8_t *d = malloc((size_t)n);
    if (!d)
        return memFullErr;
    gget(d, rd32(h), (uint32_t)n);
    int16_t err = set_data(g, d, (size_t)n);
    if (!err)
        g->handle = h;
    return err;
}

/* The picture of a PICT file, in a new handle. */
static gaddr pict_handle(importer *g)
{
    gaddr h = tb_NewHandle((int32_t)(g->len - 512), false, false);
    if (h && rd32(h))
        gput(rd32(h), g->data + 512, (uint32_t)(g->len - 512));
    return h;
}

/* The decoded image's source rectangle, alone. */
static uint8_t *cropped(importer *g, int *w, int *h)
{
    qrect s = g->source;
    if (s.left < 0)
        s.left = 0;
    if (s.top < 0)
        s.top = 0;
    if (s.right > g->w)
        s.right = (int16_t)g->w;
    if (s.bottom > g->h)
        s.bottom = (int16_t)g->h;
    *w = s.right - s.left;
    *h = s.bottom - s.top;
    if (*w <= 0 || *h <= 0)
        return NULL;
    uint8_t *out = malloc((size_t)*w * (size_t)*h * 4);
    for (int y = 0; out && y < *h; y++)
        memcpy(out + (size_t)y * (size_t)*w * 4, g->rgba + ((size_t)(s.top + y) * (size_t)g->w + (size_t)s.left) * 4,
               (size_t)*w * 4);
    return out;
}

static void put_rect(gaddr r, qrect q)
{
    if (r)
        qrect_put(r, q);
}

static int16_t draw(importer *g)
{
    if (g->kind == KIND_NONE)
        return paramErr;
    gaddr saved = tb_NewPtr(8, false, true);
    if (!saved)
        return memFullErr;
    tb_GetGWorld(saved, saved + 4);
    if (g->port)
        tb_SetGWorld(g->port, g->gdh);
    gaddr r = tb_NewPtr(8, false, true);
    if (r) {
        put_rect(r, g->bounds);
        if (g->kind == KIND_PICT) {
            gaddr pic = pict_handle(g);
            if (pic) {
                tb_DrawPicture(pic, r);
                tb_DisposeHandle(pic);
            }
        } else {
            int w, h;
            uint8_t *part = cropped(g, &w, &h);
            if (part)
                qd_draw_rgba(part, w, h, g->bounds);
            free(part);
        }
        tb_DisposePtr(r);
    }
    tb_SetGWorld(rd32(saved), rd32(saved + 4));
    tb_DisposePtr(saved);
    return noErr;
}

/* GetAsPicture: a PICT file's own picture, or the image copied from an
 * offscreen GWorld into a new picture of its natural size. */
static int16_t as_picture(importer *g, gaddr out)
{
    if (!out)
        return paramErr;
    wr32(out, 0);
    if (g->kind == KIND_PICT) {
        gaddr pic = pict_handle(g);
        wr32(out, pic);
        return pic ? noErr : memFullErr;
    }
    if (g->kind != KIND_HOST)
        return paramErr;
    gaddr tmp = tb_NewPtr(32, false, true);
    if (!tmp)
        return memFullErr;
    gaddr r = tmp, gw = tmp + 8, saved = tmp + 12;
    qrect natural = { 0, 0, (int16_t)g->h, (int16_t)g->w };
    put_rect(r, natural);
    tb_GetGWorld(saved, saved + 4);
    int16_t err = tb_NewGWorld(gw, 32, r, 0, 0, 0);
    if (!err) {
        tb_LockPixels(tb_GetGWorldPixMap(rd32(gw)));
        tb_SetGWorld(rd32(gw), 0);
        qd_draw_rgba(g->rgba, g->w, g->h, natural);
        tb_SetGWorld(rd32(saved), rd32(saved + 4));
        gaddr pic = tb_OpenPicture(r);
        if (pic) {
            gaddr pm = tb_GetGWorldPixMap(rd32(gw));
            tb_CopyBits(rd32(pm), rd32(saved) + 2, r, r, srcCopy, 0); /* to the port's bits */
            tb_ClosePicture();
            wr32(out, pic);
        } else {
            err = memFullErr;
        }
        tb_DisposeGWorld(rd32(gw));
    }
    tb_SetGWorld(rd32(saved), rd32(saved + 4));
    tb_DisposePtr(tmp);
    return err;
}

/* An ImageDescription for the image, in a new handle. */
static int16_t description(importer *g, gaddr out)
{
    if (!out)
        return paramErr;
    gaddr h = tb_NewHandle(86, false, true);
    wr32(out, h);
    if (!h || !rd32(h))
        return memFullErr;
    gaddr d = rd32(h);
    wr32(d, 86);
    wr32(d + 4, g->type);
    wr32(d + 20, 0x6170706C); /* 'appl' */
    wr16(d + 32, (uint16_t)g->w);
    wr16(d + 34, (uint16_t)g->h);
    wr32(d + 36, 72 << 16);
    wr32(d + 40, 72 << 16);
    wr32(d + 44, (uint32_t)g->len);
    wr16(d + 48, 1);
    wr16(d + 82, 32);
    wr16(d + 84, 0xFFFF);
    return noErr;
}

static qrect get_rect(gaddr r) { return r ? qrect_get(r) : (qrect){ 0 }; }

/* The calls, the arguments in order after the instance (each a long). */
static int32_t gi_call(importer *g, int16_t what, const uint32_t *a)
{
    switch (what) {
    case 0x03: return set_file(g, a[0]);                                 /* SetDataFile */
    case 0x04:                                                           /* GetDataFile */
        if (!g->spec)
            return paramErr;
        if (a[0])
            gmemcpy(a[0], g->spec, FSSpec_SIZEOF);
        return noErr;
    case 0x05: return set_handle(g, a[0]);                               /* SetDataHandle */
    case 0x06:                                                           /* GetDataHandle */
        if (a[0])
            wr32(a[0], g->handle);
        return g->handle ? noErr : paramErr;
    case 0x07: return description(g, a[0]);                              /* GetImageDescription */
    case 0x0A: g->clip = a[0]; return noErr;                             /* SetClip */
    case 0x0B: if (a[0]) wr32(a[0], g->clip); return noErr;              /* GetClip */
    case 0x0C: g->source = get_rect(a[0]); return noErr;                 /* SetSourceRect */
    case 0x0D: put_rect(a[0], g->source); return noErr;                  /* GetSourceRect */
    case 0x0E: put_rect(a[0], (qrect){ 0, 0, (int16_t)g->h, (int16_t)g->w }); return noErr; /* GetNaturalBounds */
    case 0x0F: return draw(g);                                           /* Draw */
    case 0x10: g->port = a[0]; g->gdh = a[1]; return noErr;              /* SetGWorld */
    case 0x11:                                                           /* GetGWorld */
        if (!g->port) {
            gaddr t = tb_NewPtr(8, false, true);
            if (t) {
                tb_GetGWorld(t, t + 4);
                if (a[0])
                    wr32(a[0], rd32(t));
                if (a[1])
                    wr32(a[1], rd32(t + 4));
                tb_DisposePtr(t);
            }
            return noErr;
        }
        if (a[0])
            wr32(a[0], g->port);
        if (a[1])
            wr32(a[1], g->gdh);
        return noErr;
    case 0x14: g->bounds = get_rect(a[0]); return noErr;                 /* SetBoundsRect */
    case 0x15: put_rect(a[0], g->bounds); return noErr;                  /* GetBoundsRect */
    case 0x17:                                                           /* SetGraphicsMode */
        g->mode = (int32_t)a[0];
        if (a[1])
            for (int k = 0; k < 3; k++)
                g->op[k] = rd16(a[1] + 2u * (uint32_t)k);
        return noErr;
    case 0x18:                                                           /* GetGraphicsMode */
        if (a[0])
            wr32(a[0], (uint32_t)g->mode);
        for (int k = 0; a[1] && k < 3; k++)
            wr16(a[1] + 2u * (uint32_t)k, g->op[k]);
        return noErr;
    case 0x19: g->quality = a[0]; return noErr;                          /* SetQuality */
    case 0x1A: if (a[0]) wr32(a[0], g->quality); return noErr;           /* GetQuality */
    case 0x1F: if (a[0]) wr8(a[0], g->kind != KIND_NONE); return noErr;  /* Validate */
    case 0x22: if (a[0]) wr16(a[0], g->kind == KIND_HOST); return noErr; /* DoesDrawAllPixels */
    case 0x23: return as_picture(g, a[0]);                               /* GetAsPicture */
    case 0x2B: if (a[0]) wr32(a[0], 1); return noErr;                    /* GetImageCount */
    case 0x2C: return a[0] == 1 ? noErr : paramErr;                      /* SetImageIndex */
    case 0x2D: if (a[0]) wr32(a[0], 1); return noErr;                    /* GetImageIndex */
    case 0x24: /* ExportImageFile(fileType, fileCreator, fileSpec, scriptTag) */
        return qt_gexport_importer(g->ci, a[0], a[1], a[2], (int16_t)a[3]);
    case 0x26: /* DoExportImageFileDialog(inDefaultSpec, prompt, filterProc, outType*, outSpec*, outScript*) */
        return qt_gexport_dialog(g->ci, a[0], a[1], a[3], a[4], a[5]);
    default: return badComponentSelector;
    }
}

/* The selectors whose arguments are words: SetSourceRect and the rest
 * take only longs (pointers), but these fit the long-argument table too
 * since every one listed above is a long. */
static bool can_do(int16_t what)
{
    static const int16_t done[] = { 3, 4, 5, 6, 7, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x14, 0x15,
                                    0x17, 0x18, 0x19, 0x1A, 0x1F, 0x22, 0x23, 0x24, 0x26, 0x2B, 0x2C, 0x2D };
    for (size_t i = 0; i < sizeof done / sizeof done[0]; i++)
        if (done[i] == what)
            return true;
    return false;
}

/* pascal ComponentResult (ComponentParameters *params, Handle storage) */
static void grip_entry(cpu68k *c, uint16_t tw)
{
    (void)tw;
    uint32_t ret = cpu_pop32(c);
    cpu_pop32(c); /* storage */
    gaddr params = cpu_pop32(c);
    int16_t what = (int16_t)rd16(params + 2);
    uint32_t size = rd8(params + 1);
    int32_t r = noErr;
    if (what == -1) { /* open */
        importer *g = importer_of(0);
        if (!g)
            r = memFullErr;
        else
            *g = (importer){ .ci = rd32(params + 4), .mode = srcCopy, .quality = codecNormalQuality };
    } else if (what == -2) { /* close */
        importer *g = rd32(params + 4) ? importer_of(rd32(params + 4)) : NULL;
        if (g) {
            forget_data(g);
            if (g->spec)
                tb_DisposePtr(g->spec);
            memset(g, 0, sizeof *g);
        }
    } else if (what == -3) { /* can do */
        int16_t s = (int16_t)rd16(params + 4);
        r = (s >= -4 && s <= -1) || can_do(s);
    } else if (what == -4) { /* version */
        r = 0x00020000;
    } else if (what < 0) {
        r = badComponentSelector;
    } else {
        uint32_t ci = rd32(params + 4 + size);
        importer *g = ci ? importer_of(ci) : NULL;
        uint32_t a[6] = { 0 };
        for (uint32_t i = 0; i < 6 && 4 * (i + 1) <= size; i++)
            a[i] = rd32(params + 4 + size - 4 * (i + 1));
        if (what == 0x24 && size == 14) /* ExportImageFile ends with a word, the script */
            a[3] = rd16(params + 4);
        r = g ? gi_call(g, what, a) : badComponentInstance;
    }
    wr32(c->a[7], (uint32_t)r);
    cpu_push32(c, ret);
}

/* A new importer for the file (GetGraphicsImporterForFile), or for the
 * data a handle data reference names (GetGraphicsImporterForDataRef). */
static int16_t open_importer(gaddr spec, gaddr data_handle, gaddr out)
{
    if (out)
        wr32(out, 0);
    uint32_t ci = grip_component ? components_open(grip_component) : 0;
    importer *g = ci ? importer_of(ci) : NULL;
    if (!g)
        return cantFindHandler;
    int16_t err = spec ? set_file(g, spec) : set_handle(g, data_handle);
    LOGF(LOG_TRAPS, "  graphics importer %08X: %d, %dx%d\n", ci, err, g->w, g->h);
    if (err) {
        components_close(ci);
        return err == fnfErr ? fnfErr : cantFindHandler;
    }
    if (out)
        wr32(out, ci);
    return noErr;
}

int16_t tb_GetGraphicsImporterForFile(gaddr theFile, gaddr gi) { return open_importer(theFile, 0, gi); }
int16_t tb_GetGraphicsImporterForFileWithFlags(gaddr theFile, gaddr gi, int32_t flags)
{
    (void)flags;
    return open_importer(theFile, 0, gi);
}

/* A handle data reference ('hndl') is a handle holding the data's handle. */
int16_t tb_GetGraphicsImporterForDataRef(int32_t dataRef, int32_t dataRefType, gaddr gi)
{
    if ((uint32_t)dataRefType != 0x686E646C /* 'hndl' */ || !dataRef || !rd32((gaddr)dataRef)) {
        if (gi)
            wr32(gi, 0);
        return cantFindHandler;
    }
    return open_importer(0, rd32(rd32((gaddr)dataRef)), gi);
}
int16_t tb_GetGraphicsImporterForDataRefWithFlags(int32_t dataRef, int32_t dataRefType, gaddr gi, int32_t flags)
{
    (void)flags;
    return tb_GetGraphicsImporterForDataRef(dataRef, dataRefType, gi);
}

/* A PowerPC program's component call: through ComponentDispatch as the
 * 68k glue makes it, the instance in r3, then the arguments of the sizes
 * given ("4" a long, "2" a word: a short or a Boolean). */
void qt_component_call_ppc(ppc_cpu *c, int16_t what, const char *sizes)
{
    ppc_trap_begin();
    ppc_trap_push(4, 0);
    ppc_trap_push(4, c->gpr[3]);
    uint32_t size = 0;
    for (int i = 0; sizes[i]; i++) {
        int n = sizes[i] - '0';
        ppc_trap_push(n, c->gpr[4 + i]);
        size += (uint32_t)n;
    }
    ppc_trap_push(4, size << 16 | (uint16_t)what);
    ppc_trap_set(0, 0);
    ppc_trap(0xA82A);
    uint32_t r = ppc_trap_result(4);
    ppc_trap_end();
    c->gpr[3] = r;
    c->pc = c->lr;
}

#define GI(name, what, sizes) static void gi_##name(ppc_cpu *c) { qt_component_call_ppc(c, what, sizes); }
GI(SetDataFile, 0x03, "4")
GI(GetDataFile, 0x04, "4")
GI(SetDataHandle, 0x05, "4")
GI(GetDataHandle, 0x06, "4")
GI(GetImageDescription, 0x07, "4")
GI(SetClip, 0x0A, "4")
GI(GetClip, 0x0B, "4")
GI(SetSourceRect, 0x0C, "4")
GI(GetSourceRect, 0x0D, "4")
GI(GetNaturalBounds, 0x0E, "4")
GI(Draw, 0x0F, "")
GI(SetGWorld, 0x10, "44")
GI(GetGWorld, 0x11, "44")
GI(SetBoundsRect, 0x14, "4")
GI(GetBoundsRect, 0x15, "4")
GI(SetGraphicsMode, 0x17, "44")
GI(GetGraphicsMode, 0x18, "44")
GI(SetQuality, 0x19, "4")
GI(GetQuality, 0x1A, "4")
GI(Validate, 0x1F, "4")
GI(DoesDrawAllPixels, 0x22, "4")
GI(GetAsPicture, 0x23, "4")
GI(ExportImageFile, 0x24, "4442")
GI(DoExportImageFileDialog, 0x26, "444444")
GI(GetImageCount, 0x2B, "4")
GI(SetImageIndex, 0x2C, "4")
GI(GetImageIndex, 0x2D, "4")
#undef GI

const ppc_export gimport_exports[] = {
#define E(name) { "GraphicsImport" #name, gi_##name }
    E(SetDataFile), E(GetDataFile), E(SetDataHandle), E(GetDataHandle), E(GetImageDescription),
    E(SetClip), E(GetClip), E(SetSourceRect), E(GetSourceRect), E(GetNaturalBounds), E(Draw),
    E(SetGWorld), E(GetGWorld), E(SetBoundsRect), E(GetBoundsRect), E(SetGraphicsMode),
    E(GetGraphicsMode), E(SetQuality), E(GetQuality), E(Validate), E(DoesDrawAllPixels),
    E(GetAsPicture), E(ExportImageFile), E(DoExportImageFileDialog), E(GetImageCount),
    E(SetImageIndex), E(GetImageIndex),
#undef E
};
const unsigned gimport_export_count = sizeof gimport_exports / sizeof gimport_exports[0];

bool qt_gimport_image(uint32_t ci, uint8_t **rgba, int *w, int *h, gaddr *pict)
{
    importer *g = ci ? importer_of(ci) : NULL;
    *rgba = NULL;
    *pict = 0;
    if (!g || g->kind == KIND_NONE)
        return false;
    *w = g->w;
    *h = g->h;
    if (g->kind == KIND_PICT) {
        *pict = pict_handle(g);
        return *pict != 0;
    }
    *rgba = malloc((size_t)g->w * (size_t)g->h * 4);
    if (*rgba)
        memcpy(*rgba, g->rgba, (size_t)g->w * (size_t)g->h * 4);
    return *rgba != NULL;
}

/* A new run: no importers; the component registered (components_reset). */
void qt_gimport_register(void)
{
    for (int i = 0; i < MAX_IMPORTERS; i++) {
        free(importers[i].data);
        free(importers[i].rgba);
    }
    memset(importers, 0, sizeof importers);
    grip_component = components_register_native(0x67726970 /* 'grip' */, 0, 0x6170706C /* 'appl' */,
                                                native_register(grip_entry, "GraphicsImporter", 0));
}
