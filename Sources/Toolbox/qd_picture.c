/*
 * QuickDraw pictures: DrawPicture for version 1 and 2 PICTs.
 *
 * Opcodes are played back through the ordinary drawing calls in the current
 * port, mapped from the picture frame to the destination rectangle.  Bitmap
 * and pixmap opcodes are decoded to RGB, dithered for the 1-bit screen, and
 * recorded in full colour when the port is printing.
 *
 * Drawing between OpenPicture and ClosePicture is recorded (qd_picsave.c).
 */
#include <stdlib.h>
#include <string.h>

#include "m68k.h"
#include "machine.h"
#include "qd_internal.h"
#include "tb_internal.h"
#include "trap.h"

enum { srcCopy = 0 };

/* A picture's data in guest memory, from p to end.  A picture spooled in
 * through the port's getPicProc (the handle holds only picSize and
 * picFrame) is read into a buffer, base to cap, as it is needed; end is
 * how far it has been filled. */
typedef struct {
    gaddr p, end;
    bool v2;
    gaddr proc, base, cap;
} reader;

/* Make n bytes from p available, reading them through getPicProc. */
static void need(reader *r, uint32_t n)
{
    if (!r->proc || r->p + n <= r->end)
        return;
    uint32_t want = r->p + n - r->end;
    if (want > r->cap - r->end)
        want = r->cap - r->end;
    while (want) {
        uint16_t k = want > 0x7FFF ? 0x7FFF : (uint16_t)want;
        cpu_push32(g_cpu, r->end);
        cpu_push16(g_cpu, k);
        guest_call(r->proc);
        r->end += k;
        want -= k;
    }
}

/* Between opcodes, what is still unread moves to the start of the buffer
 * (keeping its alignment) once less than half of it is left. */
static void compact(reader *r)
{
    if (!r->proc || r->cap - r->p >= (r->cap - r->base) / 2)
        return;
    uint32_t shift = (r->p - r->base) & ~1u;
    gmemcpy(r->base, r->base + shift, r->end - r->base - shift);
    r->p -= shift;
    r->end -= shift;
}

static int r8(reader *r)
{
    if (r->p >= r->end)
        need(r, 1);
    return r->p < r->end ? rd8(r->p++) : 0;
}
static int r16(reader *r)
{
    int v = (int16_t)(r8(r) << 8);
    return (int16_t)(v | r8(r));
}
static uint32_t r32(reader *r)
{
    uint32_t hi = (uint16_t)r16(r);
    return hi << 16 | (uint16_t)r16(r);
}
static void skip(reader *r, uint32_t n)
{
    need(r, n);
    r->p = r->p + n < r->end ? r->p + n : r->end;
}
static qrect rrect(reader *r)
{
    qrect q;
    q.top = (int16_t)r16(r);
    q.left = (int16_t)r16(r);
    q.bottom = (int16_t)r16(r);
    q.right = (int16_t)r16(r);
    return q;
}
static uint32_t rpt(reader *r)
{
    int v = r16(r), h = r16(r);
    return make_pt((int16_t)v, (int16_t)h);
}

/* ---------------------------------------------------------------------- */
/* Mapping from the picture frame to the destination */

typedef struct {
    qrect src, dst;
} mapping;

static int map_h(const mapping *m, int h)
{
    int sw = m->src.right - m->src.left, dw = m->dst.right - m->dst.left;
    return sw ? m->dst.left + (h - m->src.left) * dw / sw : m->dst.left;
}
static int map_v(const mapping *m, int v)
{
    int sh = m->src.bottom - m->src.top, dh = m->dst.bottom - m->dst.top;
    return sh ? m->dst.top + (v - m->src.top) * dh / sh : m->dst.top;
}
static qrect map_rect(const mapping *m, qrect q)
{
    return (qrect){ (int16_t)map_v(m, q.top), (int16_t)map_h(m, q.left), (int16_t)map_v(m, q.bottom),
                    (int16_t)map_h(m, q.right) };
}
static int scale_h(const mapping *m, int d)
{
    int sw = m->src.right - m->src.left;
    return sw ? d * (m->dst.right - m->dst.left) / sw : d;
}
static int scale_v(const mapping *m, int d)
{
    int sh = m->src.bottom - m->src.top;
    return sh ? d * (m->dst.bottom - m->dst.top) / sh : d;
}

/* ---------------------------------------------------------------------- */
/* Regions and polygons in the stream */

static void read_region(reader *r, const mapping *m, hrgn *out)
{
    gaddr start = r->p;
    int size = (uint16_t)r16(r);
    if (size < 10)
        size = 10;
    r->p = start;
    need(r, (uint32_t)size);
    gaddr h = tb_NewHandle(size, false, false);
    gmemcpy(rd32(h), start, (uint32_t)size);
    r->p = start;
    skip(r, (uint32_t)size);
    rgn_read(h, out);
    tb_DisposeHandle(h);
    /* Map: exact for offsets, by bounding box when scaled. */
    qrect bb = hrgn_bbox(out);
    qrect mb = map_rect(m, bb);
    if (mb.right - mb.left == bb.right - bb.left && mb.bottom - mb.top == bb.bottom - bb.top) {
        hrgn_offset(out, mb.left - bb.left, mb.top - bb.top);
    } else {
        hrgn_free(out);
        hrgn_init(out);
        hrgn_rect(out, mb);
    }
}

typedef struct {
    int n;
    int16_t (*pts)[2]; /* h, v (mapped) */
} poly;

static void read_poly(reader *r, const mapping *m, poly *out)
{
    int size = (uint16_t)r16(r);
    rrect(r); /* bounding box */
    int n = (size - 10) / 4;
    free(out->pts);
    out->n = n > 0 ? n : 0;
    out->pts = malloc(sizeof *out->pts * (size_t)(out->n ? out->n : 1));
    for (int i = 0; i < out->n; i++) {
        int v = r16(r), h = r16(r);
        out->pts[i][0] = (int16_t)map_h(m, h);
        out->pts[i][1] = (int16_t)map_v(m, v);
    }
}

/* Scan-convert a polygon (even-odd) into a region, a row at a time as
 * PaintPoly does (a union per row was quadratic in the rows: a tall
 * polygon in a picture took minutes). */
static void poly_region(const poly *pl, hrgn *out)
{
    hrgn_init(out);
    if (pl->n < 3)
        return;
    int (*e)[4] = malloc((size_t)pl->n * sizeof *e);
    for (int i = 0; i < pl->n; i++) {
        int j = (i + 1) % pl->n;
        e[i][0] = pl->pts[i][0];
        e[i][1] = pl->pts[i][1];
        e[i][2] = pl->pts[j][0];
        e[i][3] = pl->pts[j][1];
    }
    hrgn_even_odd(out, (const int (*)[4])e, pl->n);
    free(e);
}

/* ---------------------------------------------------------------------- */
/* Pixels */

typedef struct {
    int w, h;
    uint8_t *rgba;
} image;

/* Expand PackBits; unit is 1 (bytes) or 2 (16-bit pixels). */
static void unpack_bits(reader *r, uint8_t *out, int n, int unit)
{
    int k = 0;
    while (k < n && r->p < r->end) {
        int c = (int8_t)r8(r);
        if (c >= 0) {
            for (int i = 0; i < (c + 1) * unit; i++) {
                int b = r8(r);
                if (k < n)
                    out[k++] = (uint8_t)b;
            }
        } else if (c != -128) {
            uint8_t v[2];
            for (int u = 0; u < unit; u++)
                v[u] = (uint8_t)r8(r);
            for (int i = 0; i < -c + 1; i++)
                for (int u = 0; u < unit; u++)
                    if (k < n)
                        out[k++] = v[u];
        }
    }
}

/* One row of pixel data: unpacked below 8 row bytes, else count + PackBits. */
static void read_row(reader *r, uint8_t *row, int row_bytes, int unit, bool packed)
{
    if (!packed || row_bytes < 8) {
        for (int i = 0; i < row_bytes; i++)
            row[i] = (uint8_t)r8(r);
        return;
    }
    int count = row_bytes > 250 ? (uint16_t)r16(r) : r8(r);
    need(r, (uint32_t)count);
    reader sub = { r->p, r->p + (uint32_t)count, r->v2, 0, 0, 0 };
    unpack_bits(&sub, row, row_bytes, unit);
    skip(r, (uint32_t)count);
}

typedef struct {
    int row_bytes;
    qrect bounds;
    int pack_type, pixel_size, cmp_count;
    uint16_t ctable[256][3];
    bool is_pixmap;
} pixinfo;

static void read_pixmap_fields(reader *r, pixinfo *pi)
{
    pi->pack_type = r16(r);
    r32(r);                    /* packSize */
    r32(r);                    /* hRes */
    r32(r);                    /* vRes */
    r16(r);                    /* pixelType */
    pi->pixel_size = r16(r);
    pi->cmp_count = r16(r);
    r16(r);                    /* cmpSize */
    r32(r);                    /* planeBytes */
    r32(r);                    /* pmTable */
    r32(r);                    /* pmReserved */
}

static void read_ctable(reader *r, pixinfo *pi)
{
    r32(r);                    /* ctSeed */
    int flags = (uint16_t)r16(r);
    int n = r16(r) + 1;
    for (int i = 0; i < n; i++) {
        int idx = (uint16_t)r16(r);
        uint16_t rgb[3] = { (uint16_t)r16(r), (uint16_t)r16(r), (uint16_t)r16(r) };
        int slot = (flags & 0x8000) ? i : idx;
        if (slot >= 0 && slot < 256)
            memcpy(pi->ctable[slot], rgb, sizeof rgb);
    }
}

/* Pixel rows into RGBA. */
static void decode_pixels(reader *r, const pixinfo *pi, bool packed, image *img)
{
    int w = pi->bounds.right - pi->bounds.left, h = pi->bounds.bottom - pi->bounds.top;
    img->w = w > 0 ? w : 0;
    img->h = h > 0 ? h : 0;
    img->rgba = calloc((size_t)img->w * (size_t)img->h * 4 + 4, 1);
    int rb = pi->row_bytes;
    uint8_t *row = calloc((size_t)rb + 16, 1);
    for (int y = 0; y < img->h; y++) {
        uint8_t *px = img->rgba + (size_t)y * (size_t)img->w * 4;
        if (pi->is_pixmap && pi->pixel_size == 32) {
            int planes = pi->cmp_count ? pi->cmp_count : 3;
            if (pi->pack_type == 4 || (packed && pi->pack_type == 0)) {
                int n = img->w * planes;
                if (packed && rb >= 8) {
                    int count = rb > 250 ? (uint16_t)r16(r) : r8(r);
                    need(r, (uint32_t)count);
                    reader sub = { r->p, r->p + (uint32_t)count, r->v2, 0, 0, 0 };
                    uint8_t *buf = calloc((size_t)n + 1, 1);
                    unpack_bits(&sub, buf, n, 1);
                    skip(r, (uint32_t)count);
                    for (int x = 0; x < img->w; x++) {
                        int o = planes == 4 ? 1 : 0;
                        px[x * 4] = buf[(o + 0) * img->w + x];
                        px[x * 4 + 1] = buf[(o + 1) * img->w + x];
                        px[x * 4 + 2] = buf[(o + 2) * img->w + x];
                        px[x * 4 + 3] = 255;
                    }
                    free(buf);
                    continue;
                }
            }
            read_row(r, row, rb, 1, false);
            for (int x = 0; x < img->w && x * 4 + 3 < rb; x++) {
                px[x * 4] = row[x * 4 + 1];
                px[x * 4 + 1] = row[x * 4 + 2];
                px[x * 4 + 2] = row[x * 4 + 3];
                px[x * 4 + 3] = 255;
            }
            continue;
        }
        if (pi->is_pixmap && pi->pixel_size == 16) {
            read_row(r, row, rb, 2, packed && pi->pack_type != 1);
            for (int x = 0; x < img->w && x * 2 + 1 < rb; x++) {
                int v = row[x * 2] << 8 | row[x * 2 + 1];
                px[x * 4] = (uint8_t)(((v >> 10) & 31) * 255 / 31);
                px[x * 4 + 1] = (uint8_t)(((v >> 5) & 31) * 255 / 31);
                px[x * 4 + 2] = (uint8_t)((v & 31) * 255 / 31);
                px[x * 4 + 3] = 255;
            }
            continue;
        }
        read_row(r, row, rb, 1, packed);
        int bpp = pi->is_pixmap ? pi->pixel_size : 1;
        if (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8)
            bpp = 1;
        for (int x = 0; x < img->w; x++) {
            int bit = x * bpp;
            int v = (row[bit >> 3] >> (8 - bpp - (bit & 7))) & ((1 << bpp) - 1);
            if (pi->is_pixmap) {
                px[x * 4] = (uint8_t)(pi->ctable[v][0] >> 8);
                px[x * 4 + 1] = (uint8_t)(pi->ctable[v][1] >> 8);
                px[x * 4 + 2] = (uint8_t)(pi->ctable[v][2] >> 8);
            } else {
                uint8_t c = v ? 0 : 255;
                px[x * 4] = px[x * 4 + 1] = px[x * 4 + 2] = c;
            }
            px[x * 4 + 3] = 255;
        }
    }
    free(row);
}

/* Show an image: dithered to 1 bit through CopyBits (so clipping, scaling
 * and transfer modes apply), and recorded in colour when printing. */
static void show_image(const image *img, qrect src, qrect dst, int mode, const hrgn *mask)
{
    if (img->w <= 0 || img->h <= 0)
        return;
    gaddr port = qd_port();
    if (qd_rec_active(port))
        qd_rec_image(port, img->w, img->h, img->rgba, src, dst);
    /* Into a colour port: the image as a 32-bit PixMap, which CopyBits
     * takes to the port's depth and colours; into a black-and-white one,
     * an ordered dither. */
    bool deep = qd_port_pixels(port).depth > 1 || (rd16(port + GrafPort_portBits_off + 4) & 0x8000);
    uint32_t rb = deep ? (uint32_t)img->w * 4 : (uint32_t)((img->w + 15) / 16 * 2);
    gaddr bits = tb_NewPtr((int32_t)(rb * (uint32_t)img->h), false, true);
    if (!bits)
        return;
    gaddr bm = tb_NewPtr(PixMap_SIZEOF, false, true);
    if (deep) {
        for (int y = 0; y < img->h; y++)
            for (int x = 0; x < img->w; x++) {
                const uint8_t *p = img->rgba + ((size_t)y * (size_t)img->w + (size_t)x) * 4;
                wr32(bits + (uint32_t)y * rb + (uint32_t)x * 4, (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]);
            }
        PixMap_set_pixelSize(bm, 32);
        PixMap_set_pixelType(bm, 16); /* RGBDirect */
        PixMap_set_cmpCount(bm, 3);
        PixMap_set_cmpSize(bm, 8);
        PixMap_set_hRes(bm, 72 << 16);
        PixMap_set_vRes(bm, 72 << 16);
    } else {
        static const int bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
        for (int y = 0; y < img->h; y++)
            for (int x = 0; x < img->w; x++) {
                const uint8_t *p = img->rgba + ((size_t)y * (size_t)img->w + (size_t)x) * 4;
                int lum = (p[0] * 30 + p[1] * 59 + p[2] * 11) / 100;
                int threshold = (bayer[y & 3][x & 3] * 255 + 128) / 16;
                if (lum < threshold || (lum < 255 && lum == 0)) {
                    gaddr a = bits + (uint32_t)y * rb + (uint32_t)(x >> 3);
                    wr8(a, (uint8_t)(rd8(a) | (0x80 >> (x & 7))));
                }
            }
    }
    gaddr sr = tb_NewPtr(8, false, false), dr = tb_NewPtr(8, false, false);
    wr32(bm, bits);
    wr16(bm + 4, (uint16_t)(rb | (deep ? 0x8000 : 0)));
    qrect_put(bm + 6, (qrect){ 0, 0, (int16_t)img->h, (int16_t)img->w });
    qrect_put(sr, src);
    qrect_put(dr, dst);
    gaddr mrgn = 0;
    if (mask) {
        mrgn = tb_NewRgn();
        rgn_write(mrgn, mask);
    }
    qd_rec_suppress(true);
    tb_CopyBits(bm, port + GrafPort_portBits_off, sr, dr, (int16_t)(mode & 0x3F), mrgn);
    qd_rec_suppress(false);
    if (mrgn)
        tb_DisposeRgn(mrgn);
    tb_DisposePtr(dr);
    tb_DisposePtr(sr);
    tb_DisposePtr(bm);
    tb_DisposePtr(bits);
}

/* BitsRect, BitsRgn, PackBitsRect, PackBitsRgn, DirectBitsRect, DirectBitsRgn. */
static void bits_op(reader *r, const mapping *m, int op)
{
    bool direct = op == 0x9A || op == 0x9B, rgn = op & 1, packed = op != 0x90 && op != 0x91;
    pixinfo pi;
    memset(&pi, 0, sizeof pi);
    if (direct)
        r32(r); /* baseAddr */
    int rbw = (uint16_t)r16(r);
    pi.is_pixmap = direct || (rbw & 0x8000);
    pi.row_bytes = rbw & 0x3FFF;
    pi.bounds = rrect(r);
    if (pi.is_pixmap) {
        r16(r); /* pmVersion */
        read_pixmap_fields(r, &pi);
        if (!direct)
            read_ctable(r, &pi);
    }
    qrect src = rrect(r), dst = rrect(r);
    int mode = r16(r);
    hrgn mask;
    hrgn_init(&mask);
    if (rgn)
        read_region(r, m, &mask);
    image img;
    decode_pixels(r, &pi, packed || direct, &img);
    if (qd_survey) {
        survey_image(qd_survey, pi.is_pixmap, pi.is_pixmap ? pi.pixel_size : 1, img.rgba, img.w, img.h);
        free(img.rgba);
        hrgn_free(&mask);
        return;
    }
    /* Source rect relative to the image's own bounds. */
    src = (qrect){ (int16_t)(src.top - pi.bounds.top), (int16_t)(src.left - pi.bounds.left),
                   (int16_t)(src.bottom - pi.bounds.top), (int16_t)(src.right - pi.bounds.left) };
    show_image(&img, src, map_rect(m, dst), mode, rgn ? &mask : NULL);
    free(img.rgba);
    hrgn_free(&mask);
}

/* A colour pattern: keep its black-and-white version. */
static gaddr pixpat(reader *r, uint8_t pat[8])
{
    int type = r16(r);
    for (int i = 0; i < 8; i++)
        pat[i] = (uint8_t)r8(r);
    if (type == 2) {
        gaddr c = tb_NewPtr(6, false, false);
        for (int i = 0; i < 3; i++)
            wr16(c + (uint32_t)i * 2, (uint16_t)r16(r));
        if (qd_survey)
            survey_rgb(qd_survey, (const uint16_t[3]){ rd16(c), rd16(c + 2), rd16(c + 4) });
        gaddr pp = tb_NewPixPat();
        tb_MakeRGBPat(pp, c);
        gput(rd32(pp) + PixPat_pat1Data_off, pat, 8);
        tb_DisposePtr(c);
        return pp;
    }
    if (type != 1)
        return 0;
    pixinfo pi;
    memset(&pi, 0, sizeof pi);
    r32(r);
    pi.is_pixmap = true;
    pi.row_bytes = (uint16_t)r16(r) & 0x3FFF;
    pi.bounds = rrect(r);
    r16(r);
    read_pixmap_fields(r, &pi);
    read_ctable(r, &pi);
    image img;
    decode_pixels(r, &pi, true, &img);
    if (qd_survey && img.rgba)
        for (int i = 0; i < img.w * img.h; i++)
            survey_rgb(qd_survey, (const uint16_t[3]){ (uint16_t)(img.rgba[i * 4] * 0x101),
                                                       (uint16_t)(img.rgba[i * 4 + 1] * 0x101),
                                                       (uint16_t)(img.rgba[i * 4 + 2] * 0x101) });
    gaddr pp = 0;
    if (img.w > 0 && img.h > 0 && img.w <= 256 && img.h <= 256) {
        /* Kept as a 32-bit pattern of the decoded colours. */
        pp = tb_NewPixPat();
        gput(rd32(pp) + PixPat_pat1Data_off, pat, 8);
        gaddr pm = rd32(rd32(pp) + PixPat_patMap_off);
        PixMap_set_rowBytes(rd32(pm), (int16_t)(0x8000 | img.w * 4));
        qrect_put(rd32(pm) + PixMap_bounds_off, (qrect){ 0, 0, (int16_t)img.h, (int16_t)img.w });
        PixMap_set_pixelType(rd32(pm), 16);
        PixMap_set_pixelSize(rd32(pm), 32);
        PixMap_set_cmpCount(rd32(pm), 3);
        PixMap_set_cmpSize(rd32(pm), 8);
        gaddr data = rd32(rd32(pp) + PixPat_patData_off);
        tb_SetHandleSize(data, img.w * img.h * 4);
        for (int i = 0; i < img.w * img.h; i++)
            wr32(rd32(data) + (uint32_t)i * 4,
                 (uint32_t)img.rgba[i * 4] << 16 | (uint32_t)img.rgba[i * 4 + 1] << 8 | img.rgba[i * 4 + 2]);
    }
    free(img.rgba);
    return pp;
}

/* ---------------------------------------------------------------------- */
/* Playback */

typedef struct {
    qrect rect, rrect, oval, arc;
    int arc_start, arc_angle, ov_w, ov_h;
    poly pl;
    hrgn rgn;
    uint32_t text_pt;
    uint8_t fill_pat[8];
    gaddr fill_pp; /* a colour fill pattern (PixPat), or 0 */
} last_shapes;

enum { V_FRAME, V_PAINT, V_ERASE, V_INVERT, V_FILL };

static void shape_verb(int kind, int verb, qrect q, const last_shapes *ls)
{
    gaddr r = tb_NewPtr(8, false, false), pat = tb_NewPtr(8, false, false);
    qrect_put(r, q);
    gput(pat, ls->fill_pat, 8);
    switch (kind) {
    case 0:
        if (verb == V_FRAME) tb_FrameRect(r);
        else if (verb == V_PAINT) tb_PaintRect(r);
        else if (verb == V_ERASE) tb_EraseRect(r);
        else if (verb == V_INVERT) tb_InvertRect(r);
        else tb_FillRect(r, pat);
        break;
    case 1:
        if (verb == V_FRAME) tb_FrameRoundRect(r, (int16_t)ls->ov_w, (int16_t)ls->ov_h);
        else if (verb == V_PAINT) tb_PaintRoundRect(r, (int16_t)ls->ov_w, (int16_t)ls->ov_h);
        else if (verb == V_ERASE) tb_EraseRoundRect(r, (int16_t)ls->ov_w, (int16_t)ls->ov_h);
        else if (verb == V_INVERT) tb_InvertRoundRect(r, (int16_t)ls->ov_w, (int16_t)ls->ov_h);
        else tb_FillRoundRect(r, (int16_t)ls->ov_w, (int16_t)ls->ov_h, pat);
        break;
    case 2:
        if (verb == V_FRAME) tb_FrameOval(r);
        else if (verb == V_PAINT) tb_PaintOval(r);
        else if (verb == V_ERASE) tb_EraseOval(r);
        else if (verb == V_INVERT) tb_InvertOval(r);
        else tb_FillOval(r, pat);
        break;
    default: {
        int16_t s = (int16_t)ls->arc_start, a = (int16_t)ls->arc_angle;
        if (verb == V_FRAME) tb_FrameArc(r, s, a);
        else if (verb == V_PAINT) tb_PaintArc(r, s, a);
        else if (verb == V_ERASE) tb_EraseArc(r, s, a);
        else if (verb == V_INVERT) tb_InvertArc(r, s, a);
        else tb_FillArc(r, s, a, pat);
        break;
    }
    }
    tb_DisposePtr(pat);
    tb_DisposePtr(r);
}

static void region_verb(int verb, const hrgn *shape, const last_shapes *ls)
{
    gaddr h = tb_NewRgn(), pat = tb_NewPtr(8, false, false);
    rgn_write(h, shape);
    gput(pat, ls->fill_pat, 8);
    if (verb == V_FRAME) tb_FrameRgn(h);
    else if (verb == V_PAINT) tb_PaintRgn(h);
    else if (verb == V_ERASE) tb_EraseRgn(h);
    else if (verb == V_INVERT) tb_InvertRgn(h);
    else tb_FillRgn(h, pat);
    tb_DisposePtr(pat);
    tb_DisposeRgn(h);
}

static void poly_verb(int verb, const poly *pl, const last_shapes *ls)
{
    if (pl->n < 2)
        return;
    if (verb == V_FRAME) {
        tb_MoveTo(pl->pts[0][0], pl->pts[0][1]);
        for (int i = 1; i < pl->n; i++)
            tb_LineTo(pl->pts[i][0], pl->pts[i][1]);
        return;
    }
    hrgn shape;
    poly_region(pl, &shape);
    region_verb(verb, &shape, ls);
    hrgn_free(&shape);
}

static void draw_text_op(gaddr port, reader *r, int n)
{
    gaddr t = tb_NewPtr(n ? n : 1, false, false);
    for (int i = 0; i < n; i++)
        wr8(t + (uint32_t)i, (uint8_t)r8(r));
    tb_DrawText(t, 0, (int16_t)n);
    tb_DisposePtr(t);
}

static void set_pat(void (*fn)(gaddr), const uint8_t pat[8])
{
    gaddr p = tb_NewPtr(8, false, false);
    gput(p, pat, 8);
    fn(p);
    tb_DisposePtr(p);
}

/* A long comment's data goes to the commentProc in a handle of its own, as
 * PicComment takes it. */
static void play_comment(reader *r, int kind, int n)
{
    gaddr h = tb_NewHandle(n, false, false);
    if (!h || !rd32(h)) {
        skip(r, (uint32_t)n);
        return;
    }
    need(r, (uint32_t)n);
    for (int i = 0; i < n; i++)
        wr8(rd32(h) + (uint32_t)i, (uint8_t)r8(r));
    tb_PicComment((int16_t)kind, (int16_t)n, h);
    tb_DisposeHandle(h);
}

/* An old-style colour (blackColor, redColor ...) as RGB. */
static const uint16_t *old_color_rgb(int32_t c)
{
    static const struct { int32_t c; uint16_t rgb[3]; } colors[] = {
        { 33, { 0, 0, 0 } },          { 30, { 0xFFFF, 0xFFFF, 0xFFFF } }, { 205, { 0xDD6B, 0x08C2, 0x06A2 } },
        { 341, { 0, 0x8000, 0x11B0 } }, { 409, { 0, 0, 0xD400 } },         { 273, { 0x0241, 0xAB54, 0xEAFF } },
        { 137, { 0xF2D7, 0x0856, 0x84EC } }, { 69, { 0xFC00, 0xF37D, 0x052F } },
    };
    for (size_t i = 0; i < sizeof colors / sizeof *colors; i++)
        if (colors[i].c == c)
            return colors[i].rgb;
    return colors[0].rgb;
}

static void play(reader *r, mapping *m, gaddr port, const hrgn *base_clip)
{
    last_shapes ls;
    memset(&ls, 0, sizeof ls);
    hrgn_init(&ls.rgn);
    memset(ls.fill_pat, 0xFF, 8);
    int text_h = 0, text_v = 0;
    /* After a compressed image QuickTime drew, the warning the picture
     * carries for Macs without QuickTime ("QuickTime and a ... decompressor
     * are needed") isn't shown: its text, and the text state before it, up
     * to the next other opcode. */
    bool fallback = false;
    for (;;) {
        compact(r);
        need(r, r->v2 ? 2 : 1);
        if (r->p >= r->end)
            break;
        int op = r->v2 ? (uint16_t)r16(r) : r8(r);
        if (op == 0xFF || (r->v2 && op == 0x00FF))
            break;
        if (fallback && r->v2) {
            bool state = op == 0x00 || (op >= 0x03 && op <= 0x05) || op == 0x07 || op == 0x08 ||
                         (op >= 0x0D && op <= 0x10) || op == 0x1A || op == 0x1B || op == 0x1F || op == 0x2C ||
                         op == 0x2E;
            if (op >= 0x28 && op <= 0x2B) {
                skip(r, op == 0x28 ? 4 : op == 0x2B ? 2 : 1);
                skip(r, (uint32_t)r8(r));
                if (r->p & 1)
                    skip(r, 1);
                continue;
            }
            if (!state)
                fallback = false;
        }
        gaddr before = r->p;
        if (qd_survey)
            survey_op(qd_survey, op);
        qd_fill_pixpat = ls.fill_pp; /* fill verbs use the picture's colour pattern */
        switch (op) {
        case 0x00: break;
        case 0x01: {
            hrgn c;
            hrgn_init(&c);
            read_region(r, m, &c);
            hrgn_op(&c, &c, base_clip, RGN_SECT);
            gaddr h = tb_NewRgn();
            rgn_write(h, &c);
            tb_SetClip(h);
            tb_DisposeRgn(h);
            hrgn_free(&c);
            break;
        }
        case 0x02: { uint8_t p[8]; for (int i = 0; i < 8; i++) p[i] = (uint8_t)r8(r); set_pat(tb_BackPat, p); break; }
        case 0x03: {
            int f = r16(r);
            if (qd_survey)
                survey_font(qd_survey, f);
            tb_TextFont((int16_t)f);
            break;
        }
        case 0x04: tb_TextFace((int16_t)r8(r)); break;
        case 0x05: tb_TextMode((int16_t)r16(r)); break;
        case 0x06: r32(r); break;
        case 0x07: { int v = r16(r), h = r16(r); tb_PenSize((int16_t)scale_h(m, h), (int16_t)scale_v(m, v)); break; }
        case 0x08: tb_PenMode((int16_t)r16(r)); break;
        case 0x09: { uint8_t p[8]; for (int i = 0; i < 8; i++) p[i] = (uint8_t)r8(r); set_pat(tb_PenPat, p); break; }
        case 0x0A: for (int i = 0; i < 8; i++) ls.fill_pat[i] = (uint8_t)r8(r); ls.fill_pp = 0; break;
        case 0x0B: ls.ov_h = scale_v(m, r16(r)); ls.ov_w = scale_h(m, r16(r)); break;
        case 0x0C: { int dh = r16(r), dv = r16(r); m->src.left += dh; m->src.right += dh; m->src.top += dv; m->src.bottom += dv; break; }
        case 0x0D: tb_TextSize((int16_t)scale_v(m, r16(r))); break;
        case 0x0E: case 0x0F: {
            int32_t c = (int32_t)r32(r);
            if (qd_survey)
                survey_rgb(qd_survey, old_color_rgb(c));
            (op == 0x0E ? tb_ForeColor : tb_BackColor)(c);
            break;
        }
        case 0x10: skip(r, 8); break;
        case 0x11: r->v2 ? (void)r16(r) : (void)r8(r); break;
        case 0x12:
        case 0x13: {
            uint8_t p[8];
            gaddr pp = pixpat(r, p);
            if (pp && qd_port_is_color(port))
                (op == 0x12 ? tb_BackPixPat : tb_PenPixPat)(pp);
            else
                set_pat(op == 0x12 ? tb_BackPat : tb_PenPat, p);
            break;
        }
        case 0x14: ls.fill_pp = pixpat(r, ls.fill_pat); break;
        case 0x15: case 0x16: r16(r); break;
        case 0x1A: case 0x1B: case 0x1D: case 0x1F: {
            uint16_t c[3] = { (uint16_t)r16(r), (uint16_t)r16(r), (uint16_t)r16(r) };
            if (qd_survey && (op == 0x1A || op == 0x1B))
                survey_rgb(qd_survey, c);
            if ((op == 0x1A || op == 0x1B) && qd_port_is_color(qd_port())) {
                /* RGBFgCol, RGBBkCol: the colour itself in a colour port */
                gaddr rgb = tb_NewPtr(6, false, false);
                wr16(rgb, c[0]);
                wr16(rgb + 2, c[1]);
                wr16(rgb + 4, c[2]);
                (op == 0x1A ? tb_RGBForeColor : tb_RGBBackColor)(rgb);
                tb_DisposePtr(rgb);
            } else if (op == 0x1A || op == 0x1B) { /* an old port: black or white */
                int dark = (c[0] * 30 + c[1] * 59 + c[2] * 11) / 100 < 0x8000;
                int32_t old = dark ? 33 /* blackColor */ : 30 /* whiteColor */;
                if (op == 0x1A) tb_ForeColor(old); else tb_BackColor(old);
            }
            break;
        }
        case 0x1C: case 0x1E: break;
        case 0x20: { uint32_t a = rpt(r), b = rpt(r); tb_MoveTo((int16_t)map_h(m, pt_h(a)), (int16_t)map_v(m, pt_v(a))); tb_LineTo((int16_t)map_h(m, pt_h(b)), (int16_t)map_v(m, pt_v(b))); break; }
        case 0x21: { uint32_t b = rpt(r); tb_LineTo((int16_t)map_h(m, pt_h(b)), (int16_t)map_v(m, pt_v(b))); break; }
        case 0x22: {
            uint32_t a = rpt(r);
            int dh = (int8_t)r8(r), dv = (int8_t)r8(r);
            tb_MoveTo((int16_t)map_h(m, pt_h(a)), (int16_t)map_v(m, pt_v(a)));
            tb_LineTo((int16_t)map_h(m, pt_h(a) + dh), (int16_t)map_v(m, pt_v(a) + dv));
            break;
        }
        case 0x23: { int dh = (int8_t)r8(r), dv = (int8_t)r8(r); tb_Line((int16_t)scale_h(m, dh), (int16_t)scale_v(m, dv)); break; }
        case 0x28: {
            uint32_t p = rpt(r);
            text_h = pt_h(p);
            text_v = pt_v(p);
            tb_MoveTo((int16_t)map_h(m, text_h), (int16_t)map_v(m, text_v));
            draw_text_op(port, r, r8(r));
            if (qd_survey)
                survey_text(qd_survey, port);
            break;
        }
        case 0x29: case 0x2A: case 0x2B: {
            if (op != 0x2A) text_h += r8(r);
            if (op != 0x29) text_v += r8(r);
            tb_MoveTo((int16_t)map_h(m, text_h), (int16_t)map_v(m, text_v));
            draw_text_op(port, r, r8(r));
            if (qd_survey)
                survey_text(qd_survey, port);
            break;
        }
        case 0x2C: {
            int len = (uint16_t)r16(r);
            gaddr start = r->p;
            int old_id = r16(r);
            gaddr name = tb_NewPtr(256, false, true);
            int nl = r8(r);
            wr8(name, (uint8_t)nl);
            for (int i = 0; i < nl; i++)
                wr8(name + 1 + (uint32_t)i, (uint8_t)r8(r));
            gaddr num = tb_NewPtr(2, false, true);
            tb_GetFNum(name, num);
            int fid = (int16_t)rd16(num);
            if (qd_survey) {
                uint8_t pname[256];
                gget(pname, name, (uint32_t)nl + 1);
                survey_font_name(qd_survey, old_id, fid ? fid : old_id, pname);
            }
            /* Later txFont opcodes use the old ID; ours may differ. */
            if (fid != old_id && fid)
                tb_TextFont((int16_t)fid);
            tb_DisposePtr(num);
            tb_DisposePtr(name);
            r->p = start;
            skip(r, (uint32_t)len);
            break;
        }
        case 0x2D: case 0x2E: case 0x2F: case 0x24: case 0x25: case 0x26: case 0x27:
            skip(r, (uint16_t)r16(r));
            break;
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74:
            read_poly(r, m, &ls.pl);
            poly_verb(op - 0x70, &ls.pl, &ls);
            break;
        case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C:
            poly_verb(op - 0x78, &ls.pl, &ls);
            break;
        case 0x80: case 0x81: case 0x82: case 0x83: case 0x84:
            hrgn_free(&ls.rgn);
            hrgn_init(&ls.rgn);
            read_region(r, m, &ls.rgn);
            region_verb(op - 0x80, &ls.rgn, &ls);
            break;
        case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C:
            region_verb(op - 0x88, &ls.rgn, &ls);
            break;
        case 0x90: case 0x91: case 0x98: case 0x99: case 0x9A: case 0x9B:
            bits_op(r, m, op);
            break;
        case 0xA0: {
            int kind = r16(r);
            if (qd_survey)
                survey_comment(qd_survey, kind);
            tb_PicComment((int16_t)kind, 0, 0);
            break;
        }
        case 0xA1: {
            int kind = r16(r), n = (uint16_t)r16(r);
            if (qd_survey)
                survey_comment(qd_survey, kind);
            play_comment(r, kind, n);
            break;
        }
        case 0x0C00: skip(r, 24); break;
        case 0x8200: { /* CompressedQuickTime: version, matrix, matte, mode, srcRect, accuracy, mask, then the image */
            uint32_t len = r32(r);
            need(r, len);
            gaddr q = r->p;
            if (len >= 68 && q + len <= r->end) {
                gaddr mx = q + 2;
                qrect src = qrect_get(q + 52);
                uint32_t matte = rd32(q + 38), mask = rd32(q + 64);
                gaddr desc = q + 68 + matte + mask;
                if (qd_survey && matte < len && mask < len && desc + 86 <= q + len) {
                    /* The image's depth as QuickDraw has it: 24 is 32, 33 to 40 grays. */
                    int d = (int16_t)rd16(desc + 82);
                    survey_image(qd_survey, true, d == 24 ? 32 : d > 32 ? d - 32 : d > 0 ? d : 32, NULL, 0, 0);
                    fallback = true; /* as QuickTime would have drawn it */
                } else if (matte < len && mask < len && desc + 86 <= q + len && desc + rd32(desc) <= q + len) {
                    double sx = (int32_t)rd32(mx) / 65536.0, sy = (int32_t)rd32(mx + 16) / 65536.0;
                    double tx = (int32_t)rd32(mx + 24) / 65536.0, ty = (int32_t)rd32(mx + 28) / 65536.0;
                    qrect at = { (int16_t)(src.top * sy + ty), (int16_t)(src.left * sx + tx),
                                 (int16_t)(src.bottom * sy + ty), (int16_t)(src.right * sx + tx) };
                    fallback = icm_draw_compressed(desc, desc + rd32(desc), src, map_rect(m, at), 0);
                }
            }
            r->p = q;
            skip(r, len);
            break;
        }
        default:
            if (op >= 0x30 && op <= 0x6F) {
                int kind = (op - 0x30) >> 4;          /* 0 rect, 1 rrect, 2 oval, 3 arc */
                int verb = op & 7;
                bool same = op & 8;
                qrect *last = kind == 0 ? &ls.rect : kind == 1 ? &ls.rrect : kind == 2 ? &ls.oval : &ls.arc;
                if (!same)
                    *last = map_rect(m, rrect(r));
                if (kind == 3) {
                    ls.arc_start = r16(r);
                    ls.arc_angle = r16(r);
                }
                if (verb <= V_FILL)
                    shape_verb(kind, verb, *last, &ls);
            } else if ((op >= 0x17 && op <= 0x19) || (op >= 0x3D && op <= 0x3F)) {
                /* no data */
            } else if (op >= 0xA2 && op <= 0xAF) {
                skip(r, (uint16_t)r16(r));
            } else if (op >= 0xB0 && op <= 0xCF) {
                /* no data */
            } else if (op >= 0xD0 && op <= 0xFE) {
                skip(r, r32(r));
            } else if (op >= 0x0100 && op <= 0x7FFF) {
                skip(r, (uint32_t)((op >> 8) * 2));
            } else if (op >= 0x8100) {
                skip(r, r32(r));
            } else {
                /* reserved, no data */
            }
            break;
        }
        (void)before;
        if (r->v2 && (r->p & 1)) /* opcodes are word aligned in version 2 */
            skip(r, 1);
    }
    qd_fill_pixpat = 0;
    free(ls.pl.pts);
    hrgn_free(&ls.rgn);
}

/* ---------------------------------------------------------------------- */
/* Traps */

void tb_DrawPicture(gaddr pic, gaddr dst)
{
    if (!pic || !rd32(pic) || !dst)
        return;
    gaddr port = qd_port();
    qrect at = qrect_get(dst);
    walker_note_drawn(port, at.top, at.left, at.bottom, at.right);
    int8_t state = tb_HGetState(pic);
    tb_HLock(pic);
    gaddr p = rd32(pic);
    int32_t size = tb_GetHandleSize(pic);
    mapping m = { qrect_get(p + 2), qrect_get(dst) };
    reader r = { p + 10, p + (uint32_t)size, false, 0, 0, 0 };
    /* After picSize and picFrame, a port whose getPicProc is the program's
     * own reads the picture through it (spooling from a file, as Director
     * does), into a buffer of ours. */
    gaddr procs = GrafPort_grafProcs(port), get = procs ? rd32(procs + QDProcs_getPicProc_off) : 0;
    gaddr spool = 0;
    if (get && native_index_for(get) < 0) {
        static const int32_t sizes[] = { 4 << 20, 1 << 20, 256 << 10 };
        int32_t cap = 0;
        for (int i = 0; i < 3 && !spool; i++) {
            cap = sizes[i];
            spool = tb_NewPtr(cap, true, false);
            if (!spool)
                spool = tb_NewPtr(cap, false, false);
        }
        if (spool)
            r = (reader){ spool, spool, false, get, spool, spool + (uint32_t)cap };
    }
    /* Version: 0x11 0x01 (v1) or 0x0011 0x02FF (v2). */
    need(&r, 2);
    gaddr v = r.p;
    if (rd8(v) == 0x00 && rd8(v + 1) == 0x11)
        need(&r, 4);
    if (rd8(v) == 0x00 && rd8(v + 1) == 0x11 && rd8(v + 2) == 0x02 && rd8(v + 3) == 0xFF) {
        r.v2 = true;
        r.p = v + 4;
        need(&r, 2 + 12 + 8);
        if (rd16(r.p) == 0x0C00) {
            int16_t version = (int16_t)rd16(r.p + 2);
            /* Extended version 2: after version, reserved, hRes and vRes,
             * the frame at the picture's own resolution. */
            if (version == -2)
                m.src = qrect_get(r.p + 2 + 12);
        }
    } else if (rd8(v) == 0x11 && rd8(v + 1) == 0x01) {
        r.p = v + 2;
    }
    /* The picture draws with its own pen and text state. */
    uint8_t saved[GrafPort_SIZEOF];
    gget(saved, port, GrafPort_SIZEOF);
    /* A colour port's patterns live in its PixPat handles. */
    static const uint32_t pp_offs[3] = { CGrafPort_pnPixPat_off, CGrafPort_bkPixPat_off, CGrafPort_fillPixPat_off };
    uint8_t saved_pp[3][PixPat_SIZEOF];
    bool color = qd_port_is_color(port);
    for (int i = 0; i < 3 && color; i++) {
        gaddr h = rd32(port + pp_offs[i]);
        if (h && rd32(h))
            gget(saved_pp[i], rd32(h), PixPat_SIZEOF);
    }
    gaddr saved_clip = tb_NewRgn();
    tb_GetClip(saved_clip);
    hrgn base;
    hrgn_init(&base);
    rgn_read(saved_clip, &base);
    tb_PenNormal();
    tb_TextFont(0);
    tb_TextSize(0);
    tb_TextFace(0);
    tb_TextMode(1);
    play(&r, &m, port, &base);
    hrgn_free(&base);
    tb_SetClip(saved_clip);
    tb_DisposeRgn(saved_clip);
    /* Restore pen, text and patterns (keep the regions the port owns). */
    gaddr vis = GrafPort_visRgn(port), clip = GrafPort_clipRgn(port);
    gput(port + GrafPort_bkPat_off, saved + GrafPort_bkPat_off, GrafPort_SIZEOF - GrafPort_bkPat_off);
    for (int i = 0; i < 3 && color; i++) {
        gaddr h = rd32(port + pp_offs[i]);
        if (h && rd32(h))
            gput(rd32(h), saved_pp[i], PixPat_SIZEOF);
    }
    GrafPort_set_visRgn(port, vis);
    GrafPort_set_clipRgn(port, clip);
    if (spool)
        tb_DisposePtr(spool);
    tb_HSetState(pic, state);
}

void tb_KillPicture(gaddr pic) { if (pic) tb_DisposeHandle(pic); }
/* Comments go to the port's commentProc when the application installed one
 * (print drivers and picture parsers do), as every comment in a picture
 * being drawn does too; StdComment writes them into the picture being
 * recorded and gives them to the printing port (qd_rec_comment says which
 * it heeds). */
void tb_PicComment(int16_t kind, int16_t size, gaddr data)
{
    gaddr port = qd_port();
    gaddr procs = port ? GrafPort_grafProcs(port) : 0;
    gaddr proc = procs ? rd32(procs + QDProcs_commentProc_off) : 0;
    if (!proc || native_index_for(proc) >= 0) {
        tb_StdComment(kind, size, data);
        return;
    }
    cpu_push16(g_cpu, (uint16_t)kind);
    cpu_push16(g_cpu, (uint16_t)size);
    cpu_push32(g_cpu, data);
    guest_call(proc);
}

void tb_StdComment(int16_t kind, int16_t size, gaddr data)
{
    gaddr port = qd_port();
    if (!port)
        return;
    qd_pic_comment(port, kind, size, data);
    if (qd_rec_active(port))
        qd_rec_comment(port, kind, size, data);
}

/* Pictures are read and written whole here, so these only move bytes. */
void tb_StdGetPic(gaddr dp, int16_t bc) { gmemset(dp, 0, (uint32_t)(bc > 0 ? bc : 0)); }
void tb_StdPutPic(gaddr sp, int16_t bc) {}

/* ---------------------------------------------------------------------- */
/* bluebox inspect: a picture drawn for the host, at its frame's size. */

bool machine_picture_rgba(const uint8_t *pict, size_t len, uint8_t **rgba, int *w, int *h)
{
    *rgba = NULL;
    if (len < 10 || len > 0x1000000)
        return false;
    int top = (int16_t)(pict[2] << 8 | pict[3]), left = (int16_t)(pict[4] << 8 | pict[5]);
    int width = (int16_t)(pict[8] << 8 | pict[9]) - left, height = (int16_t)(pict[6] << 8 | pict[7]) - top;
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192)
        return false;
    /* QuickDraw's globals, in an A5 world of our own when no program has
     * set one up: thePort at A5's pointer, the rest below it. */
    if (!qd_port()) {
        gaddr world = tb_NewPtr(1024, false, true), a5 = world + 768;
        wr32(LM_CurrentA5, a5);
        tb_InitGraf(world + 512);
        tb_InitFonts();
        tb_OpenPort(tb_NewPtr(GrafPort_SIZEOF, false, true));
    }
    gaddr saved = qd_port(), saved_gd = rd32(LM_TheGDevice);
    gaddr bounds = tb_NewPtr(8, false, true), gw = tb_NewPtr(4, false, true);
    qrect_put(bounds, (qrect){ 0, 0, (int16_t)height, (int16_t)width });
    gaddr pic = tb_NewHandle((int32_t)len, false, false);
    bool ok = pic && tb_NewGWorld(gw, 32, bounds, 0, 0, 0) == noErr;
    if (ok) {
        gaddr port = rd32(gw);
        tb_SetGWorld(port, 0);
        tb_EraseRect(bounds);
        gput(rd32(pic), pict, (uint32_t)len);
        tb_DrawPicture(pic, bounds);
        gaddr pm = rd32(rd32(port + CGrafPort_portPixMap_off));
        gaddr base = qd_pixmap_base(pm, true);
        uint32_t row_bytes = (uint16_t)PixMap_rowBytes(pm) & 0x3FFF;
        *rgba = malloc((size_t)width * (size_t)height * 4);
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++) {
                uint32_t v = rd32(base + (uint32_t)y * row_bytes + (uint32_t)x * 4);
                uint8_t *o = *rgba + ((size_t)y * (size_t)width + (size_t)x) * 4;
                o[0] = (uint8_t)(v >> 16);
                o[1] = (uint8_t)(v >> 8);
                o[2] = (uint8_t)v;
                o[3] = 255;
            }
        tb_SetGWorld(saved, saved_gd);
        tb_DisposeGWorld(port);
    }
    if (pic)
        tb_DisposeHandle(pic);
    tb_DisposePtr(gw);
    tb_DisposePtr(bounds);
    *w = width;
    *h = height;
    return ok;
}
