/*
 * QuickDraw recording, for printing.
 *
 * A port can be marked as recording (the Printing Manager's printing port).
 * Drawing in it is kept as shapes, lines and text runs with their pen,
 * pattern, transfer mode and clip, in device coordinates (the page's
 * printable area starts at 0,0), so the host can render vector output.
 * Pixels are still drawn into the port's own bitmap as usual.
 */
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "qd_internal.h"
#include "tb_internal.h"

static gaddr rec_port;
static int suppressed; /* while a picture shows an image it already recorded */

void qd_rec_suppress(bool on) { suppressed += on ? 1 : -1; }
bool qd_rec_suppressed(void) { return suppressed > 0; }
static machine_print_page *pages;
static int npages, cap_pages;
static bool page_open;

void qd_rec_begin(gaddr port)
{
    qd_rec_end();
    rec_port = port;
}

bool qd_rec_active(gaddr port) { return port && port == rec_port && page_open; }

static void free_op(machine_print_op *op)
{
    free(op->clip);
    free(op->rects);
    free(op->text);
    free(op->image);
}

void qd_rec_end(void)
{
    for (int p = 0; p < npages; p++) {
        for (int i = 0; i < pages[p].nops; i++)
            free_op(&pages[p].ops[i]);
        free(pages[p].ops);
    }
    free(pages);
    pages = NULL;
    npages = cap_pages = 0;
    page_open = false;
    rec_port = 0;
}

void qd_rec_open_page(void)
{
    if (npages == cap_pages) {
        cap_pages = cap_pages ? cap_pages * 2 : 8;
        pages = realloc(pages, (size_t)cap_pages * sizeof *pages);
    }
    memset(&pages[npages++], 0, sizeof *pages);
    page_open = true;
}

void qd_rec_close_page(void) { page_open = false; }

int qd_rec_pages(const machine_print_page **out)
{
    *out = pages;
    return npages;
}

/* Local to device coordinates: the port's bitmap bounds give the origin. */
static void device_offset(gaddr port, int *dh, int *dv)
{
    qrect b = qrect_get(port + GrafPort_portBits_off + 6);
    *dh = -b.left;
    *dv = -b.top;
}

static mprect dev_rect(qrect r, int dh, int dv)
{
    return (mprect){ (int16_t)(r.top + dv), (int16_t)(r.left + dh), (int16_t)(r.bottom + dv), (int16_t)(r.right + dh) };
}

static int region_rects(const hrgn *r, int dh, int dv, mprect **out)
{
    int n = 0, cap = 0;
    mprect *rects = NULL;
    for (int b = 0; b < r->nbands; b++) {
        const rband *band = &r->bands[b];
        for (int i = 0; i + 1 < band->n; i += 2) {
            if (n == cap) {
                cap = cap ? cap * 2 : 8;
                rects = realloc(rects, (size_t)cap * sizeof *rects);
            }
            rects[n++] = dev_rect((qrect){ band->top, band->x[i], band->bottom, band->x[i + 1] }, dh, dv);
        }
    }
    *out = rects;
    return n;
}

static machine_print_op *new_op(gaddr port, int kind)
{
    machine_print_page *pg = &pages[npages - 1];
    pg->ops = realloc(pg->ops, (size_t)(pg->nops + 1) * sizeof *pg->ops);
    machine_print_op *op = &pg->ops[pg->nops++];
    memset(op, 0, sizeof *op);
    op->kind = kind;
    int dh, dv;
    device_offset(port, &dh, &dv);
    hrgn clip;
    hrgn_init(&clip);
    qd_port_clip(port, &clip);
    op->nclip = region_rects(&clip, dh, dv, &op->clip);
    hrgn_free(&clip);
    uint32_t s = rd32(port + GrafPort_pnSize_off);
    op->pen_w = pt_h(s);
    op->pen_h = pt_v(s);
    uint32_t fg, bk;
    qd_port_bits(port, &fg, &bk);
    pixdst pd = qd_port_pixels(port);
    uint16_t rgb[3];
    qd_px_rgb(pd.depth, pd.ctab, fg, rgb);
    op->fg_black = (rgb[0] * 30u + rgb[1] * 59u + rgb[2] * 11u) / 100u < 0x8000u;
    return op;
}

void qd_rec_shape(gaddr port, int kind, int verb, qrect r, int a, int b, const hrgn *shape, const uint8_t pat[8],
                  int mode)
{
    machine_print_op *op = new_op(port, kind);
    int dh, dv;
    device_offset(port, &dh, &dv);
    op->verb = verb;
    op->r = dev_rect(r, dh, dv);
    op->a = (int16_t)a;
    op->b = (int16_t)b;
    memcpy(op->pattern, pat, 8);
    op->mode = mode;
    if (kind == MACHINE_PRINT_REGION && shape)
        op->nrects = region_rects(shape, dh, dv, &op->rects);
}

void qd_rec_line(gaddr port, int h0, int v0, int h1, int v1, const uint8_t pat[8], int mode)
{
    machine_print_op *op = new_op(port, MACHINE_PRINT_LINE);
    int dh, dv;
    device_offset(port, &dh, &dv);
    op->h0 = (int16_t)(h0 + dh);
    op->v0 = (int16_t)(v0 + dv);
    op->h1 = (int16_t)(h1 + dh);
    op->v1 = (int16_t)(v1 + dv);
    memcpy(op->pattern, pat, 8);
    op->mode = mode;
}

void qd_rec_text(gaddr port, int family, int size, int face, int x, int y, const uint8_t *text, int len, int width)
{
    machine_print_op *op = new_op(port, MACHINE_PRINT_TEXT);
    int dh, dv;
    device_offset(port, &dh, &dv);
    op->font_id = (int16_t)family;
    gaddr name = tb_NewPtr(256, false, true);
    tb_GetFontName((int16_t)family, name);
    uint32_t n = rd8(name) < sizeof op->font_name - 1 ? rd8(name) : sizeof op->font_name - 1;
    gget(op->font_name, name + 1, n);
    op->font_name[n] = 0;
    tb_DisposePtr(name);
    op->size = (int16_t)size;
    op->face = (uint8_t)face;
    op->mode = GrafPort_txMode(port);
    op->h0 = (int16_t)(x + dh);
    op->v0 = (int16_t)(y + dv);
    op->width = (int16_t)width;
    op->len = len;
    op->text = malloc(len ? (size_t)len : 1);
    memcpy(op->text, text, (size_t)len);
}

void qd_rec_image(gaddr port, int w, int h, const uint8_t *rgba, qrect src, qrect dst)
{
    /* Crop to the source rectangle. */
    int x0 = src.left < 0 ? 0 : src.left, y0 = src.top < 0 ? 0 : src.top;
    int x1 = src.right > w ? w : src.right, y1 = src.bottom > h ? h : src.bottom;
    if (x1 <= x0 || y1 <= y0)
        return;
    machine_print_op *op = new_op(port, MACHINE_PRINT_IMAGE);
    int dh, dv;
    device_offset(port, &dh, &dv);
    op->r = dev_rect(dst, dh, dv);
    op->image_w = x1 - x0;
    op->image_h = y1 - y0;
    op->image = malloc((size_t)op->image_w * (size_t)op->image_h * 4);
    for (int y = y0; y < y1; y++)
        memcpy(op->image + (size_t)(y - y0) * (size_t)op->image_w * 4, rgba + ((size_t)y * (size_t)w + (size_t)x0) * 4,
               (size_t)op->image_w * 4);
}
