/*
 * QuickDraw pictures: DrawPicture for version 1 and 2 PICTs.
 *
 * Opcodes are played back through the ordinary drawing calls in the current
 * port, mapped from the picture frame to the destination rectangle.  Bitmap
 * and pixmap opcodes are decoded to RGB, dithered for the 1-bit screen, and
 * recorded in full colour when the port is printing.
 *
 * OpenPicture/ClosePicture produce a valid empty picture of the right frame;
 * recording drawing into pictures is not done yet.
 */
#include <stdlib.h>
#include <string.h>

#include "m68k.h"
#include "machine.h"
#include "qd_internal.h"
#include "tb_internal.h"
#include "trap.h"

enum { srcCopy = 0 };

typedef struct {
    gaddr p, end;
    bool v2;
} reader;

static int r8(reader *r) { return r->p < r->end ? rd8(r->p++) : 0; }
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
static void skip(reader *r, uint32_t n) { r->p = r->p + n < r->end ? r->p + n : r->end; }
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

/* Scan-convert a polygon (even-odd) into a region. */
static void poly_region(const poly *pl, hrgn *out)
{
    hrgn_init(out);
    if (pl->n < 3)
        return;
    int top = 32767, bottom = -32768;
    for (int i = 0; i < pl->n; i++) {
        if (pl->pts[i][1] < top)
            top = pl->pts[i][1];
        if (pl->pts[i][1] > bottom)
            bottom = pl->pts[i][1];
    }
    int *xs = malloc(sizeof *xs * (size_t)pl->n);
    for (int y = top; y < bottom; y++) {
        double sy = y + 0.5;
        int nx = 0;
        for (int i = 0; i < pl->n; i++) {
            int j = (i + 1) % pl->n;
            double y0 = pl->pts[i][1], y1 = pl->pts[j][1];
            if ((sy >= y0) != (sy >= y1)) {
                double x = pl->pts[i][0] + (sy - y0) * (pl->pts[j][0] - pl->pts[i][0]) / (y1 - y0);
                xs[nx++] = (int)(x + 0.5);
            }
        }
        for (int a = 1; a < nx; a++)
            for (int b = a; b > 0 && xs[b - 1] > xs[b]; b--) {
                int t = xs[b];
                xs[b] = xs[b - 1];
                xs[b - 1] = t;
            }
        for (int k = 0; k + 1 < nx; k += 2) {
            if (xs[k] >= xs[k + 1])
                continue;
            hrgn row;
            hrgn_init(&row);
            hrgn_rect(&row, (qrect){ (int16_t)y, (int16_t)xs[k], (int16_t)(y + 1), (int16_t)xs[k + 1] });
            hrgn_op(out, out, &row, RGN_UNION);
            hrgn_free(&row);
        }
    }
    free(xs);
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
    reader sub = { r->p, r->p + (uint32_t)count, r->v2 };
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
                    reader sub = { r->p, r->p + (uint32_t)count, r->v2 };
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
    /* Source rect relative to the image's own bounds. */
    src = (qrect){ (int16_t)(src.top - pi.bounds.top), (int16_t)(src.left - pi.bounds.left),
                   (int16_t)(src.bottom - pi.bounds.top), (int16_t)(src.right - pi.bounds.left) };
    show_image(&img, src, map_rect(m, dst), mode, rgn ? &mask : NULL);
    free(img.rgba);
    hrgn_free(&mask);
}

/* A colour pattern: keep its black-and-white version. */
static void pixpat(reader *r, uint8_t pat[8])
{
    int type = r16(r);
    for (int i = 0; i < 8; i++)
        pat[i] = (uint8_t)r8(r);
    if (type == 2) {
        skip(r, 6); /* RGB */
        return;
    }
    if (type != 1)
        return;
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
    free(img.rgba);
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

static void play(reader *r, mapping *m, gaddr port, const hrgn *base_clip)
{
    last_shapes ls;
    memset(&ls, 0, sizeof ls);
    hrgn_init(&ls.rgn);
    memset(ls.fill_pat, 0xFF, 8);
    int text_h = 0, text_v = 0;
    while (r->p < r->end) {
        int op = r->v2 ? (uint16_t)r16(r) : r8(r);
        if (op == 0xFF || (r->v2 && op == 0x00FF))
            break;
        gaddr before = r->p;
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
        case 0x03: tb_TextFont((int16_t)r16(r)); break;
        case 0x04: tb_TextFace((int16_t)r8(r)); break;
        case 0x05: tb_TextMode((int16_t)r16(r)); break;
        case 0x06: r32(r); break;
        case 0x07: { int v = r16(r), h = r16(r); tb_PenSize((int16_t)scale_h(m, h), (int16_t)scale_v(m, v)); break; }
        case 0x08: tb_PenMode((int16_t)r16(r)); break;
        case 0x09: { uint8_t p[8]; for (int i = 0; i < 8; i++) p[i] = (uint8_t)r8(r); set_pat(tb_PenPat, p); break; }
        case 0x0A: for (int i = 0; i < 8; i++) ls.fill_pat[i] = (uint8_t)r8(r); break;
        case 0x0B: ls.ov_h = scale_v(m, r16(r)); ls.ov_w = scale_h(m, r16(r)); break;
        case 0x0C: { int dh = r16(r), dv = r16(r); m->src.left += dh; m->src.right += dh; m->src.top += dv; m->src.bottom += dv; break; }
        case 0x0D: tb_TextSize((int16_t)scale_v(m, r16(r))); break;
        case 0x0E: tb_ForeColor((int32_t)r32(r)); break;
        case 0x0F: tb_BackColor((int32_t)r32(r)); break;
        case 0x10: skip(r, 8); break;
        case 0x11: r->v2 ? (void)r16(r) : (void)r8(r); break;
        case 0x12: { uint8_t p[8]; pixpat(r, p); set_pat(tb_BackPat, p); break; }
        case 0x13: { uint8_t p[8]; pixpat(r, p); set_pat(tb_PenPat, p); break; }
        case 0x14: pixpat(r, ls.fill_pat); break;
        case 0x15: case 0x16: r16(r); break;
        case 0x1A: case 0x1B: case 0x1D: case 0x1F: {
            uint16_t c[3] = { (uint16_t)r16(r), (uint16_t)r16(r), (uint16_t)r16(r) };
            if (op == 0x1A || op == 0x1B) {
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
            break;
        }
        case 0x29: case 0x2A: case 0x2B: {
            if (op != 0x2A) text_h += r8(r);
            if (op != 0x29) text_v += r8(r);
            tb_MoveTo((int16_t)map_h(m, text_h), (int16_t)map_v(m, text_v));
            draw_text_op(port, r, r8(r));
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
        case 0xA0: r16(r); break;
        case 0xA1: { r16(r); skip(r, (uint16_t)r16(r)); break; }
        case 0x0C00: skip(r, 24); break;
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
    int8_t state = tb_HGetState(pic);
    tb_HLock(pic);
    gaddr p = rd32(pic);
    int32_t size = tb_GetHandleSize(pic);
    mapping m = { qrect_get(p + 2), qrect_get(dst) };
    reader r = { p + 10, p + (uint32_t)size, false };
    /* Version: 0x11 0x01 (v1) or 0x0011 0x02FF (v2). */
    if (rd8(p + 10) == 0x00 && rd8(p + 11) == 0x11 && rd8(p + 12) == 0x02 && rd8(p + 13) == 0xFF) {
        r.v2 = true;
        r.p = p + 14;
        if (rd16(r.p) == 0x0C00) {
            int16_t version = (int16_t)rd16(r.p + 2);
            if (version == -2) /* extended version 2: native resolution frame */
                m.src = qrect_get(r.p + 2 + 16);
        }
    } else if (rd8(p + 10) == 0x11 && rd8(p + 11) == 0x01) {
        r.p = p + 12;
    }
    /* The picture draws with its own pen and text state. */
    uint8_t saved[GrafPort_SIZEOF];
    gget(saved, port, GrafPort_SIZEOF);
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
    GrafPort_set_visRgn(port, vis);
    GrafPort_set_clipRgn(port, clip);
    tb_HSetState(pic, state);
}

/* Pictures being defined: a valid, empty version 2 picture of the frame.
 * size(2) frame(8) 0x0011 0x02FF, HeaderOp 0x0C00 + 24 bytes (version -1,
 * -1, fixed-point bounds, reserved), EndOfPicture. */
static gaddr open_pic;

gaddr tb_OpenPicture(gaddr r)
{
    gaddr h = tb_NewHandle(42, false, true);
    gaddr p = rd32(h);
    qrect q = qrect_get(r);
    wr16(p, 42);
    gmemcpy(p + 2, r, 8);
    wr16(p + 10, 0x0011);
    wr16(p + 12, 0x02FF);
    wr16(p + 14, 0x0C00);
    wr16(p + 16, 0xFFFF);
    wr16(p + 18, 0xFFFF);
    wr32(p + 20, (uint32_t)q.left << 16);
    wr32(p + 24, (uint32_t)q.top << 16);
    wr32(p + 28, (uint32_t)q.right << 16);
    wr32(p + 32, (uint32_t)q.bottom << 16);
    wr32(p + 36, 0);
    wr16(p + 40, 0x00FF);
    open_pic = h;
    return h;
}

void tb_ClosePicture(void) { open_pic = 0; }
void tb_KillPicture(gaddr pic) { if (pic) tb_DisposeHandle(pic); }
/* Comments go to the port's commentProc when the application installed one
 * (print drivers and picture parsers do); StdComment has nothing to do. */
void tb_PicComment(int16_t kind, int16_t size, gaddr data)
{
    gaddr port = qd_port();
    gaddr procs = port ? GrafPort_grafProcs(port) : 0;
    gaddr proc = procs ? rd32(procs + QDProcs_commentProc_off) : 0;
    if (!proc || native_index_for(proc) >= 0)
        return;
    cpu_push16(g_cpu, (uint16_t)kind);
    cpu_push16(g_cpu, (uint16_t)size);
    cpu_push32(g_cpu, data);
    guest_call(proc);
}

void tb_StdComment(int16_t kind, int16_t size, gaddr data) {}

/* Pictures are read and written whole here, so these only move bytes. */
void tb_StdGetPic(gaddr dp, int16_t bc) { gmemset(dp, 0, (uint32_t)(bc > 0 ? bc : 0)); }
void tb_StdPutPic(gaddr sp, int16_t bc) {}

/* ---------------------------------------------------------------------- */
/* Picture Utilities (Pack15): the frame and resolution; no survey of the
 * colours, fonts or comments inside. */

int16_t tb_GetPictInfo(gaddr pic, gaddr info, int16_t verb, int16_t colors, int16_t method, int16_t version)
{
    gmemset(info, 0, PictInfo_SIZEOF);
    if (!pic || !rd32(pic))
        return -11000; /* pictInfoVersionErr-like: no picture */
    gaddr p = rd32(pic);
    gmemcpy(info + PictInfo_sourceRect_off, p + 2, 8);
    wr16(info + PictInfo_version_off, (uint16_t)(rd16(p + 10) == 0x0011 ? 2 : 1));
    wr32(info + PictInfo_hRes_off, 72u << 16);
    wr32(info + PictInfo_vRes_off, 72u << 16);
    wr16(info + PictInfo_depth_off, 1);
    return noErr;
}

int16_t tb_GetPixMapInfo(gaddr pm, gaddr info, int16_t verb, int16_t colors, int16_t method, int16_t version)
{
    gmemset(info, 0, PictInfo_SIZEOF);
    if (!pm || !rd32(pm))
        return -11000;
    gaddr p = rd32(pm);
    gmemcpy(info + PictInfo_sourceRect_off, p + PixMap_bounds_off, 8);
    wr32(info + PictInfo_hRes_off, 72u << 16);
    wr32(info + PictInfo_vRes_off, 72u << 16);
    wr16(info + PictInfo_depth_off, (uint16_t)PixMap_pixelSize(p));
    return noErr;
}
