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
    int fg, bk;
    qd_port_bits(port, &fg, &bk);
    op->fg_black = fg != 0;
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
