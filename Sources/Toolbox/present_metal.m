/*
 * present.h on Metal (macOS): the classic screen decoded, scaled and shown
 * by the GPU.
 *
 * One copy of the screen serves every surface, brought up to date at each
 * refresh from the rows written since the last (dirty.c: no comparison of
 * the screen; an unchanged frame costs nothing).  How it reaches the GPU
 * (present_path):
 *   zero-copy  Apple silicon: the screen's own pages of guest memory
 *              wrapped as a buffer (newBufferWithBytesNoCopy, rounded out
 *              to whole pages; a compact Mac's screen, not page-aligned,
 *              at an offset into its first page), and the rows written
 *              copied by the GPU into a private buffer, in a command
 *              buffer of their own committed first: the processor copies
 *              nothing, and a program waiting for the refresh goes on
 *              once that copy is done, not after the drawing.
 *   shared     the rows written compared and copied by the processor into
 *              a buffer the GPU shares: when the pages can't be wrapped,
 *              or BLUEBOX_PRESENT_PATH=shared.
 *   managed    as shared, the changed rows then uploaded: a GPU with
 *              memory of its own (an Intel Mac's).
 * Once a second while the emulator runs, everything is looked at (shared
 * and managed compare it all and count the rows a writer changed unseen;
 * zero-copy copies and draws it all): the safety net under the tracking.
 * BLUEBOX_DIRTY=0 compares the screen with the copy instead (zero-copy
 * then copies it whole).
 * The fragment shader decodes the pixels from the buffer itself, at every
 * depth (1, 2, 4 and 8 bits through the video hardware's colours, 16 and
 * 32 bits direct), then the gamma and fades and After Dark's brightness,
 * with integers, so that at 1x it shows exactly what machine_screen_rgba
 * makes.  Scales that are whole numbers of the layer's pixels sample the
 * nearest pixel; others (1.5x on a display of 1x, a full screen stretched)
 * a sharp bilinear: each pixel stays a flat square but for a band of one
 * output pixel at its edges.  A window's shape and the program's desktop
 * (left out while it is in the background) are textures, made again only
 * when they change.
 *
 * Refreshes come from the host's display link (present_vsync) and are
 * handled on a thread of the presenter's own: the copy brought up to date,
 * and the surfaces whose rows changed drawn in one command buffer.  Frame
 * pacing goes by these refreshes (pace.c): a program that finished a
 * frame waits for the refresh, and is let go when the refresh has its
 * copy of the screen (the processor's taken, or the GPU's done).  A
 * program with no frames of its own is shown as it is at each refresh,
 * as a Mac's video showed it.
 *
 * The wrapped pages must stay mapped while the GPU copies from them: mem.c
 * and hibernate.c call present_memory_changing before mapping any of the
 * arena again, and it waits for the GPU's last copy and lets go of the
 * buffer first.
 */
#ifdef __APPLE__
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "machine.h"
#include "mem.h"
#include "present.h"
#include "present_backend.h"

#if !__has_feature(objc_arc)
#error "present_metal.m needs ARC"
#endif

static const char *shader_source =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct U {\n"
    "  float2 origin; float2 size; float2 dst;\n"
    "  uint base_off; uint row_bytes;\n"
    "  int width; int height; int depth; int brightness; int lut_on; int sharp;\n"
    "  int has_shape; int shape_x; int shape_y; int shape_w; int shape_h; int has_desk;\n"
    "  int cx0; int cy0; int cx1; int cy1;\n"
    "};\n"
    "struct V { float4 pos [[position]]; };\n"
    "vertex V vs(uint i [[vertex_id]]) {\n"
    "  V o; float2 p = float2((i << 1) & 2, i & 2);\n"
    "  o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); return o;\n"
    "}\n"
    /* A pixel as machine_screen_rgba makes it: white off the screen. */
    "static uint3 px(const device uchar *s, constant U &u, constant uint *clut, constant uchar *lut, int x, int y) {\n"
    "  uint3 c;\n"
    "  if (x < 0 || y < 0 || x >= u.width || y >= u.height) c = uint3(255);\n"
    "  else {\n"
    "    uint row = u.base_off + uint(y) * u.row_bytes;\n"
    "    if (u.depth == 32) { uint o = row + uint(x) * 4; c = uint3(s[o + 1], s[o + 2], s[o + 3]); }\n"
    "    else if (u.depth == 16) {\n"
    "      uint o = row + uint(x) * 2; uint v = (uint(s[o]) << 8) | uint(s[o + 1]);\n"
    "      uint3 f = uint3((v >> 10) & 31, (v >> 5) & 31, v & 31); c = (f << 3) | (f >> 2);\n"
    "    } else {\n"
    "      uint v;\n"
    "      if (u.depth == 8) v = s[row + uint(x)];\n"
    "      else { uint bit = uint(x) * uint(u.depth);\n"
    "             v = (uint(s[row + (bit >> 3)]) >> (8 - uint(u.depth) - (bit & 7))) & ((1u << u.depth) - 1); }\n"
    "      if (u.depth == 1) c = v ? uint3(0) : uint3(255);\n"
    "      else { uint e = clut[v]; c = uint3(e & 255, (e >> 8) & 255, (e >> 16) & 255); }\n"
    "    }\n"
    "  }\n"
    "  if (u.lut_on) c = uint3(lut[c.r], lut[256 + c.g], lut[512 + c.b]);\n"
    "  if (u.brightness != 255) c = c * uint(u.brightness) / 255;\n"
    "  return c;\n"
    "}\n"
    "static float3 pf(const device uchar *s, constant U &u, constant uint *clut, constant uchar *lut, int2 p) {\n"
    "  p = clamp(p, int2(u.cx0, u.cy0), int2(u.cx1, u.cy1));\n"
    "  return float3(px(s, u, clut, lut, p.x, p.y));\n"
    "}\n"
    "fragment float4 fs(V in [[stage_in]], constant U &u [[buffer(0)]], const device uchar *s [[buffer(1)]],\n"
    "                   constant uint *clut [[buffer(2)]], constant uchar *lut [[buffer(3)]],\n"
    "                   texture2d<uint> shape [[texture(0)]], texture2d<uint> desk [[texture(1)]]) {\n"
    "  float2 t = u.origin + in.pos.xy * (u.size / u.dst);\n"
    "  float2 fl = floor(t);\n"
    "  int2 p = int2(fl);\n"
    "  float3 c;\n"
    "  if (!u.sharp) c = float3(px(s, u, clut, lut, p.x, p.y));\n"
    "  else {\n"
    "    float2 scale = u.dst / u.size, f = t - fl - 0.5, r = 0.5 - 0.5 / scale;\n"
    "    float2 m = fl + (f - clamp(f, -r, r)) * scale;\n"
    "    float2 b = floor(m), w = m - b; int2 q = int2(b);\n"
    "    c = mix(mix(pf(s, u, clut, lut, q), pf(s, u, clut, lut, q + int2(1, 0)), w.x),\n"
    "            mix(pf(s, u, clut, lut, q + int2(0, 1)), pf(s, u, clut, lut, q + int2(1, 1)), w.x), w.y);\n"
    "  }\n"
    "  float a = 1.0;\n"
    "  if (u.has_shape) {\n"
    "    int2 k = p - int2(u.shape_x, u.shape_y);\n"
    "    if (k.x >= 0 && k.y >= 0 && k.x < u.shape_w && k.y < u.shape_h && shape.read(uint2(k)).r == 0) a = 0.0;\n"
    "  }\n"
    "  if (u.has_desk && p.x >= 0 && p.y >= 0 && p.x < u.width && p.y < u.height && desk.read(uint2(p)).r == 0) a = 0.0;\n"
    "  return float4(c / 255.0 * a, a);\n"
    "}\n";

typedef struct {
    float origin[2], size[2], dst[2];
    uint32_t base_off, row_bytes;
    int32_t width, height, depth, brightness, lut_on, sharp;
    int32_t has_shape, shape_x, shape_y, shape_w, shape_h, has_desk;
    int32_t cx0, cy0, cx1, cy1;
} uniforms;

enum { PATH_ZERO_COPY, PATH_SHARED, PATH_MANAGED };
static const char *const path_names[] = { "zero-copy", "shared", "managed" };
/* Everything looked at (compared, or drawn again on zero-copy) once in
 * this many refreshes when anything ran: the safety net under the
 * tracking of writes, or the comparison without it. */
enum { MAX_BANDS = 64, FULL_EVERY = 60, SCAN_AT_LEAST = 30 };

@interface PresentSurface : NSObject {
@public
    CAMetalLayer *layer;
    double x, y, w, h;
    int pw, ph;
    uint8_t *shape;
    int sw, sh;
    bool shape_dirty, desktop, visible, needs;
    id<MTLTexture> shape_tex;
}
@end
@implementation PresentSurface
- (void)dealloc
{
    bb_host_free(shape);
}
@end

struct present_surface; /* the PresentSurface, bridged */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t vlock = PTHREAD_MUTEX_INITIALIZER; /* the refreshes asked for */
/* The surfaces and what each shows: the host's thread sets them without
 * waiting for a comparison of the screen (the presenter's lock). */
static pthread_mutex_t slock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static int started = -1;
static id<MTLDevice> device;
static id<MTLCommandQueue> queue;
static id<MTLRenderPipelineState> pipe_layer;
static id<MTLTexture> dummy;
static NSMutableArray<PresentSurface *> *surfaces;
static int path = PATH_SHARED, best_path = PATH_SHARED;

/* The copy of the screen the surfaces are drawn from: its form, and its
 * rows (a GPU buffer: shared or managed, written by the CPU; private for
 * zero-copy, written by the GPU's own copy from the wrapped pages). */
static present_state shown_state;
static bool copy_valid;
static id<MTLBuffer> rows_buf;
/* The screen's pages wrapped, [nc_lo, nc_lo + nc_len) of the host. */
static id<MTLBuffer> nocopy;
static uintptr_t nc_lo;
static size_t nc_len;
static id<MTLTexture> desk_tex;
static bool desk_valid;
/* The last command buffers: the drawing, and zero-copy's copy (the only
 * one that reads guest memory). */
static id<MTLCommandBuffer> last_cmd, last_copy;
/* The rows written that this presenter has seen (dirty.c), the
 * emulator's progress at the last look at everything, and the refreshes
 * since. */
static uint64_t dirty_seen, progress_seen;
static int since_full;

static bool vsync_wanted, thread_running;
static uint64_t vsync_count;
static int vsync_every = 1;
static CVDisplayLinkRef link_ref;

/* ---- Setting up ---- */

static void memory_changing(void *lo, size_t len) { present_memory_changing(lo, len); }

bool present_start(void)
{
    if (started >= 0)
        return started;
    started = 0;
    const char *e = getenv("BLUEBOX_METAL");
    if (e && e[0] == '0')
        return false;
    @autoreleasepool {
        device = MTLCreateSystemDefaultDevice();
        if (!device)
            return false;
        queue = [device newCommandQueue];
        NSError *err = nil;
        id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:shader_source]
                                                  options:nil
                                                    error:&err];
        if (!lib || !queue) {
            fprintf(stderr, "bluebox: the screen's shader didn't compile: %s\n",
                    err ? err.localizedDescription.UTF8String : "no queue");
            return false;
        }
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [lib newFunctionWithName:@"vs"];
        d.fragmentFunction = [lib newFunctionWithName:@"fs"];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pipe_layer = [device newRenderPipelineStateWithDescriptor:d error:&err];
        if (!pipe_layer)
            return false;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Uint
                                                                                      width:1
                                                                                     height:1
                                                                                  mipmapped:NO];
        dummy = [device newTextureWithDescriptor:td];
        uint8_t one = 1;
        [dummy replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&one bytesPerRow:1];
        surfaces = [NSMutableArray new];
    }
    /* Zero-copy on unified memory (Apple silicon), unless asked not to. */
#if defined(__arm64__)
    best_path = device.hasUnifiedMemory ? PATH_ZERO_COPY : PATH_MANAGED;
#else
    best_path = device.hasUnifiedMemory ? PATH_SHARED : PATH_MANAGED;
#endif
    const char *p = getenv("BLUEBOX_PRESENT_PATH");
    if (p && !strcmp(p, "shared") && device.hasUnifiedMemory)
        best_path = PATH_SHARED;
    else if (p && !strcmp(p, "managed"))
        best_path = PATH_MANAGED;
    path = best_path;
    g_mem_remap_fn = memory_changing;
    started = 1;
    return true;
}

const char *present_path(void) { return started == 1 ? path_names[path] : "cpu"; }

/* With the lock held: the copy made again at the next refresh. */
static void wait_gpu(void)
{
    if (last_copy)
        [last_copy waitUntilCompleted];
    if (last_cmd)
        [last_cmd waitUntilCompleted];
}

static void drop_copy(void)
{
    wait_gpu();
    nocopy = nil;
    nc_lo = 0;
    nc_len = 0;
    rows_buf = nil;
    copy_valid = false;
}

bool present_force_path(const char *name)
{
    if (!present_start())
        return false;
    int want = !strcmp(name, "zero-copy") ? PATH_ZERO_COPY : !strcmp(name, "shared") ? PATH_SHARED
                                                          : !strcmp(name, "managed") ? PATH_MANAGED
                                                                                     : -1;
    if (want < 0 || (want != PATH_MANAGED && !device.hasUnifiedMemory))
        return false;
    pthread_mutex_lock(&lock);
    drop_copy();
    path = best_path = want;
    pthread_mutex_unlock(&lock);
    return true;
}

void present_memory_changing(void *lo, size_t len)
{
    if (started != 1)
        return;
    pthread_mutex_lock(&lock);
    if (!lo) {
        /* The screen's page moved: its old pages stay mapped, so the buffer
         * is simply made again for the new one. */
        copy_valid = false;
    } else if (nocopy && (uintptr_t)lo < nc_lo + nc_len && nc_lo < (uintptr_t)lo + len) {
        if (last_copy)
            [last_copy waitUntilCompleted]; /* no read of the old pages in flight */
        nocopy = nil;
        nc_lo = 0;
        nc_len = 0;
        copy_valid = false;
    }
    pthread_mutex_unlock(&lock);
}

/* ---- Surfaces ---- */

present_surface *present_surface_new(void *layer_ptr)
{
    if (!present_start() || !layer_ptr)
        return NULL;
    PresentSurface *s = [PresentSurface new];
    s->layer = (__bridge CAMetalLayer *)layer_ptr;
    s->layer.device = device;
    s->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    s->layer.framebufferOnly = YES;
    s->layer.maximumDrawableCount = 3;
    s->layer.magnificationFilter = kCAFilterNearest;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    s->layer.colorspace = cs;
    CGColorSpaceRelease(cs);
    s->visible = true;
    s->needs = true;
    pthread_mutex_lock(&slock);
    [surfaces addObject:s];
    pthread_mutex_unlock(&slock);
    return (__bridge_retained void *)s;
}

void present_surface_free(present_surface *ps)
{
    if (!ps)
        return;
    PresentSurface *s = (__bridge_transfer PresentSurface *)(void *)ps;
    pthread_mutex_lock(&slock);
    [surfaces removeObject:s];
    pthread_mutex_unlock(&slock);
}

static PresentSurface *surface(present_surface *ps) { return (__bridge PresentSurface *)(void *)ps; }

void present_surface_set(present_surface *ps, double x, double y, double w, double h, int pixels_w, int pixels_h)
{
    if (!ps)
        return;
    PresentSurface *s = surface(ps);
    if (pixels_w < 1)
        pixels_w = 1;
    if (pixels_h < 1)
        pixels_h = 1;
    pthread_mutex_lock(&slock);
    if (s->x != x || s->y != y || s->w != w || s->h != h || s->pw != pixels_w || s->ph != pixels_h) {
        s->x = x, s->y = y, s->w = w, s->h = h;
        if (s->pw != pixels_w || s->ph != pixels_h)
            s->layer.drawableSize = CGSizeMake(pixels_w, pixels_h);
        s->pw = pixels_w, s->ph = pixels_h;
        s->needs = true;
    }
    pthread_mutex_unlock(&slock);
}

void present_surface_set_shape(present_surface *ps, const uint8_t *mask, int w, int h)
{
    if (!ps)
        return;
    PresentSurface *s = surface(ps);
    pthread_mutex_lock(&slock);
    if (!mask || w <= 0 || h <= 0) {
        if (s->shape) {
            bb_host_free(s->shape);
            s->shape = NULL;
            s->needs = true;
        }
    } else if (!s->shape || s->sw != w || s->sh != h || memcmp(s->shape, mask, (size_t)w * (size_t)h)) {
        bb_host_free(s->shape);
        s->shape = bb_host_malloc((size_t)w * (size_t)h);
        if (s->shape)
            memcpy(s->shape, mask, (size_t)w * (size_t)h);
        s->sw = w, s->sh = h;
        s->shape_dirty = s->needs = true;
    }
    pthread_mutex_unlock(&slock);
}

void present_surface_set_desktop(present_surface *ps, bool leave_out)
{
    if (!ps)
        return;
    PresentSurface *s = surface(ps);
    pthread_mutex_lock(&slock);
    if (s->desktop != leave_out) {
        s->desktop = leave_out;
        s->needs = true;
        if (leave_out)
            desk_valid = false; /* not kept up to date meanwhile */
    }
    pthread_mutex_unlock(&slock);
}

void present_surface_set_visible(present_surface *ps, bool visible)
{
    if (!ps)
        return;
    PresentSurface *s = surface(ps);
    pthread_mutex_lock(&slock);
    if (s->visible != visible) {
        s->visible = visible;
        if (visible)
            s->needs = true;
    }
    pthread_mutex_unlock(&slock);
}

/* ---- The copy of the screen ---- */

/* With the lock held: the screen's pages wrapped for the GPU, or false
 * (the path then falls back to shared). */
static bool wrap_pages(const present_state *st)
{
    size_t page = (size_t)getpagesize();
    uintptr_t at = (uintptr_t)gptr(st->base), end = at + (size_t)st->row_bytes * (size_t)st->height;
    if (nocopy && at >= nc_lo && end <= nc_lo + nc_len)
        return true;
    if (last_copy)
        [last_copy waitUntilCompleted];
    nocopy = nil;
    uintptr_t lo = at & ~(uintptr_t)(page - 1), hi = (end + page - 1) & ~(uintptr_t)(page - 1);
    nocopy = [device newBufferWithBytesNoCopy:(void *)lo
                                       length:hi - lo
                                      options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeDefaultCache
                                  deallocator:nil];
    if (!nocopy) {
        nc_lo = 0;
        nc_len = 0;
        return false;
    }
    nc_lo = lo;
    nc_len = hi - lo;
    return true;
}

/* Zero-copy's rows written, in bytes, from which the GPU copies them
 * (BLUEBOX_GPU_COPY_KB; 0 for always). */
static size_t gpu_copy_bytes(void)
{
    static long kb = -1;
    if (kb < 0) {
        const char *e = getenv("BLUEBOX_GPU_COPY_KB");
        kb = e && *e ? atol(e) : 6144;
        if (kb < 0)
            kb = 0;
    }
    return (size_t)kb * 1024;
}

/* Whether row y is in bands. */
static bool in_bands(const int32_t *bands, int n, int y)
{
    for (int i = 0; i < n; i++)
        if (y >= bands[i * 2] && y < bands[i * 2 + 1])
            return true;
    return false;
}

/* With the lock held: the copy brought up to date with the screen.  The
 * changed rows as bands; *all when everything is to be drawn again.
 * Zero-copy's copy is the GPU's, made by the command buffer *copy (to be
 * committed before the drawing). */
static int update_copy(int32_t *bands, bool force, bool *all, bool *scanned, id<MTLCommandBuffer> *copy)
{
    present_state st;
    present_screen_state(&st);
    *all = force || !copy_valid || memcmp(&st, &shown_state, sizeof st) != 0;
    *scanned = false;
    *copy = nil;
    /* The rows written since the last refresh (dirty.c): none, and nothing
     * is to be done.  Everything is looked at once a second while the
     * emulator runs, in case a writer wasn't seen. */
    int32_t marked[MAX_BANDS * 2];
    int nm = present_dirty_rows(&dirty_seen, 0, st.height, marked, MAX_BANDS);
    bool tracked = nm != PRESENT_DIRTY_UNKNOWN;
    if (nm == PRESENT_DIRTY_ALL)
        *all = true;
    if (nm < 0)
        nm = 0;
    uint64_t progress = present_machine_progress();
    bool full = ++since_full >= (tracked ? FULL_EVERY : SCAN_AT_LEAST) || (!tracked && progress != progress_seen);
    if (full && tracked && progress == progress_seen)
        full = false, since_full = 0; /* nothing ran: nothing to look for */
    if (!*all && !nm && !full)
        return 0;
    if (full) {
        since_full = 0;
        progress_seen = progress;
    }
    size_t len = (size_t)st.row_bytes * (size_t)st.height;
    if (len == 0)
        return 0;
    if (path == PATH_ZERO_COPY && !wrap_pages(&st)) {
        path = PATH_SHARED; /* this screen can't be wrapped: copied instead */
        *all = true;
    }
    MTLResourceOptions opts = path == PATH_MANAGED ? MTLResourceStorageModeManaged : MTLResourceStorageModeShared;
    if (!rows_buf || rows_buf.length < len || (rows_buf.resourceOptions & MTLResourceStorageModeMask) !=
                                                    (opts & MTLResourceStorageModeMask)) {
        wait_gpu();
        rows_buf = [device newBufferWithLength:len options:opts];
        *all = true;
    }
    if (!rows_buf)
        return 0;
    int n;
    /* Zero-copy: many rows written are copied by the GPU from the screen's
     * own pages (the processor copies nothing); a few by the processor, as
     * shared does, which lets a waiting program go sooner than the GPU's
     * round trip would. */
    size_t marked_bytes = 0;
    for (int i = 0; i < nm; i++)
        marked_bytes += (size_t)(marked[i * 2 + 1] - marked[i * 2]) * st.row_bytes;
    bool by_gpu = path == PATH_ZERO_COPY && (*all ? len : !full && tracked ? marked_bytes : 0) >= gpu_copy_bytes();
    if (by_gpu) {
        if (*all || !tracked) {
            bands[0] = 0;
            bands[1] = st.height;
            n = 1;
            *all = true;
        } else {
            memcpy(bands, marked, sizeof(int32_t) * 2 * (size_t)nm);
            n = nm;
        }
        id<MTLCommandBuffer> c = [queue commandBuffer];
        id<MTLBlitCommandEncoder> b = [c blitCommandEncoder];
        size_t off = (size_t)((uintptr_t)gptr(st.base) - nc_lo);
        for (int i = 0; i < n; i++) {
            size_t y0 = (size_t)bands[i * 2], y1 = (size_t)bands[i * 2 + 1];
            if (y1 > (size_t)st.height)
                y1 = (size_t)st.height;
            if (y1 > y0)
                [b copyFromBuffer:nocopy
                         sourceOffset:off + y0 * st.row_bytes
                             toBuffer:rows_buf
                    destinationOffset:y0 * st.row_bytes
                                 size:(y1 - y0) * st.row_bytes];
        }
        [b endEncoding];
        *copy = c;
    } else {
        uint8_t *rows = rows_buf.contents;
        wait_gpu(); /* the GPU done with the copy before it changes */
        if (*all) {
            memcpy(rows, gptr(st.base), len);
            bands[0] = 0;
            bands[1] = st.height;
            n = 1;
        } else if (full) {
            /* Compared everywhere: a row changed outside the marked ones
             * is a writer the tracking missed (counted, and shown). */
            n = present_scan_rows(&st, rows, bands, MAX_BANDS);
            *scanned = true;
            uint64_t missed = 0;
            for (int i = 0; tracked && n < MAX_BANDS && i < n; i++)
                for (int y = bands[i * 2]; y < bands[i * 2 + 1]; y++)
                    missed += !in_bands(marked, nm, y);
            present_stats_missed(missed);
        } else {
            /* Only the rows written, compared and copied: those rewritten
             * the same draw nothing. */
            n = present_scan_bands(&st, rows, marked, nm, bands, MAX_BANDS);
        }
        if (n < 0) { /* the screen changed form under us */
            memcpy(rows, gptr(st.base), len);
            bands[0] = 0;
            bands[1] = st.height;
            n = 1;
            *all = true;
        }
        if (path == PATH_MANAGED)
            for (int i = 0; i < n; i++)
                [rows_buf didModifyRange:NSMakeRange((size_t)bands[i * 2] * st.row_bytes,
                                                     (size_t)(bands[i * 2 + 1] - bands[i * 2]) * st.row_bytes)];
    }
    shown_state = st;
    copy_valid = true;
    return n;
}

/* With the lock held: the desktop's mask over the changed rows. */
static void update_desk(const int32_t *bands, int n, bool all)
{
    int w = shown_state.width, h = shown_state.height;
    if (!desk_tex || (int)desk_tex.width != w || (int)desk_tex.height != h) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Uint
                                                                                      width:(NSUInteger)w
                                                                                     height:(NSUInteger)h
                                                                                  mipmapped:NO];
        desk_tex = [device newTextureWithDescriptor:td];
        desk_valid = false;
    }
    int32_t whole[2] = { 0, h };
    if (!desk_valid || all) {
        bands = whole;
        n = 1;
    }
    for (int i = 0; i < n; i++) {
        int y0 = bands[i * 2], y1 = bands[i * 2 + 1] < h ? bands[i * 2 + 1] : h;
        if (y1 <= y0)
            continue;
        uint8_t *m = bb_host_malloc((size_t)w * (size_t)(y1 - y0));
        if (!m)
            continue;
        if (!machine_desktop_mask(0, y0, w, y1 - y0, m))
            memset(m, 1, (size_t)w * (size_t)(y1 - y0));
        [desk_tex replaceRegion:MTLRegionMake2D(0, (NSUInteger)y0, (NSUInteger)w, (NSUInteger)(y1 - y0))
                    mipmapLevel:0
                      withBytes:m
                    bytesPerRow:(NSUInteger)w];
        bb_host_free(m);
    }
    desk_valid = true;
}

/* ---- Drawing ---- */

/* With the lock held: s drawn into target (w x h pixels). */
static void encode(id<MTLCommandBuffer> cmd, PresentSurface *s, id<MTLTexture> target)
{
    const present_state *st = &shown_state;
    if (s->shape && s->shape_dirty) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Uint
                                                                                      width:(NSUInteger)s->sw
                                                                                     height:(NSUInteger)s->sh
                                                                                  mipmapped:NO];
        s->shape_tex = [device newTextureWithDescriptor:td];
        [s->shape_tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)s->sw, (NSUInteger)s->sh)
                        mipmapLevel:0
                          withBytes:s->shape
                        bytesPerRow:(NSUInteger)s->sw];
        s->shape_dirty = false;
    }
    uniforms u;
    memset(&u, 0, sizeof u);
    u.origin[0] = (float)s->x, u.origin[1] = (float)s->y;
    u.size[0] = (float)s->w, u.size[1] = (float)s->h;
    u.dst[0] = (float)target.width, u.dst[1] = (float)target.height;
    u.row_bytes = st->row_bytes;
    u.width = st->width, u.height = st->height, u.depth = st->depth;
    u.brightness = st->brightness;
    u.lut_on = st->lut_on;
    double rx = target.width / s->w, ry = target.height / s->h;
    u.sharp = fabs(rx - round(rx)) > 1e-3 || fabs(ry - round(ry)) > 1e-3 || rx < 1 || ry < 1;
    u.has_shape = s->shape && s->shape_tex;
    u.shape_x = (int)floor(s->x), u.shape_y = (int)floor(s->y), u.shape_w = s->sw, u.shape_h = s->sh;
    u.has_desk = s->desktop && desk_tex;
    u.cx0 = (int)floor(s->x), u.cy0 = (int)floor(s->y);
    u.cx1 = (int)ceil(s->x + s->w) - 1, u.cy1 = (int)ceil(s->y + s->h) - 1;
    id<MTLBuffer> src = rows_buf;
    uint32_t clut[256];
    for (int i = 0; i < 256; i++)
        clut[i] = (uint32_t)st->clut[i][0] | (uint32_t)st->clut[i][1] << 8 | (uint32_t)st->clut[i][2] << 16;
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:pipe_layer];
    [enc setFragmentBytes:&u length:sizeof u atIndex:0];
    [enc setFragmentBuffer:src offset:0 atIndex:1];
    [enc setFragmentBytes:clut length:sizeof clut atIndex:2];
    [enc setFragmentBytes:st->lut length:sizeof st->lut atIndex:3];
    [enc setFragmentTexture:u.has_shape ? s->shape_tex : dummy atIndex:0];
    [enc setFragmentTexture:u.has_desk ? desk_tex : dummy atIndex:1];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
}

static bool touches(PresentSurface *s, const int32_t *bands, int n)
{
    double y0 = floor(s->y), y1 = ceil(s->y + s->h) + 1; /* (the sharp filter's neighbour) */
    for (int i = 0; i < n; i++)
        if (bands[i * 2] < y1 && bands[i * 2 + 1] > y0)
            return true;
    return false;
}

/* A refresh: the copy brought up to date and the surfaces it changed
 * drawn.  only: just that surface, now, as the copy is.  Called with the
 * lock held; it lets go of it while it waits for the layers' drawables
 * (the host's thread may set a surface meanwhile). */
static void refresh(PresentSurface *only)
{
    uint64_t t0 = present_now_ns();
    uint64_t seq = only ? 0 : present_vsync_begin();
    bool any = false, desk = false;
    pthread_mutex_lock(&slock);
    for (PresentSurface *s in surfaces)
        if (s->visible && s->pw > 0 && s->w > 0) {
            any = true;
            desk |= s->desktop;
        }
    pthread_mutex_unlock(&slock);
    if (!only)
        present_set_shown(any);
    if (!any || (only && !copy_valid)) {
        if (seq)
            present_vsync_read(seq);
        return;
    }
    int32_t bands[MAX_BANDS * 2];
    bool all = false, scanned = false;
    int n = 0;
    id<MTLCommandBuffer> copy = nil;
    if (!only)
        n = update_copy(bands, false, &all, &scanned, &copy);
    if (copy) {
        /* Zero-copy: the GPU's copy of the rows goes first, on its own, and
         * a program waiting for the refresh goes on once it is done, not
         * after the drawing. */
        if (seq) {
            uint64_t s0 = seq;
            [copy addCompletedHandler:^(id<MTLCommandBuffer> b) {
                (void)b;
                present_stats_read(present_now_ns() - t0);
                present_vsync_read(s0);
            }];
            seq = 0;
        }
        [copy commit];
        last_copy = copy;
    }
    if (!copy_valid) {
        if (seq)
            present_vsync_read(seq);
        return;
    }
    if (desk && (n || all || !desk_valid))
        update_desk(bands, n, all);
    int rows = 0;
    for (int i = 0; i < n; i++)
        rows += bands[i * 2 + 1] - bands[i * 2];
    uint64_t bytes = path == PATH_ZERO_COPY ? 0 : (uint64_t)rows * shown_state.row_bytes;
    NSMutableArray<PresentSurface *> *draw = [NSMutableArray new];
    pthread_mutex_lock(&slock);
    for (PresentSurface *s in surfaces) {
        if (!s->visible || s->pw <= 0 || s->w <= 0)
            continue;
        if (only ? s == only : (s->needs || all || touches(s, bands, n)))
            [draw addObject:s];
    }
    pthread_mutex_unlock(&slock);
    if (seq) {
        present_vsync_read(seq); /* the copy is taken (or nothing to read): the program may go on */
        present_stats_read(present_now_ns() - t0);
        seq = 0;
    }
    if (!draw.count) {
        present_stats_gpu(scanned, !scanned, 0, rows, bytes, present_now_ns() - t0);
        return;
    }
    /* The drawables, without the lock: a layer's next one can take until
     * the compositor is done with one. */
    NSMutableArray *drawables = [NSMutableArray new];
    pthread_mutex_unlock(&lock);
    for (PresentSurface *s in draw) {
        id<CAMetalDrawable> d = [s->layer nextDrawable];
        [drawables addObject:d ? (id)d : (id)[NSNull null]];
    }
    pthread_mutex_lock(&lock);
    pthread_mutex_lock(&slock);
    if (!copy_valid) { /* the memory moved meanwhile */
        for (PresentSurface *s in draw)
            s->needs = true;
        pthread_mutex_unlock(&slock);
        return;
    }
    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    int drawn = 0;
    for (NSUInteger i = 0; i < draw.count; i++) {
        PresentSurface *s = draw[i];
        id d = drawables[i];
        if (d == [NSNull null] || ![surfaces containsObject:s])
            continue;
        id<CAMetalDrawable> drawable = d;
        encode(cmd, s, drawable.texture);
        [cmd presentDrawable:drawable];
        s->needs = false;
        drawn++;
    }
    pthread_mutex_unlock(&slock);
    [cmd commit];
    last_cmd = cmd;
    present_stats_gpu(scanned, !scanned && !only, drawn, rows, bytes, present_now_ns() - t0);
}

void present_surface_draw_now(present_surface *ps)
{
    if (!ps)
        return;
    pthread_mutex_lock(&lock);
    @autoreleasepool {
        refresh(surface(ps));
    }
    pthread_mutex_unlock(&lock);
}

/* ---- Refreshes ---- */

static void *render_thread(void *arg)
{
    (void)arg;
    pthread_setname_np("Blue Box present");
    for (;;) {
        pthread_mutex_lock(&vlock);
        while (!vsync_wanted)
            pthread_cond_wait(&wake, &vlock);
        vsync_wanted = false;
        pthread_mutex_unlock(&vlock);
        uint64_t cpu0 = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
        pthread_mutex_lock(&lock);
        @autoreleasepool {
            refresh(nil);
        }
        pthread_mutex_unlock(&lock);
        present_stats_cpu(clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - cpu0);
    }
    return NULL;
}

/* Called on the host's display-link thread or its main thread: only the
 * refreshes' own lock, never the presenter's (held while it draws). */
void present_vsync(double hz)
{
    if (started != 1)
        return;
    pthread_mutex_lock(&vlock);
    if (!thread_running) {
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_set_qos_class_np(&a, QOS_CLASS_USER_INTERACTIVE, 0);
        thread_running = pthread_create(&t, &a, render_thread, NULL) == 0;
        pthread_attr_destroy(&a);
        if (thread_running)
            pthread_detach(t);
    }
    /* Paced programs go by 60 refreshes a second, whatever the display's
     * rate (a ProMotion display's 120): the others are skipped.  With
     * pacing off (BLUEBOX_PACE=0) every refresh is drawn. */
    static int pacing = -1;
    if (pacing < 0) {
        const char *e = getenv("BLUEBOX_PACE");
        pacing = !(e && e[0] == '0');
    }
    vsync_every = pacing && hz > 61 ? (int)lround(hz / 60) : 1;
    if (vsync_every < 1)
        vsync_every = 1;
    if (++vsync_count % (uint64_t)vsync_every == 0) {
        vsync_wanted = true;
        pthread_cond_signal(&wake);
    }
    pthread_mutex_unlock(&vlock);
}

void present_wake(void) { present_vsync(60); }

static CVReturn link_callback(CVDisplayLinkRef l, const CVTimeStamp *now, const CVTimeStamp *out, CVOptionFlags in,
                              CVOptionFlags *flags, void *ctx)
{
    (void)now, (void)in, (void)flags, (void)ctx;
    double hz = out->videoRefreshPeriod ? (double)out->videoTimeScale / (double)out->videoRefreshPeriod : 60;
    (void)l;
    present_vsync(hz);
    return kCVReturnSuccess;
}

bool present_own_display_link(uint32_t display_id)
{
    if (!present_start())
        return false;
    if (link_ref) {
        CVDisplayLinkSetCurrentCGDisplay(link_ref, display_id);
        return true;
    }
    if (CVDisplayLinkCreateWithCGDisplay(display_id, &link_ref) != kCVReturnSuccess)
        return false;
    CVDisplayLinkSetOutputCallback(link_ref, link_callback, NULL);
    return CVDisplayLinkStart(link_ref) == kCVReturnSuccess;
}

void present_stop_display_link(void)
{
    if (link_ref) {
        CVDisplayLinkStop(link_ref);
        CVDisplayLinkRelease(link_ref);
        link_ref = NULL;
    }
}

/* ---- Offscreen (tests, benchmarks) ---- */

bool present_offscreen(double x, double y, double w, double h, int out_w, int out_h, const uint8_t *shape, bool desktop,
                       uint8_t *rgba)
{
    if (!present_start() || out_w <= 0 || out_h <= 0 || w <= 0 || h <= 0)
        return false;
    bool ok = false;
    pthread_mutex_lock(&lock);
    @autoreleasepool {
        int32_t bands[MAX_BANDS * 2];
        bool all, scanned;
        id<MTLCommandBuffer> copy = nil;
        int n = update_copy(bands, true, &all, &scanned, &copy);
        (void)n;
        if (copy) {
            [copy commit];
            last_copy = copy;
        }
        if (copy_valid) {
            if (desktop)
                update_desk(bands, 1, true);
            PresentSurface *s = [PresentSurface new];
            s->x = x, s->y = y, s->w = w, s->h = h, s->pw = out_w, s->ph = out_h;
            s->desktop = desktop;
            if (shape) {
                int sw = (int)ceil(w), sh = (int)ceil(h);
                s->shape = bb_host_malloc((size_t)sw * (size_t)sh);
                memcpy(s->shape, shape, (size_t)sw * (size_t)sh);
                s->sw = sw, s->sh = sh;
                s->shape_dirty = true;
            }
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:(NSUInteger)out_w
                                                                                         height:(NSUInteger)out_h
                                                                                      mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModeShared;
            if (!device.hasUnifiedMemory)
                td.storageMode = MTLStorageModeManaged;
            id<MTLTexture> target = [device newTextureWithDescriptor:td];
            id<MTLCommandBuffer> cmd = [queue commandBuffer];
            encode(cmd, s, target);
            if (td.storageMode == MTLStorageModeManaged) {
                id<MTLBlitCommandEncoder> b = [cmd blitCommandEncoder];
                [b synchronizeResource:target];
                [b endEncoding];
            }
            [cmd commit];
            last_cmd = cmd;
            [cmd waitUntilCompleted];
            [target getBytes:rgba
                 bytesPerRow:(NSUInteger)out_w * 4
                  fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)out_w, (NSUInteger)out_h)
                 mipmapLevel:0];
            for (size_t i = 0, k = (size_t)out_w * (size_t)out_h; i < k; i++) { /* BGRA to RGBA */
                uint8_t t = rgba[i * 4];
                rgba[i * 4] = rgba[i * 4 + 2];
                rgba[i * 4 + 2] = t;
            }
            ok = cmd.status == MTLCommandBufferStatusCompleted;
        }
    }
    pthread_mutex_unlock(&lock);
    return ok;
}
#endif
