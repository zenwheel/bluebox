/*
 * Icon Utilities (IconDispatch): icon suites and plotting.
 *
 * A suite is a handle holding its label and, for each icon family member,
 * the type and data handle: 2 bytes label, 2 bytes count, then 8 bytes per
 * entry.  Plotting picks the member that suits the rectangle (32 or 16
 * pixels; 12-pixel mini icons are drawn from the small ones) and the
 * screen's depth, masks it with the family's black-and-white mask, and
 * applies the transform: disabled lightens, selected darkens, a label
 * tints it with the label's colour.  An icon cache is a suite that asks
 * its getter routine for the members it lacks; the ...Method calls work
 * through a cache made for the call.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "icons_internal.h"
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

static void suite_put(gaddr suite, uint32_t type, gaddr data);

/* Icon caches: the suites made by MakeIconCache, with their getter
 * routine and its data. */
#define MAX_CACHES 256
static struct {
    gaddr suite, proc, data;
} caches[MAX_CACHES];

static int cache_index(gaddr suite)
{
    for (int i = 0; suite && i < MAX_CACHES; i++)
        if (caches[i].suite == suite)
            return i;
    return -1;
}

/* FUNCTION getter(theType: ResType; yourDataPtr: Ptr): Handle */
static gaddr call_getter(gaddr proc, uint32_t type, gaddr data)
{
    cpu_push32(g_cpu, 0);
    cpu_push32(g_cpu, type);
    cpu_push32(g_cpu, data);
    guest_call(proc);
    return cpu_pop32(g_cpu);
}

static gaddr suite_get(gaddr suite, uint32_t type)
{
    for (int i = 0; i < suite_count(suite); i++)
        if (rd32(suite_entry(suite, i)) == type) {
            gaddr h = rd32(suite_entry(suite, i) + 4);
            if (h || cache_index(suite) < 0)
                return h;
        }
    int c = cache_index(suite);
    if (c < 0 || !caches[c].proc)
        return 0;
    gaddr h = call_getter(caches[c].proc, type, caches[c].data);
    if (h)
        suite_put(suite, type, h);
    return h;
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

/* A suite has a place for each member of the family: one it lacks is a
 * nil handle and noErr (the Icon Utilities read the slot), not an error.
 * Icon Machine's paint pane makes an empty suite, asks for its 'ics#' and
 * gives up on any error, leaving the window without the pane. */
int16_t tb_GetIconFromSuite(gaddr iconp, gaddr suite, int32_t type)
{
    gaddr h = suite ? suite_get(suite, (uint32_t)type) : 0;
    wr32(iconp, h);
    if (h)
        return noErr;
    if (!suite)
        return paramErr_;
    for (unsigned i = 0; i < sizeof members / sizeof members[0]; i++)
        if ((uint32_t)type == fourcc(members[i]))
            return noErr;
    return paramErr_;
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
    int c = cache_index(suite);
    if (c >= 0)
        caches[c].suite = 0;
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

/* The Finder's labels: System 7's colours and names, until the Labels
 * control panel (SetLabel) changes them.  Label 0 is none: black. */
static struct {
    uint16_t rgb[3];
    char name[32];
} labels[8];
static bool labels_set;

static void labels_init(void)
{
    static const struct {
        uint16_t r, g, b;
        const char *name;
    } std[8] = { { 0, 0, 0, "None" },
                 { 0xFFFF, 0x648A, 0x028C, "Essential" },
                 { 0xDD6B, 0x08C2, 0x06A2, "Hot" },
                 { 0xF2D7, 0x0856, 0x84EC, "In Progress" },
                 { 0x0241, 0xAB54, 0xEAFF, "Cool" },
                 { 0x0000, 0x0000, 0xD400, "Personal" },
                 { 0x0000, 0x64AF, 0x11B0, "Project 1" },
                 { 0x5600, 0x2C9D, 0x0524, "Project 2" } };
    for (int i = 0; i < 8; i++) {
        labels[i].rgb[0] = std[i].r;
        labels[i].rgb[1] = std[i].g;
        labels[i].rgb[2] = std[i].b;
        snprintf(labels[i].name, sizeof labels[i].name, "%s", std[i].name);
    }
    labels_set = true;
}

void icons_reset(void)
{
    labels_set = false;
    memset(caches, 0, sizeof caches);
}

int16_t tb_GetLabel(int16_t label, gaddr color, gaddr str)
{
    if (label < 0 || label > 7)
        return paramErr_;
    if (!labels_set)
        labels_init();
    if (color)
        for (int c = 0; c < 3; c++)
            wr16(color + (uint32_t)c * 2, labels[label].rgb[c]);
    if (str) {
        size_t n = strlen(labels[label].name);
        wr8(str, (uint8_t)n);
        gput(str + 1, labels[label].name, (uint32_t)n);
    }
    return noErr;
}

/* SetLabel (IconDispatch $50C, the Labels control panel's): a label's
 * colour and name for the rest of the run. */
int16_t tb_SetLabel(int16_t label, gaddr color, gaddr str)
{
    if (label < 1 || label > 7)
        return paramErr_;
    if (!labels_set)
        labels_init();
    if (color)
        for (int c = 0; c < 3; c++)
            labels[label].rgb[c] = rd16(color + (uint32_t)c * 2);
    if (str) {
        int n = rd8(str) > 31 ? 31 : rd8(str);
        gget(labels[label].name, str + 1, (uint32_t)n);
        labels[label].name[n] = 0;
    }
    return noErr;
}

/* IconDispatch $218, which System 7.1's Finder calls with an item's icon
 * cache before drawing it and with nil after: what it is for is not
 * known, and drawing needs nothing from it, so it is kept and nothing
 * more. */
static gaddr icon_context;
void tb_IconDispatch18(int32_t context) { icon_context = (gaddr)context; }

/* ---------------------------------------------------------------------- */
/* Rendering */

static bool bit_at(const uint8_t *bits, int rowbytes, int x, int y)
{
    return bits[y * rowbytes + x / 8] & (0x80 >> (x % 8));
}

/* Colour data `d` (depth 1, 4 or 8) plus a 1-bit mask into img. */
void icon_render(icon_img *img, int w, int h, int depth, const uint8_t *d, const uint8_t *mask)
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
            icon_render(img, w, h, 8, data, bw + plane);
        else if (depth >= 4 && handle_bytes(suite_get(suite, fourcc(fam[f].c4)), (uint32_t)(w * h / 2), data))
            icon_render(img, w, h, 4, data, bw + plane);
        else
            icon_render(img, w, h, 1, bw, bw + plane);
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

qrect icon_place(icon_img *img, qrect r, int16_t align)
{
    int rw = r.right - r.left, rh = r.bottom - r.top;
    /* A small icon in a mini icon's place (16x12): its drawing is rarely the
     * full 16 rows, so it is shown at its own size when what it draws fits,
     * rather than shrunk (which loses rows of detail). */
    if (img->w > rw || img->h > rh) {
        icon_img *trimmed = malloc(sizeof *trimmed);
        *trimmed = *img;
        crop_to_mask(trimmed);
        if (trimmed->w <= rw && trimmed->h <= rh)
            *img = *trimmed;
        free(trimmed);
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
    return (qrect){ (int16_t)top, (int16_t)left, (int16_t)(top + dh), (int16_t)(left + dw) };
}

void icon_plot(icon_img *img, qrect r, int16_t align, int16_t transform)
{
    qrect dst = icon_place(img, r, align);
    int t = transform & 0xFF;
    bool sel = transform & ttSelected;
    int label = (transform >> 8) & 7;
    if (label && !labels_set)
        labels_init();
    for (int i = 0; i < img->w * img->h; i++) {
        uint8_t *p = img->px + i * 4;
        /* A label tints the icon on a colour screen: its colours are
         * multiplied by the label's, so white takes the label colour. */
        if (label && qd_screen_depth() >= 4)
            for (int c = 0; c < 3; c++)
                p[c] = (uint8_t)(p[c] * (labels[label].rgb[c] >> 8) / 255);
        for (int c = 0; c < 3; c++) {
            if (t == ttDisabled || t == ttOffline)
                p[c] = (uint8_t)(p[c] + (255 - p[c]) / 2);
            if (sel)
                p[c] = (uint8_t)(p[c] / 2);
        }
        if (t == ttOpen && p[3])
            p[0] = p[1] = p[2] = 0x88;
    }
    qd_draw_rgba(img->px, img->w, img->h, dst);
}

int16_t tb_PlotIconSuite(gaddr rect, int16_t align, int16_t transform, gaddr suite)
{
    qrect r = qrect_get(rect);
    icon_img *img = malloc(sizeof *img);
    int size = r.right - r.left < r.bottom - r.top ? r.right - r.left : r.bottom - r.top;
    bool ok = suite && suite_image(suite, size, img);
    /* The suite's own label, when the transform names none. */
    if (ok && !(transform & 0x0700))
        transform = (int16_t)(transform | (tb_GetSuiteLabel(suite) & 7) << 8);
    if (ok)
        icon_plot(img, r, align, transform);
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
    icon_render(img, 32, 32, 1, d, n >= 256 ? d + 128 : NULL);
    icon_plot(img, qrect_get(rect), align, transform);
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
    icon_render(img, 16, 16, 1, d, NULL);
    for (int i = 0; i < 16 * 16; i++) /* white is transparent */
        if (img->px[i * 4] == 255)
            img->px[i * 4 + 3] = 0;
    icon_plot(img, qrect_get(rect), align, transform);
    free(img);
    return noErr;
}

int16_t tb_PlotCIconHandle(gaddr rect, int16_t align, int16_t transform, gaddr icon)
{
    tb_PlotCIcon(rect, icon);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Icon caches and getter methods */

int16_t tb_MakeIconCache(gaddr cachep, gaddr proc, gaddr data)
{
    int16_t err = tb_NewIconSuite(cachep);
    if (err)
        return err;
    for (int i = 0; i < MAX_CACHES; i++)
        if (!caches[i].suite) {
            caches[i].suite = rd32(cachep);
            caches[i].proc = proc;
            caches[i].data = data;
            return noErr;
        }
    tb_DisposeIconSuite(rd32(cachep), false);
    wr32(cachep, 0);
    return memFullErr;
}

/* The members a plot in this rectangle would use, got now. */
int16_t tb_LoadIconCache(gaddr rect, int16_t align, int16_t transform, gaddr cache)
{
    if (cache_index(cache) < 0)
        return paramErr_;
    icon_img *img = malloc(sizeof *img);
    qrect r = qrect_get(rect);
    int size = r.right - r.left < r.bottom - r.top ? r.right - r.left : r.bottom - r.top;
    bool ok = suite_image(cache, size, img);
    free(img);
    return ok ? noErr : noMaskFoundErr;
}

int16_t tb_GetIconCacheData(gaddr cache, gaddr datap)
{
    int c = cache_index(cache);
    if (c < 0)
        return paramErr_;
    wr32(datap, caches[c].data);
    return noErr;
}

int16_t tb_SetIconCacheData(gaddr cache, gaddr data)
{
    int c = cache_index(cache);
    if (c < 0)
        return paramErr_;
    caches[c].data = data;
    return noErr;
}

int16_t tb_GetIconCacheProc(gaddr cache, gaddr procp)
{
    int c = cache_index(cache);
    if (c < 0)
        return paramErr_;
    wr32(procp, caches[c].proc);
    return noErr;
}

int16_t tb_SetIconCacheProc(gaddr cache, gaddr proc)
{
    int c = cache_index(cache);
    if (c < 0)
        return paramErr_;
    caches[c].proc = proc;
    return noErr;
}

/* A cache for one ...Method call, disposed of after it (the icons the
 * getter gives stay the caller's). */
static gaddr method_cache(gaddr method, gaddr data)
{
    gaddr tmp = tb_NewPtr(4, false, true);
    gaddr cache = tmp && !tb_MakeIconCache(tmp, method, data) ? rd32(tmp) : 0;
    if (tmp)
        tb_DisposePtr(tmp);
    return cache;
}

int16_t tb_PlotIconMethod(gaddr rect, int16_t align, int16_t transform, gaddr method, gaddr data)
{
    gaddr cache = method_cache(method, data);
    if (!cache)
        return memFullErr;
    int16_t err = tb_PlotIconSuite(rect, align, transform, cache);
    tb_DisposeIconSuite(cache, false);
    return err;
}

/* FUNCTION action(theType: ResType; VAR theIcon: Handle; yourDataPtr: Ptr): OSErr,
 * for each member the selector names, present or not; what it leaves in
 * theIcon becomes the member. */
int16_t tb_ForEachIconDo(gaddr suite, uint32_t selector, gaddr action, gaddr data)
{
    if (!suite || !action)
        return paramErr_;
    gaddr hp = tb_NewPtr(4, false, true);
    if (!hp)
        return memFullErr;
    int16_t err = noErr;
    for (unsigned i = 0; i < sizeof members / sizeof members[0] && !err; i++) {
        if (!(selector & member_bits[i]))
            continue;
        uint32_t type = fourcc(members[i]);
        gaddr before = suite_get(suite, type);
        wr32(hp, before);
        cpu_push16(g_cpu, 0);
        cpu_push32(g_cpu, type);
        cpu_push32(g_cpu, hp);
        cpu_push32(g_cpu, data);
        guest_call(action);
        err = (int16_t)cpu_pop16(g_cpu);
        if (rd32(hp) != before)
            suite_put(suite, type, rd32(hp));
    }
    tb_DisposePtr(hp);
    return err;
}

/* ---------------------------------------------------------------------- */
/* Hit testing and regions: by the icon's mask, where it is drawn */

/* The mask of the member a plot in r would use, as a region (global to
 * the rectangle's coordinates); false when the suite has no mask. */
static bool suite_mask_rgn(gaddr suite, qrect r, int16_t align, hrgn *out)
{
    icon_img *img = malloc(sizeof *img);
    int size = r.right - r.left < r.bottom - r.top ? r.right - r.left : r.bottom - r.top;
    if (!suite || !suite_image(suite, size, img)) {
        free(img);
        return false;
    }
    qrect d = icon_place(img, r, align);
    int dw = d.right - d.left, dh = d.bottom - d.top;
    hrgn_init(out);
    if (dw <= 0 || dh <= 0 || dw > ICON_MAX) {
        free(img);
        return true;
    }
    int16_t runs[ICON_MAX + 2];
    for (int y = 0; y < dh; y++) {
        int sy = y * img->h / dh, start = -1, n = 0;
        for (int x = 0; x <= dw; x++) {
            bool on = x < dw && img->px[(sy * img->w + x * img->w / dw) * 4 + 3];
            if (on && start < 0)
                start = x;
            if (!on && start >= 0 && n + 2 <= ICON_MAX + 2) {
                runs[n++] = (int16_t)(d.left + start);
                runs[n++] = (int16_t)(d.left + x);
                start = -1;
            }
        }
        hrgn_add_runs(out, d.top + y, runs, n);
    }
    free(img);
    return true;
}

/* Without a mask to go by, the icon's square. */
static qrect icon_rect(qrect r)
{
    int rw = r.right - r.left, rh = r.bottom - r.top;
    int s = rw >= 32 && rh >= 32 ? 32 : 16;
    int left = r.left + (rw - s) / 2, top = r.top + (rh - s) / 2;
    return (qrect){ (int16_t)top, (int16_t)left, (int16_t)(top + s), (int16_t)(left + s) };
}

static void suite_rgn(gaddr suite, qrect r, int16_t align, hrgn *out)
{
    if (!suite_mask_rgn(suite, r, align, out)) {
        hrgn_init(out);
        hrgn_rect(out, icon_rect(r));
    }
}

int16_t tb_IconSuiteToRgn(gaddr rgn, gaddr rect, int16_t align, gaddr suite)
{
    hrgn h;
    suite_rgn(suite, qrect_get(rect), align, &h);
    rgn_write(rgn, &h);
    hrgn_free(&h);
    return noErr;
}

static gaddr id_suite(int16_t icon_id)
{
    gaddr sp = tb_NewPtr(4, false, true);
    gaddr suite = sp && !tb_GetIconSuite(sp, icon_id, 0xFFFFFFFFu) ? rd32(sp) : 0;
    if (sp && !suite && rd32(sp))
        tb_DisposeIconSuite(rd32(sp), false);
    if (sp)
        tb_DisposePtr(sp);
    return suite;
}

int16_t tb_IconIDToRgn(gaddr rgn, gaddr rect, int16_t align, int16_t icon_id)
{
    gaddr suite = id_suite(icon_id);
    int16_t err = tb_IconSuiteToRgn(rgn, rect, align, suite);
    if (suite)
        tb_DisposeIconSuite(suite, false);
    return err;
}

int8_t tb_PtInIconSuite(uint32_t pt, gaddr rect, int16_t align, gaddr suite)
{
    hrgn h;
    suite_rgn(suite, qrect_get(rect), align, &h);
    bool in = hrgn_contains(&h, pt_h(pt), pt_v(pt));
    hrgn_free(&h);
    return in;
}

int8_t tb_PtInIconID(uint32_t pt, gaddr rect, int16_t align, int16_t icon_id)
{
    gaddr suite = id_suite(icon_id);
    int8_t in = tb_PtInIconSuite(pt, rect, align, suite);
    if (suite)
        tb_DisposeIconSuite(suite, false);
    return in;
}

int8_t tb_RectInIconSuite(gaddr test, gaddr rect, int16_t align, gaddr suite)
{
    hrgn h, t;
    suite_rgn(suite, qrect_get(rect), align, &h);
    hrgn_init(&t);
    hrgn_rect(&t, qrect_get(test));
    hrgn_op(&t, &t, &h, RGN_SECT);
    bool in = !hrgn_empty(&t);
    hrgn_free(&h);
    hrgn_free(&t);
    return in;
}

int8_t tb_RectInIconID(gaddr test, gaddr rect, int16_t align, int16_t icon_id)
{
    gaddr suite = id_suite(icon_id);
    int8_t in = tb_RectInIconSuite(test, rect, align, suite);
    if (suite)
        tb_DisposeIconSuite(suite, false);
    return in;
}

int8_t tb_PtInIconMethod(uint32_t pt, gaddr rect, int16_t align, gaddr method, gaddr data)
{
    gaddr cache = method_cache(method, data);
    int8_t in = tb_PtInIconSuite(pt, rect, align, cache);
    if (cache)
        tb_DisposeIconSuite(cache, false);
    return in;
}

int8_t tb_RectInIconMethod(gaddr test, gaddr rect, int16_t align, gaddr method, gaddr data)
{
    gaddr cache = method_cache(method, data);
    int8_t in = tb_RectInIconSuite(test, rect, align, cache);
    if (cache)
        tb_DisposeIconSuite(cache, false);
    return in;
}

int16_t tb_IconMethodToRgn(gaddr rgn, gaddr rect, int16_t align, gaddr method, gaddr data)
{
    gaddr cache = method_cache(method, data);
    int16_t err = tb_IconSuiteToRgn(rgn, rect, align, cache);
    if (cache)
        tb_DisposeIconSuite(cache, false);
    return err;
}
