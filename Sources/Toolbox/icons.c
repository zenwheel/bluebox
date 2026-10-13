/*
 * Icon Utilities (IconDispatch): icon suites and plotting.
 *
 * A suite is a handle holding its label and, for each icon family member,
 * the type and data handle: 2 bytes label, 2 bytes count, then 8 bytes per
 * entry.  Plotting picks the member that suits the rectangle (32 or 16
 * pixels; 12-pixel mini icons are drawn from the small ones) and the
 * screen's depth, masks it with the family's black-and-white mask, and
 * applies the transform: disabled lightens, selected darkens.
 */
#include <stdlib.h>
#include <string.h>

#include "qd_internal.h"
#include "tb_internal.h"

enum { ttNone = 0, ttDisabled = 1, ttOffline = 2, ttOpen = 3, ttSelected = 0x4000 };
enum { noMaskFoundErr = -1000, paramErr_ = -50 };

#define SUITE_HDR 4u

static const char *const members[] = { "ICN#", "icl4", "icl8", "ics#", "ics4", "ics8", "icm#", "icm4", "icm8" };
/* GetIconSuite's selector bits, in the order above. */
static const uint32_t member_bits[] = { 0x01, 0x04, 0x02, 0x100, 0x400, 0x200, 0x10000, 0x40000, 0x20000 };

static int suite_count(gaddr suite) { return (int16_t)rd16(rd32(suite) + 2); }
static gaddr suite_entry(gaddr suite, int i) { return rd32(suite) + SUITE_HDR + (uint32_t)i * 8; }

static gaddr suite_get(gaddr suite, uint32_t type)
{
    for (int i = 0; i < suite_count(suite); i++)
        if (rd32(suite_entry(suite, i)) == type)
            return rd32(suite_entry(suite, i) + 4);
    return 0;
}

static void suite_put(gaddr suite, uint32_t type, gaddr data)
{
    for (int i = 0; i < suite_count(suite); i++)
        if (rd32(suite_entry(suite, i)) == type) {
            wr32(suite_entry(suite, i) + 4, data);
            return;
        }
    int n = suite_count(suite);
    tb_SetHandleSize(suite, (int32_t)(SUITE_HDR + (uint32_t)(n + 1) * 8));
    wr16(rd32(suite) + 2, (uint16_t)(n + 1));
    wr32(suite_entry(suite, n), type);
    wr32(suite_entry(suite, n) + 4, data);
}

int16_t tb_NewIconSuite(gaddr suitep)
{
    gaddr h = tb_NewHandle(SUITE_HDR, false, true);
    wr32(suitep, h);
    return h ? noErr : memFullErr;
}

int16_t tb_AddIconToSuite(gaddr icon, gaddr suite, int32_t type)
{
    if (!suite)
        return paramErr_;
    suite_put(suite, (uint32_t)type, icon);
    return noErr;
}

int16_t tb_GetIconFromSuite(gaddr iconp, gaddr suite, int32_t type)
{
    gaddr h = suite ? suite_get(suite, (uint32_t)type) : 0;
    wr32(iconp, h);
    return h ? noErr : resNotFound;
}

int16_t tb_GetIconSuite(gaddr suitep, int16_t res_id, uint32_t selector)
{
    gaddr tmp = tb_NewPtr(4, false, true);
    tb_NewIconSuite(tmp);
    gaddr suite = rd32(tmp);
    tb_DisposePtr(tmp);
    int found = 0;
    for (unsigned i = 0; i < sizeof members / sizeof members[0]; i++) {
        if (!(selector & member_bits[i]))
            continue;
        gaddr h = tb_GetResource((int32_t)fourcc(members[i]), res_id);
        if (h) {
            suite_put(suite, fourcc(members[i]), h);
            found++;
        }
    }
    wr32(suitep, suite);
    return found ? noErr : resNotFound;
}

int16_t tb_DisposeIconSuite(gaddr suite, int8_t dispose_data)
{
    if (!suite)
        return noErr;
    if (dispose_data)
        for (int i = 0; i < suite_count(suite); i++) {
            gaddr h = rd32(suite_entry(suite, i) + 4);
            /* Resources are released by their owner, not disposed. */
            if (h && !(tb_HGetState(h) & 0x20))
                tb_DisposeHandle(h);
        }
    tb_DisposeHandle(suite);
    return noErr;
}

int16_t tb_GetSuiteLabel(gaddr suite) { return suite ? (int16_t)rd16(rd32(suite)) : 0; }

int16_t tb_SetSuiteLabel(gaddr suite, int16_t label)
{
    if (!suite)
        return paramErr_;
    wr16(rd32(suite), (uint16_t)label);
    return noErr;
}

int16_t tb_GetLabel(int16_t label, gaddr color, gaddr str)
{
    if (color)
        gmemset(color, 0, 6);
    if (str)
        wr8(str, 0);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Rendering */

/* An icon as RGBA: `size` square (32, 16 or 12 x 16 for mini icons). */
typedef struct {
    int w, h;
    uint8_t px[32 * 32 * 4];
} icon_img;

static bool bit_at(const uint8_t *bits, int rowbytes, int x, int y)
{
    return bits[y * rowbytes + x / 8] & (0x80 >> (x % 8));
}

/* Colour data `d` (depth 1, 4 or 8) plus a 1-bit mask into img. */
static void render(icon_img *img, int w, int h, int depth, const uint8_t *d, const uint8_t *mask)
{
    img->w = w;
    img->h = h;
    int rb = w * depth / 8;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t v;
            if (depth == 1)
                v = bit_at(d, rb, x, y);
            else if (depth == 4)
                v = (d[y * rb + x / 2] >> (x % 2 ? 0 : 4)) & 15;
            else
                v = d[y * rb + x];
            uint16_t rgb[3];
            if (depth == 8)
                qd_std_rgb8((int)v, rgb);
            else
                qd_px_rgb(depth, 0, v, rgb);
            uint8_t *p = img->px + (y * w + x) * 4;
            p[0] = (uint8_t)(rgb[0] >> 8);
            p[1] = (uint8_t)(rgb[1] >> 8);
            p[2] = (uint8_t)(rgb[2] >> 8);
            p[3] = mask ? (bit_at(mask, w / 8, x, y) ? 255 : 0) : 255;
        }
}

static bool handle_bytes(gaddr h, uint32_t need, uint8_t *out)
{
    if (!h)
        return false;
    if (!rd32(h))
        tb_LoadResource(h);
    if (!rd32(h) || (uint32_t)tb_GetHandleSize(h) < need)
        return false;
    gget(out, rd32(h), need);
    return true;
}

/* The best member of a suite for a rectangle of the given size. */
static bool suite_image(gaddr suite, int size, icon_img *img)
{
    static const struct {
        const char *mask, *c8, *c4;
        int w, h;
    } fam[] = { { "ICN#", "icl8", "icl4", 32, 32 }, { "ics#", "ics8", "ics4", 16, 16 }, { "icm#", "icm8", "icm4", 16, 12 } };
    int depth = qd_screen_depth();
    /* Nearest size first: 32 large, 16 small, 12 mini, then the others
     * from the closest down. */
    static const int order[3][3] = { { 0, 1, 2 }, { 1, 2, 0 }, { 2, 1, 0 } };
    int first = size >= 32 ? 0 : size > 12 ? 1 : 2;
    for (int k = 0; k < 3; k++) {
        int f = order[first][k];
        int w = fam[f].w, h = fam[f].h;
        gaddr mh = suite_get(suite, fourcc(fam[f].mask));
        /* The height follows the data: applications put 16x16 small icons
         * in a suite as its mini icons (Greg's Browser), to be scaled. */
        if (mh && rd32(mh) && tb_GetHandleSize(mh) == w / 8 * 32)
            h = 16;
        uint8_t bw[2 * 32 * 4];
        uint32_t plane = (uint32_t)(w / 8 * h);
        if (!handle_bytes(mh, plane * 2, bw))
            continue;
        uint8_t data[32 * 32];
        if (depth >= 8 && handle_bytes(suite_get(suite, fourcc(fam[f].c8)), (uint32_t)(w * h), data))
            render(img, w, h, 8, data, bw + plane);
        else if (depth >= 4 && handle_bytes(suite_get(suite, fourcc(fam[f].c4)), (uint32_t)(w * h / 2), data))
            render(img, w, h, 4, data, bw + plane);
        else
            render(img, w, h, 1, bw, bw + plane);
        return true;
    }
    return false;
}

/* Alignment within the rectangle and the transform.  The icon keeps its
 * size in a rectangle of a standard size it fits, and is otherwise scaled
 * to fit, keeping its shape.  Alignment: the low two bits place it
 * vertically (0 or 1 centred, 2 top, 3 bottom), the next two horizontally
 * (0 or 4 centred, 8 left, 12 right). */
/* Keep only the drawn part (the mask's bounding box) of an icon. */
static void crop_to_mask(icon_img *img)
{
    int top = img->h, left = img->w, bottom = 0, right = 0;
    for (int y = 0; y < img->h; y++)
        for (int x = 0; x < img->w; x++)
            if (img->px[(y * img->w + x) * 4 + 3]) {
                if (y < top) top = y;
                if (y >= bottom) bottom = y + 1;
                if (x < left) left = x;
                if (x >= right) right = x + 1;
            }
    if (bottom <= top || (top == 0 && left == 0 && bottom == img->h && right == img->w))
        return;
    int w = right - left, h = bottom - top;
    for (int y = 0; y < h; y++)
        memmove(img->px + (size_t)y * (size_t)w * 4, img->px + ((size_t)(y + top) * (size_t)img->w + (size_t)left) * 4,
                (size_t)w * 4);
    img->w = w;
    img->h = h;
}

static void plot(icon_img *img, qrect r, int16_t align, int16_t transform)
{
    int rw = r.right - r.left, rh = r.bottom - r.top;
    /* A small icon in a mini icon's place (16x12): its drawing is rarely the
     * full 16 rows, so it is shown at its own size when what it draws fits,
     * rather than shrunk (which loses rows of detail). */
    if (img->w > rw || img->h > rh) {
        icon_img trimmed = *img;
        crop_to_mask(&trimmed);
        if (trimmed.w <= rw && trimmed.h <= rh)
            *img = trimmed;
    }
    int dw = img->w, dh = img->h;
    bool standard = (rw == 32 || rw == 16 || rw == 12) && (rh == 32 || rh == 16 || rh == 12);
    if (!standard || dw > rw || dh > rh) {
        if (rw * img->h < rh * img->w) { /* width limits */
            dw = rw;
            dh = img->h * rw / img->w;
        } else {
            dh = rh;
            dw = img->w * rh / img->h;
        }
    }
    int left = r.left + (rw - dw) / 2, top = r.top + (rh - dh) / 2;
    switch (align & 3) {
    case 2: top = r.top; break;
    case 3: top = r.bottom - dh; break;
    }
    switch (align & 12) {
    case 8: left = r.left; break;
    case 12: left = r.right - dw; break;
    }
    int t = transform & 0xFF;
    bool sel = transform & ttSelected;
    for (int i = 0; i < img->w * img->h; i++) {
        uint8_t *p = img->px + i * 4;
        for (int c = 0; c < 3; c++) {
            if (t == ttDisabled || t == ttOffline)
                p[c] = (uint8_t)(p[c] + (255 - p[c]) / 2);
            if (sel)
                p[c] = (uint8_t)(p[c] / 2);
        }
        if (t == ttOpen && p[3])
            p[0] = p[1] = p[2] = 0x88;
    }
    qd_draw_rgba(img->px, img->w, img->h, (qrect){ (int16_t)top, (int16_t)left, (int16_t)(top + dh), (int16_t)(left + dw) });
}

int16_t tb_PlotIconSuite(gaddr rect, int16_t align, int16_t transform, gaddr suite)
{
    qrect r = qrect_get(rect);
    icon_img *img = malloc(sizeof *img);
    int size = r.right - r.left < r.bottom - r.top ? r.right - r.left : r.bottom - r.top;
    bool ok = suite && suite_image(suite, size, img);
    if (ok)
        plot(img, r, align, transform);
    free(img);
    return ok ? noErr : noMaskFoundErr;
}

int16_t tb_PlotIconID(gaddr rect, int16_t align, int16_t transform, int16_t res_id)
{
    gaddr sp = tb_NewPtr(4, false, true);
    int16_t err = tb_GetIconSuite(sp, res_id, 0xFFFFFFFFu);
    if (!err)
        err = tb_PlotIconSuite(rect, align, transform, rd32(sp));
    if (rd32(sp))
        tb_DisposeIconSuite(rd32(sp), false);
    tb_DisposePtr(sp);
    return err;
}

/* An ICON (32x32, no mask) or ICN# (data and mask). */
int16_t tb_PlotIconHandle(gaddr rect, int16_t align, int16_t transform, gaddr icon)
{
    if (!icon || !rd32(icon))
        return paramErr_;
    uint8_t d[256];
    int32_t n = tb_GetHandleSize(icon);
    if (n < 128)
        return paramErr_;
    gget(d, rd32(icon), n >= 256 ? 256 : 128);
    icon_img *img = malloc(sizeof *img);
    render(img, 32, 32, 1, d, n >= 256 ? d + 128 : NULL);
    plot(img, qrect_get(rect), align, transform);
    free(img);
    return noErr;
}

/* A SICN: 16x16 1-bit, the first of the list. */
int16_t tb_PlotSICNHandle(gaddr rect, int16_t align, int16_t transform, gaddr icon)
{
    if (!icon || !rd32(icon) || tb_GetHandleSize(icon) < 32)
        return paramErr_;
    uint8_t d[32];
    gget(d, rd32(icon), 32);
    icon_img *img = malloc(sizeof *img);
    render(img, 16, 16, 1, d, NULL);
    for (int i = 0; i < 16 * 16; i++) /* white is transparent */
        if (img->px[i * 4] == 255)
            img->px[i * 4 + 3] = 0;
    plot(img, qrect_get(rect), align, transform);
    free(img);
    return noErr;
}

int16_t tb_PlotCIconHandle(gaddr rect, int16_t align, int16_t transform, gaddr icon)
{
    tb_PlotCIcon(rect, icon);
    return noErr;
}

int16_t tb_PlotIconMethod(gaddr rect, int16_t align, int16_t transform, gaddr method, gaddr data)
{
    return paramErr_;
}

int16_t tb_ForEachIconDo(gaddr suite, uint32_t selector, gaddr action, gaddr data) { return noErr; }

/* ---------------------------------------------------------------------- */
/* Hit testing: by the icon's rectangle */

static qrect icon_rect(qrect r)
{
    int rw = r.right - r.left, rh = r.bottom - r.top;
    int s = rw >= 32 && rh >= 32 ? 32 : 16;
    int left = r.left + (rw - s) / 2, top = r.top + (rh - s) / 2;
    return (qrect){ (int16_t)top, (int16_t)left, (int16_t)(top + s), (int16_t)(left + s) };
}

int16_t tb_IconSuiteToRgn(gaddr rgn, gaddr rect, int16_t align, gaddr suite)
{
    gaddr r = tb_NewPtr(8, false, false);
    qrect_put(r, icon_rect(qrect_get(rect)));
    tb_RectRgn(rgn, r);
    tb_DisposePtr(r);
    return noErr;
}

int16_t tb_IconIDToRgn(gaddr rgn, gaddr rect, int16_t align, int16_t icon_id)
{
    return tb_IconSuiteToRgn(rgn, rect, align, 0);
}

int8_t tb_PtInIconSuite(uint32_t pt, gaddr rect, int16_t align, gaddr suite)
{
    qrect r = icon_rect(qrect_get(rect));
    return pt_v(pt) >= r.top && pt_v(pt) < r.bottom && pt_h(pt) >= r.left && pt_h(pt) < r.right;
}

int8_t tb_PtInIconID(uint32_t pt, gaddr rect, int16_t align, int16_t icon_id)
{
    return tb_PtInIconSuite(pt, rect, align, 0);
}

int8_t tb_RectInIconSuite(gaddr test, gaddr rect, int16_t align, gaddr suite)
{
    qrect a = qrect_get(test), r = icon_rect(qrect_get(rect));
    return a.left < r.right && a.right > r.left && a.top < r.bottom && a.bottom > r.top;
}

int8_t tb_RectInIconID(gaddr test, gaddr rect, int16_t align, int16_t icon_id)
{
    return tb_RectInIconSuite(test, rect, align, 0);
}
