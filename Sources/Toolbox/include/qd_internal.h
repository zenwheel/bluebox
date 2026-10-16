/* QuickDraw internals shared between the QuickDraw source files. */
#ifndef QD_INTERNAL_H
#define QD_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "mem.h"

/* ---- Rectangles and points (host side) ---- */

typedef struct {
    int16_t top, left, bottom, right;
} qrect;

static inline qrect qrect_get(gaddr r)
{
    return (qrect){ (int16_t)rd16(r), (int16_t)rd16(r + 2), (int16_t)rd16(r + 4), (int16_t)rd16(r + 6) };
}
static inline void qrect_put(gaddr r, qrect q)
{
    wr16(r, (uint16_t)q.top);
    wr16(r + 2, (uint16_t)q.left);
    wr16(r + 4, (uint16_t)q.bottom);
    wr16(r + 6, (uint16_t)q.right);
}
static inline bool qrect_empty(qrect r) { return r.bottom <= r.top || r.right <= r.left; }
static inline int16_t pt_v(uint32_t p) { return (int16_t)(p >> 16); }
static inline int16_t pt_h(uint32_t p) { return (int16_t)(p & 0xFFFF); }
static inline uint32_t make_pt(int16_t v, int16_t h) { return ((uint32_t)(uint16_t)v << 16) | (uint16_t)h; }

/* ---- Regions (host side) ----
 * A region is a list of horizontal bands; each band covers rows [top, bottom)
 * and holds sorted, disjoint x intervals [x0, x1). */

typedef struct {
    int16_t top, bottom;
    int n;           /* number of x values (2 per interval) */
    int16_t *x;
} rband;

typedef struct {
    int nbands, cap;
    rband *bands;
} hrgn;

void hrgn_init(hrgn *r);
void hrgn_free(hrgn *r);
void hrgn_rect(hrgn *r, qrect q);
bool hrgn_empty(const hrgn *r);
qrect hrgn_bbox(const hrgn *r);
void hrgn_copy(hrgn *dst, const hrgn *src);
void hrgn_offset(hrgn *r, int dh, int dv);
void hrgn_inset(hrgn *r, int dh, int dv);
enum { RGN_UNION, RGN_SECT, RGN_DIFF, RGN_XOR };
void hrgn_op(hrgn *dst, const hrgn *a, const hrgn *b, int op);
bool hrgn_contains(const hrgn *r, int h, int v);
bool hrgn_equal(const hrgn *a, const hrgn *b);
/* Add one row's interval [x0, x1) at row y (rows must be added in order). */
void hrgn_add_row(hrgn *r, int y, int x0, int x1);
/* Add one row's sorted, disjoint intervals (x0, x1, x0, x1 …); rows in order. */
void hrgn_add_runs(hrgn *r, int y, const int16_t *x, int n);
/* Build from a per-row list of intervals: rows[y - top] = {x0, x1} (empty if x0 >= x1). */
void hrgn_from_rows(hrgn *r, int top, int nrows, const int16_t (*rows)[2]);

/* Guest region handles. */
void rgn_read(gaddr rh, hrgn *out);
void rgn_write(gaddr rh, const hrgn *r);
gaddr rgn_new_rect(qrect q);

/* ---- Ports and drawing ---- */

/* Pixel destination: a BitMap or PixMap resolved to concrete numbers. */
typedef struct {
    gaddr base;
    uint32_t row_bytes;
    qrect bounds;   /* in the port's local coordinates */
    int depth;      /* bits per pixel: 1, 2, 4, 8, 16, 32 */
    gaddr ctab;     /* colour table handle for indexed depths (0: the standard one) */
} pixdst;

/* No wider than its rows hold: a PixMap whose pixelSize disagrees with its
 * rowBytes (set up for another screen depth) is drawn into only as far as
 * each row reaches, and not past the end of its buffer. */
static inline void pixdst_clamp(pixdst *d)
{
    int fit = (int)(d->row_bytes * 8u / (unsigned)(d->depth > 0 ? d->depth : 1));
    if (d->bounds.right - d->bounds.left > fit)
        d->bounds.right = (int16_t)(d->bounds.left + fit);
}

gaddr qd_port(void);
bool qd_port_is_color(gaddr port);
pixdst qd_port_pixels(gaddr port);
/* visRgn ∩ clipRgn ∩ bitmap bounds, in local coordinates. */
void qd_port_clip(gaddr port, hrgn *out);

/* ---- Colour ---- */

/* The screen's depth (1 or 8) and colour table. */
int qd_screen_depth(void);
gaddr qd_screen_ctab(void);
/* Pixel value <-> RGB for a depth and colour table. */
void qd_px_rgb(int depth, gaddr ctab, uint32_t v, uint16_t rgb[3]);
uint32_t qd_rgb_px(int depth, gaddr ctab, const uint16_t rgb[3]);
/* The standard Macintosh colour for an 8-bit index. */
void qd_std_rgb8(int i, uint16_t rgb[3]);

/* ---- Pixel access (raw values: bit, index, or 16/32-bit RGB) ---- */

static inline uint32_t px_mask(int depth) { return depth >= 32 ? 0xFFFFFFu : (1u << depth) - 1; }

static inline uint32_t get_px(const pixdst *d, int col, int row)
{
    gaddr rowp = d->base + (uint32_t)row * d->row_bytes;
    switch (d->depth) {
    case 1: return (rd8(rowp + (uint32_t)(col >> 3)) >> (7 - (col & 7))) & 1;
    case 8: return rd8(rowp + (uint32_t)col);
    case 16: return rd16(rowp + (uint32_t)col * 2);
    case 32: return rd32(rowp + (uint32_t)col * 4) & 0xFFFFFF;
    default: {
        int bit = col * d->depth;
        return (uint32_t)(rd8(rowp + (uint32_t)(bit >> 3)) >> (8 - d->depth - (bit & 7))) & px_mask(d->depth);
    }
    }
}

static inline void put_px(const pixdst *d, int col, int row, uint32_t v)
{
    gaddr rowp = d->base + (uint32_t)row * d->row_bytes;
    switch (d->depth) {
    case 1: {
        gaddr a = rowp + (uint32_t)(col >> 3);
        uint8_t m = (uint8_t)(0x80 >> (col & 7));
        wr8(a, v ? (uint8_t)(rd8(a) | m) : (uint8_t)(rd8(a) & ~m));
        break;
    }
    case 8: wr8(rowp + (uint32_t)col, (uint8_t)v); break;
    case 16: wr16(rowp + (uint32_t)col * 2, (uint16_t)v); break;
    case 32: wr32(rowp + (uint32_t)col * 4, v & 0xFFFFFF); break;
    default: {
        int bit = col * d->depth, shift = 8 - d->depth - (bit & 7);
        gaddr a = rowp + (uint32_t)(bit >> 3);
        uint8_t m = (uint8_t)(px_mask(d->depth) << shift);
        wr8(a, (uint8_t)((rd8(a) & ~m) | ((v << shift) & m)));
        break;
    }
    }
}

/* Apply a transfer mode for one pixel: s is the source/pattern bit, fg and
 * bk are pixel values at the destination's depth, inv inverts a pixel. */
static inline uint32_t apply_mode(int mode, int s, uint32_t dst, uint32_t fg, uint32_t bk, uint32_t inv)
{
    if (mode & 4)
        s = !s;
    switch (mode & 3) {
    case 0: return s ? fg : bk;         /* copy */
    case 1: return s ? fg : dst;        /* or */
    case 2: return s ? dst ^ inv : dst; /* xor */
    default: return s ? bk : dst;       /* bic */
    }
}

/* Clip intervals of a region on row y: returns the band's x list (or NULL). */
const int16_t *hrgn_row(const hrgn *r, int y, int *n);
/* Draw with this foreground colour instead of the port's (NULL to stop);
 * for the system's own colour drawing in old-style ports. */
void qd_set_fg_override(const uint16_t *rgb);
/* The port's foreground and background as pixel values for its depth. */
void qd_port_bits(gaddr port, uint32_t *fg, uint32_t *bk);
/* PmForeColor/PmBackColor: draw with this index of the screen's table. */
void qd_port_set_index(gaddr port, bool fore, uint32_t index);
void qd_port_forget(gaddr port);
void qd_port_pattern(gaddr port, int which, uint8_t out[8]);

/* Fill a region with an 8x8 1-bit pattern using a pattern transfer mode
 * (patCopy..notPatBic = 8..15), in the current port's colours. */
void qd_fill_region(gaddr port, const hrgn *shape, const uint8_t pat[8], int mode);
/* Shape helpers. */
void qd_oval_region(hrgn *out, qrect r);
void qd_rrect_region(hrgn *out, qrect r, int ow, int oh);
/* Region covered by the pen moving from (h0,v0) to (h1,v1). */
void qd_line_region(hrgn *out, int h0, int v0, int h1, int v1, int pw, int ph);

/* ---- Recording (printing) ---- */

/* Start recording drawing in port; qd_rec_end discards the recording. */
void qd_rec_begin(gaddr port);
void qd_rec_end(void);
bool qd_rec_active(gaddr port);
void qd_rec_open_page(void);
void qd_rec_close_page(void);
void qd_rec_shape(gaddr port, int kind, int verb, qrect r, int a, int b, const hrgn *shape, const uint8_t pat[8],
                  int mode);
void qd_rec_line(gaddr port, int h0, int v0, int h1, int v1, const uint8_t pat[8], int mode);
void qd_rec_text(gaddr port, int family, int size, int face, int x, int y, const uint8_t *text, int len, int width);
/* An RGBA image (w x h) whose src part is shown in dst (local coordinates). */
void qd_rec_image(gaddr port, int w, int h, const uint8_t *rgba, qrect src, qrect dst);
/* CopyBits does not record while suppressed (the caller recorded already). */
void qd_rec_suppress(bool on);
bool qd_rec_suppressed(void);

/* A PixPat handle for a black-and-white pattern. */
gaddr qd_pixpat_for(const uint8_t pat[8]);
/* A cicn's image as RGBA (malloc'd); NULL if it cannot be read. */
uint8_t *qd_cicn_rgba(const uint8_t *d, uint32_t len, int *w, int *h);
/* Draw RGBA pixels (transparent where alpha is 0) scaled into dst in the
 * current port, through its clip. */
void qd_draw_rgba(const uint8_t *rgba, int w, int h, qrect dst);

/* Draw text at the pen location, advancing it. */
void qd_draw_text(gaddr port, const uint8_t *text, int len);
int qd_text_width(gaddr port, const uint8_t *text, int len);

/* Screen. */
void qd_screen_init(int width, int height);
extern int g_screen_width, g_screen_height;

/* Standard patterns. */
extern const uint8_t qd_pat_black[8], qd_pat_white[8], qd_pat_gray[8], qd_pat_ltgray[8], qd_pat_dkgray[8];

/* Screen depth changes (qd_screen.c). */
void qd_screen_port_opened(gaddr port);
void qd_screen_port_closed(gaddr port);
bool qd_screen_supports(int depth);
bool qd_screen_set_depth(int depth);
gaddr qd_screen_bits(void);
void wm_repaint_all(void);

#endif
