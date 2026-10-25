/*
 * DrawSprocket, as PowerPC games call it (DrawSprocketLib): one context, the
 * screen as it is.  A game asks for a display of some size and depth; it
 * gets a window covering the screen, black, with the display centred in
 * it (the front buffer), and a back buffer that is an offscreen GWorld of
 * the size and depth it asked for.  Swapping copies the back buffer to the
 * front.  Palettes go to the back buffer's colour table (and to the screen
 * when it is indexed); gamma fades are accepted and do nothing.
 *
 * DSpContextAttributes: frequency 0, displayWidth 4, displayHeight 8,
 * colorNeeds 20, colorTable 24, contextOptions 28, backBufferDepthMask 32,
 * displayDepthMask 36, backBufferBestDepth 40, displayBestDepth 44,
 * pageCount 48, gameMustConfirmSwitch 55; 72 bytes.
 */
#include <string.h>

#include "machine.h"
#include "ppc_bridge.h"
#include "qd_internal.h"
#include "tb_internal.h"

enum { ATTR_SIZE = 72 };
enum { kDSpContextState_Active = 0, kDSpContextState_Paused = 1, kDSpContextState_Inactive = 2 };
enum { kDSpBufferKind_Normal = 0 };
enum {
    kDSpNotInitializedErr = -30440,
    kDSpSystemSWTooOldErr = -30441,
    kDSpInvalidContextErr = -30442,
    kDSpInvalidAttributesErr = -30443,
    kDSpContextAlreadyReservedErr = -30444,
    kDSpContextNotReservedErr = -30445,
    kDSpContextNotFoundErr = -30446,
};

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

/* The one context, in guest memory so it has an address to hand out:
 * the attributes reserved, then our own fields. */
static gaddr ctx;
static int depth_before; /* the screen's depth before a context changed it */
#define CTX_STATE (ATTR_SIZE + 0)    /* word */
#define CTX_RESERVED (ATTR_SIZE + 2) /* byte */
#define CTX_WINDOW (ATTR_SIZE + 4)   /* the front buffer's window */
#define CTX_GWORLD (ATTR_SIZE + 8)   /* the back buffer */
#define CTX_LEFT (ATTR_SIZE + 12)    /* the display's place in the window */
#define CTX_TOP (ATTR_SIZE + 14)
#define CTX_SIZE (ATTR_SIZE + 16)

static bool started;

static int screen_depth(void)
{
    gaddr gd = rd32(LM_MainDevice);
    gaddr pm = gd && rd32(gd) ? rd32(rd32(gd) + GDevice_gdPMap_off) : 0;
    return pm && rd32(pm) ? (int16_t)rd16(rd32(pm) + PixMap_pixelSize_off) : 8;
}

/* The deepest depth in a mask (bit n: depth 2^n) no deeper than 32. */
static int depth_from_mask(uint32_t mask, int fallback)
{
    for (int d = 32; d >= 1; d /= 2)
        if (mask & (uint32_t)d)
            return d;
    return fallback;
}

static void context_make(void)
{
    if (ctx)
        return;
    ctx = tb_NewPtr(CTX_SIZE, true, true);
    /* What the screen offers: its size, its depth. */
    wr32(ctx + 4, (uint32_t)g_screen_width);
    wr32(ctx + 8, (uint32_t)g_screen_height);
    wr32(ctx + 0, 60u << 16);
    int d = screen_depth();
    wr32(ctx + 32, 0x3F);
    wr32(ctx + 36, 0x3F);
    wr32(ctx + 40, (uint32_t)d);
    wr32(ctx + 44, (uint32_t)d);
    wr32(ctx + 48, 1);
}

static bool is_context(gaddr p) { return p && p == ctx; }

static void dsp_Startup(ppc_cpu *c)
{
    started = true;
    context_make();
    RETURN(0);
}
static void dsp_Shutdown(ppc_cpu *c)
{
    started = false;
    RETURN(0);
}
/* 1.7, as a NumVersion through the result pointer in r3 */
static void dsp_GetVersion(ppc_cpu *c)
{
    if (R(0))
        wr32(R(0), 0x01708000);
    c->pc = c->lr;
}

/* DSpFindBestContext(desired, *context): ours, whatever was asked. */
static void dsp_FindBestContext(ppc_cpu *c)
{
    context_make();
    if (R(1))
        wr32(R(1), ctx);
    RETURN(0);
}
static void dsp_FindBestContextOnDisplayID(ppc_cpu *c) { dsp_FindBestContext(c); }
static void dsp_GetFirstContext(ppc_cpu *c)
{
    context_make();
    if (R(1))
        wr32(R(1), ctx);
    RETURN(0);
}
static void dsp_GetNextContext(ppc_cpu *c)
{
    if (R(1))
        wr32(R(1), 0);
    RETURN(kDSpContextNotFoundErr);
}
static void dsp_GetAttributes(ppc_cpu *c)
{
    if (!is_context(R(0)))
        RETURN(kDSpInvalidContextErr);
    else {
        if (R(1))
            ppc_copy(R(1), ctx, ATTR_SIZE);
        RETURN(0);
    }
}
static void dsp_GetDisplayID(ppc_cpu *c)
{
    if (R(1))
        wr32(R(1), 1);
    RETURN(0);
}

/* DSpContext_Reserve(context, desired): the size and depth asked for
 * become the back buffer's. */
static void dsp_Reserve(ppc_cpu *c)
{
    if (!is_context(R(0))) {
        RETURN(kDSpInvalidContextErr);
        return;
    }
    gaddr want = R(1);
    uint32_t w = want ? rd32(want + 4) : 0, h = want ? rd32(want + 8) : 0;
    if (!w || w > (uint32_t)g_screen_width)
        w = (uint32_t)g_screen_width;
    if (!h || h > (uint32_t)g_screen_height)
        h = (uint32_t)g_screen_height;
    int depth = want ? depth_from_mask(rd32(want + 32), 0) : 0;
    if (!depth && want)
        depth = (int)rd32(want + 40);
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16 && depth != 32)
        depth = 8;
    if (want)
        ppc_copy(ctx, want, ATTR_SIZE);
    wr32(ctx + 4, w);
    wr32(ctx + 8, h);
    wr32(ctx + 40, (uint32_t)depth);
    wr32(ctx + 44, (uint32_t)depth);
    wr32(ctx + 32, (uint32_t)depth);
    wr32(ctx + 36, (uint32_t)depth);
    if (!rd32(ctx + 48))
        wr32(ctx + 48, 1);
    wr8(ctx + CTX_RESERVED, 1);
    wr16(ctx + CTX_STATE, kDSpContextState_Inactive);
    display_want_screen("it uses DrawSprocket");
    /* As DrawSprocket set the display to the context's depth (and size,
     * which ours keeps): games write to the screen's memory themselves,
     * from its top left (Star Wars DroidWorks), so the display is there. */
    if (!depth_before)
        depth_before = qd_screen_depth();
    qd_screen_set_depth(depth);
    wr16(ctx + CTX_LEFT, 0);
    wr16(ctx + CTX_TOP, 0);

    /* The back buffer: a GWorld of that size and depth. */
    gaddr r = tb_NewPtr(8, true, true), gwp = tb_NewPtr(4, true, true);
    wr16(r + 4, (uint16_t)h);
    wr16(r + 6, (uint16_t)w);
    gaddr ctab = want && depth <= 8 ? rd32(want + 24) : 0;
    int16_t err = tb_NewGWorld(gwp, (int16_t)depth, r, ctab, 0, 0);
    wr32(ctx + CTX_GWORLD, err ? 0 : rd32(gwp));
    tb_DisposePtr(r);
    tb_DisposePtr(gwp);
    RETURN(err);
}

static void black_window(void)
{
    gaddr r = tb_NewPtr(16, true, true);
    wr16(r + 4, (uint16_t)g_screen_height);
    wr16(r + 6, (uint16_t)g_screen_width);
    gaddr title = r + 8, black = r + 10;
    gaddr w = tb_NewCWindow(0, r, title, true, 2 /* plainDBox */, 0xFFFFFFFFu, false, 0);
    if (w) {
        gaddr saved = tb_NewPtr(4, false, true);
        tb_GetPort(saved);
        tb_SetPort(w);
        tb_RGBForeColor(black);
        tb_PaintRect(r);
        tb_SetPort(rd32(saved));
        tb_DisposePtr(saved);
    }
    tb_DisposePtr(r);
    wr32(ctx + CTX_WINDOW, w);
}

/* DSpContext_SetState(context, state): active puts the black window up,
 * inactive takes it away. */
static void dsp_SetState(ppc_cpu *c)
{
    if (!is_context(R(0)) || !rd8(ctx + CTX_RESERVED)) {
        RETURN(is_context(R(0)) ? kDSpContextNotReservedErr : kDSpInvalidContextErr);
        return;
    }
    int state = (int)(R(1) & 0xFFFF);
    if (state == kDSpContextState_Active && !rd32(ctx + CTX_WINDOW)) {
        wm_hide_menu_bar(true);
        black_window();
    }
    if (state == kDSpContextState_Inactive && rd32(ctx + CTX_WINDOW)) {
        tb_DisposeWindow(rd32(ctx + CTX_WINDOW));
        wr32(ctx + CTX_WINDOW, 0);
        wm_hide_menu_bar(false);
    }
    wr16(ctx + CTX_STATE, (uint16_t)state);
    RETURN(0);
}
static void dsp_GetState(ppc_cpu *c)
{
    if (R(1))
        wr16(R(1), rd16(ctx + CTX_STATE));
    RETURN(0);
}
static void dsp_Release(ppc_cpu *c)
{
    if (is_context(R(0))) {
        if (depth_before)
            qd_screen_set_depth(depth_before);
        depth_before = 0;
        if (rd32(ctx + CTX_WINDOW)) {
            tb_DisposeWindow(rd32(ctx + CTX_WINDOW));
            wm_hide_menu_bar(false);
        }
        if (rd32(ctx + CTX_GWORLD))
            tb_DisposeGWorld(rd32(ctx + CTX_GWORLD));
        wr32(ctx + CTX_WINDOW, 0);
        wr32(ctx + CTX_GWORLD, 0);
        wr8(ctx + CTX_RESERVED, 0);
    }
    RETURN(0);
}

/* One page (pageCount 1): no back buffer to swap, the program draws on
 * the display itself (Star Wars DroidWorks blits each frame into it). */
static bool single_buffered(void) { return rd32(ctx + 48) <= 1 && rd32(ctx + CTX_WINDOW); }

static void dsp_GetBackBuffer(ppc_cpu *c)
{
    if (R(2))
        wr32(R(2), !is_context(R(0)) ? 0 : single_buffered() ? rd32(ctx + CTX_WINDOW) : rd32(ctx + CTX_GWORLD));
    RETURN(is_context(R(0)) ? 0 : kDSpInvalidContextErr);
}
static void dsp_GetFrontBuffer(ppc_cpu *c)
{
    if (R(1))
        wr32(R(1), is_context(R(0)) ? rd32(ctx + CTX_WINDOW) : 0);
    RETURN(is_context(R(0)) ? 0 : kDSpInvalidContextErr);
}

/* Swap: the back buffer to its place in the window. */
static void swap(void)
{
    gaddr gw = rd32(ctx + CTX_GWORLD), w = rd32(ctx + CTX_WINDOW);
    if (!gw || !w)
        return;
    gaddr r = tb_NewPtr(32, true, true), src = r, dst = r + 8, black = r + 16, white = r + 22;
    uint16_t width = (uint16_t)rd32(ctx + 4), height = (uint16_t)rd32(ctx + 8);
    wr16(src + 4, height);
    wr16(src + 6, width);
    int16_t left = (int16_t)rd16(ctx + CTX_LEFT), top = (int16_t)rd16(ctx + CTX_TOP);
    wr16(dst, (uint16_t)top);
    wr16(dst + 2, (uint16_t)left);
    wr16(dst + 4, (uint16_t)(top + height));
    wr16(dst + 6, (uint16_t)(left + width));
    wr16(white, 0xFFFF);
    wr16(white + 2, 0xFFFF);
    wr16(white + 4, 0xFFFF);
    gaddr saved_port = tb_NewPtr(8, false, true);
    tb_GetGWorld(saved_port, saved_port + 4);
    tb_SetPort(w);
    tb_RGBForeColor(black);
    tb_RGBBackColor(white);
    tb_CopyBits(gw + CGrafPort_portPixMap_off, w + CGrafPort_portPixMap_off, src, dst, 0 /* srcCopy */, 0);
    tb_SetGWorld(rd32(saved_port), rd32(saved_port + 4));
    tb_DisposePtr(saved_port);
    tb_DisposePtr(r);
}
/* Alternate buffers: GWorlds the context's size and depth, with its
 * colours.  The underlay is the one the back buffer is restored from: the
 * rectangles invalidated since the last swap, after each swap. */
enum { ALT_MAGIC = 0x616C7462 /* 'altb' */, ALT_GWORLD = 4, ALT_SIZE = 8 };
static gaddr underlay;
static qrect dirty;

static bool is_alt(gaddr a) { return a && rd32(a) == ALT_MAGIC; }

static void copy_rect(gaddr from, gaddr to, qrect q)
{
    gaddr r = tb_NewPtr(8, true, true);
    qrect_put(r, q);
    gaddr saved = tb_NewPtr(8, false, true);
    tb_GetGWorld(saved, saved + 4);
    tb_SetGWorld(to, 0);
    tb_CopyBits(from + CGrafPort_portPixMap_off, to + CGrafPort_portPixMap_off, r, r, 0 /* srcCopy */, 0);
    tb_SetGWorld(rd32(saved), rd32(saved + 4));
    tb_DisposePtr(saved);
    tb_DisposePtr(r);
}

static void dsp_SwapBuffers(ppc_cpu *c)
{
    if (is_context(R(0))) {
        if (!single_buffered())
            swap();
        gaddr back = single_buffered() ? rd32(ctx + CTX_WINDOW) : rd32(ctx + CTX_GWORLD);
        if (is_alt(underlay) && back && dirty.bottom > dirty.top && dirty.right > dirty.left)
            copy_rect(rd32(underlay + ALT_GWORLD), back, dirty);
        dirty = (qrect){ 0, 0, 0, 0 };
    }
    RETURN(0);
}

/* DSpContext_InvalBackBufferRect(context, rect) */
static void dsp_InvalBackBufferRect(ppc_cpu *c)
{
    if (!is_context(R(0)))
        RETURN(kDSpInvalidContextErr);
    else {
        qrect q = qrect_get(R(1));
        if (dirty.bottom <= dirty.top || dirty.right <= dirty.left)
            dirty = q;
        else
            dirty = (qrect){ q.top < dirty.top ? q.top : dirty.top, q.left < dirty.left ? q.left : dirty.left,
                             q.bottom > dirty.bottom ? q.bottom : dirty.bottom,
                             q.right > dirty.right ? q.right : dirty.right };
        RETURN(0);
    }
}

/* DSpAltBuffer_New(context, inVRAM, attributes, &altBuffer) */
static void dsp_AltBufferNew(ppc_cpu *c)
{
    gaddr back = is_context(R(0)) ? rd32(ctx + CTX_GWORLD) : 0;
    if (!back || !R(3))
        RETURN(is_context(R(0)) ? kDSpContextNotReservedErr : kDSpInvalidContextErr);
    else {
        gaddr pmh = rd32(back + CGrafPort_portPixMap_off);
        gaddr pm = rd32(pmh);
        int depth = (int16_t)rd16(pm + PixMap_pixelSize_off);
        gaddr ctab = depth <= 8 ? rd32(pm + PixMap_pmTable_off) : 0;
        gaddr r = tb_NewPtr(8, true, true), gwp = tb_NewPtr(4, true, true);
        wr16(r + 4, (uint16_t)rd32(ctx + 8));
        wr16(r + 6, (uint16_t)rd32(ctx + 4));
        int16_t err = tb_NewGWorld(gwp, (int16_t)depth, r, ctab, 0, 0);
        gaddr alt = err ? 0 : tb_NewPtr(ALT_SIZE, true, true);
        if (alt) {
            wr32(alt, ALT_MAGIC);
            wr32(alt + ALT_GWORLD, rd32(gwp));
        }
        wr32(R(3), alt);
        tb_DisposePtr(r);
        tb_DisposePtr(gwp);
        RETURN(err ? err : alt ? 0 : memFullErr);
    }
}

static void dsp_AltBufferDispose(ppc_cpu *c)
{
    gaddr alt = R(0);
    if (!is_alt(alt))
        RETURN(paramErr);
    else {
        if (underlay == alt)
            underlay = 0;
        tb_DisposeGWorld(rd32(alt + ALT_GWORLD));
        wr32(alt, 0);
        tb_DisposePtr(alt);
        RETURN(0);
    }
}

/* DSpAltBuffer_GetCGrafPtr(altBuffer, bufferKind, &port, &device) */
static void dsp_AltBufferGetCGrafPtr(ppc_cpu *c)
{
    gaddr alt = R(0);
    if (!is_alt(alt))
        RETURN(paramErr);
    else {
        gaddr gw = rd32(alt + ALT_GWORLD);
        if (R(2))
            wr32(R(2), gw);
        if (R(3))
            wr32(R(3), tb_GetGWorldDevice(gw));
        RETURN(0);
    }
}

static void dsp_SetUnderlayAltBuffer(ppc_cpu *c)
{
    underlay = is_alt(R(1)) ? R(1) : 0;
    RETURN(is_context(R(0)) ? 0 : kDSpInvalidContextErr);
}

static void dsp_GetUnderlayAltBuffer(ppc_cpu *c)
{
    if (R(1))
        wr32(R(1), underlay);
    RETURN(is_context(R(0)) ? 0 : kDSpInvalidContextErr);
}

/* DSpContext_SetCLUTEntries(context, entries, start, count) */
static void dsp_SetCLUTEntries(ppc_cpu *c)
{
    gaddr entries = R(1);
    int start = (int16_t)R(2), count = (int16_t)R(3);
    gaddr gw = is_context(R(0)) ? rd32(ctx + CTX_GWORLD) : 0;
    gaddr pmh = gw ? rd32(gw + CGrafPort_portPixMap_off) : 0;
    gaddr cth = pmh && rd32(pmh) ? rd32(rd32(pmh) + PixMap_pmTable_off) : 0;
    if (cth && rd32(cth) && entries) {
        gaddr ct = rd32(cth);
        int size = (int16_t)rd16(ct + ColorTable_ctSize_off);
        for (int i = 0; i < count && start + i <= size; i++) {
            gaddr to = ct + ColorTable_ctTable_off + (gaddr)(start + i) * ColorSpec_SIZEOF;
            ppc_copy(to + 2, entries + (gaddr)i * ColorSpec_SIZEOF + 2, 6);
            wr16(to, (uint16_t)(start + i));
        }
        wr32(ct, rd32(ct) + 1); /* ctSeed: a new table */
    }
    if (screen_depth() <= 8 && entries && count > 0)
        tb_SetEntries((int16_t)start, (int16_t)(count - 1), entries);
    RETURN(0);
}
static void dsp_GetCLUTEntries(ppc_cpu *c)
{
    gaddr entries = R(1);
    int start = (int16_t)R(2), count = (int16_t)R(3);
    gaddr gw = is_context(R(0)) ? rd32(ctx + CTX_GWORLD) : 0;
    gaddr pmh = gw ? rd32(gw + CGrafPort_portPixMap_off) : 0;
    gaddr cth = pmh && rd32(pmh) ? rd32(rd32(pmh) + PixMap_pmTable_off) : 0;
    if (cth && rd32(cth) && entries)
        for (int i = 0; i < count; i++)
            ppc_copy(entries + (gaddr)i * ColorSpec_SIZEOF,
                     rd32(cth) + ColorTable_ctTable_off + (gaddr)(start + i) * ColorSpec_SIZEOF, ColorSpec_SIZEOF);
    RETURN(0);
}

static void dsp_noErr(ppc_cpu *c) { RETURN(0); }
static void dsp_false(ppc_cpu *c)
{
    if (R(1))
        wr8(R(1), 0);
    RETURN(0);
}
static void dsp_GetMonitorFrequency(ppc_cpu *c)
{
    if (R(1))
        wr32(R(1), 60u << 16);
    RETURN(0);
}
/* DSpGetMouse(*globalPoint) */
static void dsp_GetMouse(ppc_cpu *c)
{
    if (R(0)) {
        tb_GetMouse(R(0));
        tb_LocalToGlobal(R(0));
    }
    RETURN(0);
}
/* DSpFindContextFromPoint(globalPoint, *context): ours for a point on the
 * screen, the one display (µWar finds the context the mouse is over). */
static void dsp_FindContextFromPoint(ppc_cpu *c)
{
    int v = (int16_t)(R(0) >> 16), h = (int16_t)R(0);
    bool on = v >= 0 && h >= 0 && v < g_screen_height && h < g_screen_width;
    if (on)
        context_make();
    if (R(1))
        wr32(R(1), on ? ctx : 0);
    RETURN(on ? 0 : kDSpContextNotFoundErr);
}
/* DSpProcessEvent(event, *processed): nothing of ours in events. */
static void dsp_ProcessEvent(ppc_cpu *c)
{
    if (R(1))
        wr8(R(1), 0);
    RETURN(0);
}
/* Global and local: the display's corner in the window. */
static void dsp_GlobalToLocal(ppc_cpu *c)
{
    if (R(1)) {
        wr16(R(1), (uint16_t)((int16_t)rd16(R(1)) - (int16_t)rd16(ctx + CTX_TOP)));
        wr16(R(1) + 2, (uint16_t)((int16_t)rd16(R(1) + 2) - (int16_t)rd16(ctx + CTX_LEFT)));
    }
    RETURN(0);
}
static void dsp_LocalToGlobal(ppc_cpu *c)
{
    if (R(1)) {
        wr16(R(1), (uint16_t)((int16_t)rd16(R(1)) + (int16_t)rd16(ctx + CTX_TOP)));
        wr16(R(1) + 2, (uint16_t)((int16_t)rd16(R(1) + 2) + (int16_t)rd16(ctx + CTX_LEFT)));
    }
    RETURN(0);
}
static void dsp_GetMaxFrameRate(ppc_cpu *c)
{
    if (R(1))
        wr32(R(1), 0);
    RETURN(0);
}
/* DSpUserSelectContext(desired, displayID, eventProc, *context): no
 * dialog to choose a display from, the one there is. */
static void dsp_UserSelectContext(ppc_cpu *c)
{
    context_make();
    if (R(3))
        wr32(R(3), ctx);
    RETURN(0);
}

/* The rest, for one context with nothing to save, queue or blit. */
static void dsp_GetFlattenedSize(ppc_cpu *c)
{
    if (R(1))
        wr32(R(1), ATTR_SIZE);
    RETURN(0);
}
static void dsp_Flatten(ppc_cpu *c)
{
    if (R(1))
        ppc_copy(R(1), ctx, ATTR_SIZE);
    RETURN(0);
}
static void dsp_Restore(ppc_cpu *c)
{
    context_make();
    if (R(1))
        wr32(R(1), ctx);
    RETURN(0);
}
static void dsp_GetDirtyRectGridSize(ppc_cpu *c)
{
    if (R(1))
        wr32(R(1), 16);
    if (R(2))
        wr32(R(2), 16);
    RETURN(0);
}
static void dsp_unsupported(ppc_cpu *c) { RETURN(-30450); } /* kDSpFeatureNotImplementedErr */

/* DSpBlit_Fastest and DSpBlit_Faster(info, asyncOK): DSpBlitInfo is
 * completionFlag 0, completionProc 4, srcContext 8, srcBuffer 12,
 * srcRect 16, srcKey 24, dstContext 28, dstBuffer 32, dstRect 36,
 * dstKey 44, mode 48 (1: srcKey, 2: dstKey, 4: interpolate).  Done at once:
 * pixel by pixel between buffers of one depth and size, keys honoured;
 * otherwise by CopyBits.  The completion flag is set, the routine not
 * called (asynchronous blits are never left running). */
enum { kDSpBlitMode_SrcKey = 1, kDSpBlitMode_DstKey = 2 };

static bool buffer_pixels(gaddr port, gaddr *base, uint32_t *row_bytes, int *depth, qrect *bounds)
{
    if (!port || !qd_port_is_color(port))
        return false;
    gaddr h = rd32(port + CGrafPort_portPixMap_off);
    gaddr pm = h ? rd32(h) : 0;
    if (!pm)
        return false;
    *base = PixMap_baseAddr(pm);
    *row_bytes = (uint32_t)(PixMap_rowBytes(pm) & 0x3FFF);
    *depth = PixMap_pixelSize(pm);
    *bounds = qrect_get(pm + PixMap_bounds_off);
    return *base != 0;
}

static uint32_t get_pixel(gaddr row, int x, int depth)
{
    switch (depth) {
    case 8: return rd8(row + (uint32_t)x);
    case 16: return rd16(row + (uint32_t)x * 2);
    case 32: return rd32(row + (uint32_t)x * 4);
    default: {
        uint32_t bit = (uint32_t)x * (uint32_t)depth;
        return (rd8(row + bit / 8) >> (8 - depth - bit % 8)) & ((1u << depth) - 1);
    }
    }
}

static void put_pixel(gaddr row, int x, int depth, uint32_t v)
{
    switch (depth) {
    case 8: wr8(row + (uint32_t)x, (uint8_t)v); break;
    case 16: wr16(row + (uint32_t)x * 2, (uint16_t)v); break;
    case 32: wr32(row + (uint32_t)x * 4, v); break;
    default: {
        uint32_t bit = (uint32_t)x * (uint32_t)depth, sh = 8 - (uint32_t)depth - bit % 8;
        uint8_t m = (uint8_t)(((1u << depth) - 1) << sh);
        wr8(row + bit / 8, (uint8_t)((rd8(row + bit / 8) & ~m) | ((v << sh) & m)));
    }
    }
}

static void dsp_Blit(ppc_cpu *c)
{
    gaddr info = R(0);
    if (!info)
        RETURN(paramErr);
    else {
        gaddr src = rd32(info + 12), dst = rd32(info + 32);
        qrect sr = qrect_get(info + 16), dr = qrect_get(info + 36);
        uint32_t mode = rd32(info + 48), skey = rd32(info + 24), dkey = rd32(info + 44);
        gaddr sb, db;
        uint32_t srb, drb;
        int sd, dd;
        qrect sbn, dbn;
        int w = sr.right - sr.left, h = sr.bottom - sr.top;
        if (buffer_pixels(src, &sb, &srb, &sd, &sbn) && buffer_pixels(dst, &db, &drb, &dd, &dbn) && sd == dd &&
            w == dr.right - dr.left && h == dr.bottom - dr.top) {
            for (int y = 0; y < h; y++) {
                int sy = sr.top + y - sbn.top, dy = dr.top + y - dbn.top;
                if (sy < 0 || dy < 0 || sy >= sbn.bottom - sbn.top || dy >= dbn.bottom - dbn.top)
                    continue;
                gaddr srow = sb + (uint32_t)sy * srb, drow = db + (uint32_t)dy * drb;
                for (int x = 0; x < w; x++) {
                    int sx = sr.left + x - sbn.left, dx = dr.left + x - dbn.left;
                    if (sx < 0 || dx < 0 || sx >= sbn.right - sbn.left || dx >= dbn.right - dbn.left)
                        continue;
                    uint32_t v = get_pixel(srow, sx, sd);
                    if ((mode & kDSpBlitMode_SrcKey) && v == skey)
                        continue;
                    if ((mode & kDSpBlitMode_DstKey) && get_pixel(drow, dx, dd) != dkey)
                        continue;
                    put_pixel(drow, dx, dd, v);
                }
            }
        } else if (src && dst) {
            gaddr r = tb_NewPtr(16, true, true);
            qrect_put(r, sr);
            qrect_put(r + 8, dr);
            gaddr saved = tb_NewPtr(8, false, true);
            tb_GetGWorld(saved, saved + 4);
            tb_SetGWorld(dst, 0);
            tb_CopyBits(src + CGrafPort_portPixMap_off, dst + CGrafPort_portPixMap_off, r, r + 8, 0, 0);
            tb_SetGWorld(rd32(saved), rd32(saved + 4));
            tb_DisposePtr(saved);
            tb_DisposePtr(r);
        }
        wr8(info, 1); /* completionFlag */
        RETURN(0);
    }
}

const ppc_export dsp_exports[] = {
    { "DSpAltBuffer_Dispose", dsp_AltBufferDispose },
    { "DSpAltBuffer_GetCGrafPtr", dsp_AltBufferGetCGrafPtr },
    { "DSpAltBuffer_InvalRect", dsp_noErr },
    { "DSpAltBuffer_New", dsp_AltBufferNew },
    { "DSpBlit_Faster", dsp_Blit },
    { "DSpBlit_Fastest", dsp_Blit },
    { "DSpContext_Dispose", dsp_noErr },
    { "DSpContext_Flatten", dsp_Flatten },
    { "DSpContext_GetDirtyRectGridSize", dsp_GetDirtyRectGridSize },
    { "DSpContext_GetDirtyRectGridUnits", dsp_GetDirtyRectGridSize },
    { "DSpContext_GetFlattenedSize", dsp_GetFlattenedSize },
    { "DSpContext_GetUnderlayAltBuffer", dsp_GetUnderlayAltBuffer },
    { "DSpContext_Queue", dsp_noErr },
    { "DSpContext_Restore", dsp_Restore },
    { "DSpContext_SetDirtyRectGridSize", dsp_noErr },
    { "DSpContext_SetUnderlayAltBuffer", dsp_SetUnderlayAltBuffer },
    { "DSpContext_Switch", dsp_noErr },
    { "DSpCanUserSelectContext", dsp_false },
    { "DSpContext_CanUserSelectContext", dsp_false },
    { "DSpContext_FadeGamma", dsp_noErr },
    { "DSpContext_FadeGammaIn", dsp_noErr },
    { "DSpContext_FadeGammaOut", dsp_noErr },
    { "DSpContext_GetAttributes", dsp_GetAttributes },
    { "DSpContext_GetBackBuffer", dsp_GetBackBuffer },
    { "DSpContext_GetCLUTEntries", dsp_GetCLUTEntries },
    { "DSpContext_GetDisplayID", dsp_GetDisplayID },
    { "DSpContext_GetFrontBuffer", dsp_GetFrontBuffer },
    { "DSpContext_GetMaxFrameRate", dsp_GetMaxFrameRate },
    { "DSpContext_GetMonitorFrequency", dsp_GetMonitorFrequency },
    { "DSpContext_GetState", dsp_GetState },
    { "DSpContext_GlobalToLocal", dsp_GlobalToLocal },
    { "DSpContext_InvalBackBufferRect", dsp_InvalBackBufferRect },
    { "DSpContext_IsBusy", dsp_false },
    { "DSpContext_LocalToGlobal", dsp_LocalToGlobal },
    { "DSpContext_Release", dsp_Release },
    { "DSpContext_Reserve", dsp_Reserve },
    { "DSpContext_SetCLUTEntries", dsp_SetCLUTEntries },
    { "DSpContext_SetMaxFrameRate", dsp_noErr },
    { "DSpContext_SetState", dsp_SetState },
    { "DSpContext_SetVBLProc", dsp_noErr },
    { "DSpContext_SwapBuffers", dsp_SwapBuffers },
    { "DSpFindBestContext", dsp_FindBestContext },
    { "DSpFindBestContextOnDisplayID", dsp_FindBestContextOnDisplayID },
    { "DSpFindContextFromPoint", dsp_FindContextFromPoint },
    { "DSpGetFirstContext", dsp_GetFirstContext },
    { "DSpGetMouse", dsp_GetMouse },
    { "DSpGetNextContext", dsp_GetNextContext },
    { "DSpGetVersion", dsp_GetVersion },
    { "DSpProcessEvent", dsp_ProcessEvent },
    { "DSpSetBlankingColor", dsp_noErr },
    { "DSpSetDebugMode", dsp_noErr },
    { "DSpShutdown", dsp_Shutdown },
    { "DSpStartup", dsp_Startup },
    { "DSpUserSelectContext", dsp_UserSelectContext },
};
const unsigned dsp_export_count = sizeof dsp_exports / sizeof dsp_exports[0];
