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
    wr16(ctx + CTX_LEFT, (uint16_t)((g_screen_width - (int)w) / 2));
    wr16(ctx + CTX_TOP, (uint16_t)((g_screen_height - (int)h) / 2));

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

static void dsp_GetBackBuffer(ppc_cpu *c)
{
    if (R(2))
        wr32(R(2), is_context(R(0)) ? rd32(ctx + CTX_GWORLD) : 0);
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
static void dsp_SwapBuffers(ppc_cpu *c)
{
    if (is_context(R(0)))
        swap();
    RETURN(0);
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
static void dsp_UserSelectContext(ppc_cpu *c) { RETURN(kDSpContextNotFoundErr); }

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

const ppc_export dsp_exports[] = {
    { "DSpAltBuffer_Dispose", dsp_noErr },
    { "DSpAltBuffer_GetCGrafPtr", dsp_unsupported },
    { "DSpAltBuffer_InvalRect", dsp_noErr },
    { "DSpAltBuffer_New", dsp_unsupported },
    { "DSpBlit_Faster", dsp_unsupported },
    { "DSpBlit_Fastest", dsp_unsupported },
    { "DSpContext_Dispose", dsp_noErr },
    { "DSpContext_Flatten", dsp_Flatten },
    { "DSpContext_GetDirtyRectGridSize", dsp_GetDirtyRectGridSize },
    { "DSpContext_GetDirtyRectGridUnits", dsp_GetDirtyRectGridSize },
    { "DSpContext_GetFlattenedSize", dsp_GetFlattenedSize },
    { "DSpContext_GetUnderlayAltBuffer", dsp_unsupported },
    { "DSpContext_Queue", dsp_noErr },
    { "DSpContext_Restore", dsp_Restore },
    { "DSpContext_SetDirtyRectGridSize", dsp_noErr },
    { "DSpContext_SetUnderlayAltBuffer", dsp_noErr },
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
    { "DSpContext_InvalBackBufferRect", dsp_noErr },
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
