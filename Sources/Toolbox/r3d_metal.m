/*
 * r3d.h on Metal (macOS): a frame is a render target and a depth texture
 * of its size, private to the GPU, with a shared buffer the colours come
 * back through.  A frame's draws are kept until r3d_end, then encoded in
 * one command buffer: a pass per clear, the vertices in one buffer, a
 * pipeline per blend mode and depth and sampler states as the draws ask.
 * Metal renders offscreen without a window or a window server, so the
 * headless runs and the tests draw as the windowed ones do.
 */
#ifdef __APPLE__
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <stdlib.h>
#include <string.h>

#include "r3d.h"

#if !__has_feature(objc_arc)
#error "r3d_metal.m needs ARC"
#endif

static const char *shader_source =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VIn { float4 p; float4 c; float4 t; float4 s; };\n"
    "struct VOut { float4 pos [[position]]; float4 c; float2 uv; float3 kd; float3 ks; float size [[point_size]]; };\n"
    "struct U { float2 view; float size; int textured; int op; };\n"
    "vertex VOut vs(uint i [[vertex_id]], const device VIn *v [[buffer(0)]], constant U &u [[buffer(1)]]) {\n"
    "  VIn a = v[i]; VOut o;\n"
    "  float w = a.p.w > 0 ? 1.0 / a.p.w : 1.0;\n"
    "  float x = a.p.x * 2.0 / u.view.x - 1.0, y = 1.0 - a.p.y * 2.0 / u.view.y;\n"
    "  o.pos = float4(x * w, y * w, clamp(a.p.z, 0.0, 1.0) * w, w);\n"
    "  o.c = a.c; o.uv = a.t.xy; o.kd = float3(a.t.z, a.t.w, a.s.x); o.ks = a.s.yzw; o.size = u.size;\n"
    "  return o;\n"
    "}\n"
    "fragment float4 fs(VOut i [[stage_in]], constant U &u [[buffer(1)]], texture2d<float> tx [[texture(0)]],\n"
    "                   sampler sm [[sampler(0)]]) {\n"
    "  if (!u.textured) return i.c;\n"
    "  float4 t = tx.sample(sm, i.uv);\n"
    "  float3 rgb;\n"
    "  if (u.op & 4) rgb = mix(i.c.rgb, t.rgb, t.a);\n"
    "  else if (u.op & 1) rgb = t.rgb * i.kd + i.ks;\n"
    "  else if (u.op & 2) rgb = t.rgb + i.ks;\n"
    "  else rgb = t.rgb;\n"
    "  return float4(clamp(rgb, 0.0, 1.0), (u.op & 4) ? i.c.a : t.a * i.c.a);\n"
    "}\n";

typedef struct {
    float view[2];
    float size;
    int32_t textured, op;
} uniforms;

enum { CMD_DRAW, CMD_CLEAR };

typedef struct {
    int kind;
    r3d_state s;
    int primitive, first, count;
    int x0, y0, x1, y1;
    bool color, depth;
    uint32_t argb;
    float z;
} command;

@interface R3DFrame : NSObject {
@public
    int width, height;
    id<MTLTexture> color, depth;
    id<MTLBuffer> readback;
    uint32_t clear_argb;
    float clear_z;
    /* Where the next pass's colour and depth come from: cleared, the
     * readback buffers (r3d_begin's pixels, r3d_write's), or the textures
     * as the last pass left them (after r3d_read). */
    int color_src, depth_src;
    id<MTLBuffer> depthback;
    r3d_vertex *verts;
    int nverts, capverts;
    command *cmds;
    int ncmds, capcmds;
}
@end
@implementation R3DFrame
enum { SRC_CLEAR, SRC_BUFFER, SRC_TEXTURE };
- (void)dealloc
{
    free(verts);
    free(cmds);
}
@end

static id<MTLDevice> device;
static id<MTLCommandQueue> queue;
static id<MTLRenderPipelineState> pipelines[3], depth_only;
static id<MTLDepthStencilState> depth_states[9][2];
static id<MTLSamplerState> samplers[2][2][2];
static id<MTLTexture> white; /* bound when nothing is textured */
static int available = -1;

enum { MAX_TEXTURES = 4096 };
static id<MTLTexture> textures[MAX_TEXTURES];

static MTLRenderPipelineDescriptor *pipeline_descriptor(id<MTLLibrary> lib, int blend)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"vs"];
    d.fragmentFunction = [lib newFunctionWithName:@"fs"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    if (blend != R3D_BLEND_NONE) {
        MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[0];
        c.blendingEnabled = YES;
        c.sourceRGBBlendFactor = blend == R3D_BLEND_PREMULTIPLY ? MTLBlendFactorOne : MTLBlendFactorSourceAlpha;
        c.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        c.sourceAlphaBlendFactor = MTLBlendFactorOne;
        c.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    }
    return d;
}

bool r3d_available(void)
{
    if (available >= 0)
        return available;
    available = 0;
    @autoreleasepool {
        device = MTLCreateSystemDefaultDevice();
        if (!device)
            return false;
        queue = [device newCommandQueue];
        NSError *err = nil;
        id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:shader_source]
                                                  options:nil
                                                    error:&err];
        if (!lib || !queue)
            return false;
        for (int b = 0; b < 3; b++) {
            pipelines[b] = [device newRenderPipelineStateWithDescriptor:pipeline_descriptor(lib, b) error:&err];
            if (!pipelines[b])
                return false;
        }
        MTLRenderPipelineDescriptor *dd = pipeline_descriptor(lib, R3D_BLEND_NONE);
        dd.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
        depth_only = [device newRenderPipelineStateWithDescriptor:dd error:&err];
        if (!depth_only)
            return false;
        static const MTLCompareFunction fns[9] = {
            MTLCompareFunctionAlways, MTLCompareFunctionLess,         MTLCompareFunctionEqual,
            MTLCompareFunctionLessEqual, MTLCompareFunctionGreater,   MTLCompareFunctionNotEqual,
            MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways, MTLCompareFunctionNever,
        };
        for (int f = 0; f < 9; f++)
            for (int w = 0; w < 2; w++) {
                MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
                d.depthCompareFunction = fns[f];
                /* kQAZFunction_None neither tests nor writes. */
                d.depthWriteEnabled = w && f != R3D_Z_NONE;
                depth_states[f][w] = [device newDepthStencilStateWithDescriptor:d];
            }
        for (int lin = 0; lin < 2; lin++)
            for (int cu = 0; cu < 2; cu++)
                for (int cv = 0; cv < 2; cv++) {
                    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
                    d.minFilter = d.magFilter = lin ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
                    d.mipFilter = lin ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNotMipmapped;
                    d.sAddressMode = cu ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
                    d.tAddressMode = cv ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
                    samplers[lin][cu][cv] = [device newSamplerStateWithDescriptor:d];
                }
        uint32_t px = 0xFFFFFFFF;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:1
                                                                                     height:1
                                                                                  mipmapped:NO];
        white = [device newTextureWithDescriptor:td];
        [white replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&px bytesPerRow:4];
    }
    available = 1;
    return true;
}

r3d_frame *r3d_frame_new(int width, int height)
{
    if (!r3d_available() || width <= 0 || height <= 0 || width > 16384 || height > 16384)
        return NULL;
    R3DFrame *f = [R3DFrame new];
    f->width = width;
    f->height = height;
    MTLTextureDescriptor *cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:(NSUInteger)width
                                                                                 height:(NSUInteger)height
                                                                              mipmapped:NO];
    cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    cd.storageMode = MTLStorageModePrivate;
    f->color = [device newTextureWithDescriptor:cd];
    MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                  width:(NSUInteger)width
                                                                                 height:(NSUInteger)height
                                                                              mipmapped:NO];
    dd.usage = MTLTextureUsageRenderTarget;
    dd.storageMode = MTLStorageModePrivate;
    f->depth = [device newTextureWithDescriptor:dd];
    f->readback = [device newBufferWithLength:(NSUInteger)width * (NSUInteger)height * 4
                                      options:MTLResourceStorageModeShared];
    f->depthback = [device newBufferWithLength:(NSUInteger)width * (NSUInteger)height * 4
                                       options:MTLResourceStorageModeShared];
    if (!f->color || !f->depth || !f->readback || !f->depthback)
        return NULL;
    return (__bridge_retained r3d_frame *)f;
}

void r3d_frame_free(r3d_frame *f)
{
    if (f) {
        R3DFrame *o = (__bridge_transfer R3DFrame *)f;
        (void)o;
    }
}

static R3DFrame *frame(r3d_frame *f) { return (__bridge R3DFrame *)f; }

int r3d_frame_width(const r3d_frame *f) { return f ? frame((r3d_frame *)f)->width : 0; }
int r3d_frame_height(const r3d_frame *f) { return f ? frame((r3d_frame *)f)->height : 0; }

static command *new_command(R3DFrame *o)
{
    if (o->ncmds == o->capcmds) {
        int cap = o->capcmds ? o->capcmds * 2 : 64;
        command *n = realloc(o->cmds, (size_t)cap * sizeof *n);
        if (!n)
            return NULL;
        o->cmds = n;
        o->capcmds = cap;
    }
    command *c = &o->cmds[o->ncmds++];
    memset(c, 0, sizeof *c);
    return c;
}

void r3d_begin(r3d_frame *f, const uint32_t *pixels, uint32_t clear_argb, float clear_z)
{
    if (!f)
        return;
    R3DFrame *o = frame(f);
    o->nverts = o->ncmds = 0;
    o->clear_argb = clear_argb;
    o->clear_z = clear_z;
    o->color_src = pixels ? SRC_BUFFER : SRC_CLEAR;
    o->depth_src = SRC_CLEAR;
    if (pixels)
        memcpy([o->readback contents], pixels, (size_t)o->width * (size_t)o->height * 4);
}

void r3d_clear(r3d_frame *f, int x0, int y0, int x1, int y1, bool color, uint32_t argb, bool depth, float z)
{
    if (!f)
        return;
    R3DFrame *o = frame(f);
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > o->width)
        x1 = o->width;
    if (y1 > o->height)
        y1 = o->height;
    if (x0 >= x1 || y0 >= y1 || (!color && !depth))
        return;
    command *c = new_command(o);
    if (!c)
        return;
    c->kind = CMD_CLEAR;
    c->x0 = x0, c->y0 = y0, c->x1 = x1, c->y1 = y1;
    c->color = color, c->argb = argb, c->depth = depth, c->z = z;
}

void r3d_draw(r3d_frame *f, const r3d_state *s, int primitive, const r3d_vertex *v, int count)
{
    if (!f || count <= 0)
        return;
    R3DFrame *o = frame(f);
    if (o->nverts + count > o->capverts) {
        int cap = o->capverts ? o->capverts : 1024;
        while (cap < o->nverts + count)
            cap *= 2;
        r3d_vertex *n = realloc(o->verts, (size_t)cap * sizeof *n);
        if (!n)
            return;
        o->verts = n;
        o->capverts = cap;
    }
    /* Runs of one state are one draw. */
    command *last = o->ncmds ? &o->cmds[o->ncmds - 1] : NULL;
    if (last && last->kind == CMD_DRAW && last->primitive == primitive && !memcmp(&last->s, s, sizeof *s) &&
        last->first + last->count == o->nverts) {
        last->count += count;
    } else {
        command *c = new_command(o);
        if (!c)
            return;
        c->kind = CMD_DRAW;
        c->s = *s;
        c->primitive = primitive;
        c->first = o->nverts;
        c->count = count;
    }
    memcpy(o->verts + o->nverts, v, (size_t)count * sizeof *v);
    o->nverts += count;
}

static MTLClearColor clear_color(uint32_t argb)
{
    return MTLClearColorMake(((argb >> 16) & 0xFF) / 255.0, ((argb >> 8) & 0xFF) / 255.0, (argb & 0xFF) / 255.0,
                             (argb >> 24) / 255.0);
}

static void encode_clear(R3DFrame *o, id<MTLRenderCommandEncoder> e, const command *c)
{
    /* A quad over the rectangle, at the depth asked, writing what's asked. */
    float z = c->depth ? c->z : 0;
    float r = ((c->argb >> 16) & 0xFF) / 255.0f, g = ((c->argb >> 8) & 0xFF) / 255.0f, b = (c->argb & 0xFF) / 255.0f,
          a = (c->argb >> 24) / 255.0f;
    r3d_vertex q[6];
    float xs[6] = { (float)c->x0, (float)c->x1, (float)c->x0, (float)c->x1, (float)c->x1, (float)c->x0 };
    float ys[6] = { (float)c->y0, (float)c->y0, (float)c->y1, (float)c->y0, (float)c->y1, (float)c->y1 };
    for (int i = 0; i < 6; i++)
        q[i] = (r3d_vertex){ xs[i], ys[i], z, 1, r, g, b, a, 0, 0, 1, 1, 1, 0, 0, 0 };
    uniforms u = { { (float)o->width, (float)o->height }, 1, 0, 0 };
    [e setRenderPipelineState:pipelines[R3D_BLEND_NONE]];
    [e setDepthStencilState:depth_states[R3D_Z_TRUE][c->depth]];
    [e setVertexBytes:q length:sizeof q atIndex:0];
    [e setVertexBytes:&u length:sizeof u atIndex:1];
    [e setFragmentBytes:&u length:sizeof u atIndex:1];
    [e setFragmentTexture:white atIndex:0];
    [e setFragmentSamplerState:samplers[0][1][1] atIndex:0];
    if (!c->color) /* depth alone */
        [e setRenderPipelineState:depth_only];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
}

/* The frame's commands drawn; the colour (and depth) read back if asked.
 * keep: more of the frame follows, so its buffers are kept. */
static void run(R3DFrame *o, bool keep, uint32_t *pixels, float *depth_out)
{
    @autoreleasepool {
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        NSUInteger w = (NSUInteger)o->width, h = (NSUInteger)o->height;
        if (o->depth_src == SRC_BUFFER) {
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b copyFromBuffer:o->depthback
                       sourceOffset:0
                  sourceBytesPerRow:w * 4
                sourceBytesPerImage:w * h * 4
                         sourceSize:MTLSizeMake(w, h, 1)
                          toTexture:o->depth
                   destinationSlice:0
                   destinationLevel:0
                  destinationOrigin:MTLOriginMake(0, 0, 0)];
            [b endEncoding];
        }
        if (o->color_src == SRC_BUFFER) {
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b copyFromBuffer:o->readback
                       sourceOffset:0
                  sourceBytesPerRow:w * 4
                sourceBytesPerImage:w * h * 4
                         sourceSize:MTLSizeMake(w, h, 1)
                          toTexture:o->color
                   destinationSlice:0
                   destinationLevel:0
                  destinationOrigin:MTLOriginMake(0, 0, 0)];
            [b endEncoding];
        }
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = o->color;
        rp.colorAttachments[0].loadAction = o->color_src == SRC_CLEAR ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.colorAttachments[0].clearColor = clear_color(o->clear_argb);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.depthAttachment.texture = o->depth;
        rp.depthAttachment.loadAction = o->depth_src == SRC_CLEAR ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.depthAttachment.clearDepth = o->clear_z;
        rp.depthAttachment.storeAction = keep ? MTLStoreActionStore : MTLStoreActionDontCare;
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        id<MTLBuffer> vb = o->nverts ? [device newBufferWithBytes:o->verts
                                                           length:(NSUInteger)o->nverts * sizeof(r3d_vertex)
                                                          options:MTLResourceStorageModeShared]
                                     : nil;
        [e setCullMode:MTLCullModeNone];
        for (int i = 0; i < o->ncmds; i++) {
            const command *c = &o->cmds[i];
            if (c->kind == CMD_CLEAR) {
                encode_clear(o, e, c);
                continue;
            }
            const r3d_state *s = &c->s;
            int blend = s->blend >= 0 && s->blend < 3 ? s->blend : 0;
            int zf = s->zfunc >= 0 && s->zfunc < 9 ? s->zfunc : R3D_Z_LT;
            id<MTLTexture> t = s->texture > 0 && s->texture < MAX_TEXTURES ? textures[s->texture] : nil;
            uniforms u = { { (float)o->width, (float)o->height }, s->size > 0 ? s->size : 1, t != nil, s->texop };
            [e setRenderPipelineState:pipelines[blend]];
            [e setDepthStencilState:depth_states[zf][s->zwrite ? 1 : 0]];
            [e setVertexBuffer:vb offset:(NSUInteger)c->first * sizeof(r3d_vertex) atIndex:0];
            [e setVertexBytes:&u length:sizeof u atIndex:1];
            [e setFragmentBytes:&u length:sizeof u atIndex:1];
            [e setFragmentTexture:t ? t : white atIndex:0];
            [e setFragmentSamplerState:samplers[s->linear][s->clamp_u][s->clamp_v] atIndex:0];
            MTLPrimitiveType p = c->primitive == R3D_LINES    ? MTLPrimitiveTypeLine
                                 : c->primitive == R3D_POINTS ? MTLPrimitiveTypePoint
                                                              : MTLPrimitiveTypeTriangle;
            [e drawPrimitives:p vertexStart:0 vertexCount:(NSUInteger)c->count];
        }
        [e endEncoding];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:o->color
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(w, h, 1)
                        toBuffer:o->readback
               destinationOffset:0
          destinationBytesPerRow:w * 4
        destinationBytesPerImage:w * h * 4];
        if (depth_out)
            [b copyFromTexture:o->depth
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(w, h, 1)
                            toBuffer:o->depthback
                   destinationOffset:0
              destinationBytesPerRow:w * 4
            destinationBytesPerImage:w * h * 4];
        [b endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (pixels)
            memcpy(pixels, [o->readback contents], w * h * 4);
        if (depth_out)
            memcpy(depth_out, [o->depthback contents], w * h * 4);
        o->nverts = o->ncmds = 0;
        if (keep)
            o->color_src = o->depth_src = SRC_TEXTURE;
    }
}

void r3d_end(r3d_frame *f, uint32_t *pixels)
{
    if (f)
        run(frame(f), false, pixels, NULL);
}

void r3d_read(r3d_frame *f, uint32_t *pixels, float *depth)
{
    if (f)
        run(frame(f), true, pixels, depth);
}

void r3d_write(r3d_frame *f, const uint32_t *pixels, const float *depth)
{
    if (!f)
        return;
    R3DFrame *o = frame(f);
    if (o->ncmds) /* what was drawn before stays before */
        run(o, true, NULL, NULL);
    size_t n = (size_t)o->width * (size_t)o->height * 4;
    if (pixels) {
        memcpy([o->readback contents], pixels, n);
        o->color_src = SRC_BUFFER;
    }
    if (depth) {
        memcpy([o->depthback contents], depth, n);
        o->depth_src = SRC_BUFFER;
    }
}



int r3d_texture_new(int width, int height, const uint32_t *pixels, bool mipmap)
{
    if (!r3d_available() || width <= 0 || height <= 0 || width > 16384 || height > 16384)
        return 0;
    int slot = 0;
    for (int i = 1; i < MAX_TEXTURES; i++)
        if (!textures[i]) {
            slot = i;
            break;
        }
    if (!slot)
        return 0;
    @autoreleasepool {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:(NSUInteger)width
                                                                                     height:(NSUInteger)height
                                                                                  mipmapped:mipmap];
        td.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> t = [device newTextureWithDescriptor:td];
        if (!t)
            return 0;
        [t replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)width, (NSUInteger)height)
             mipmapLevel:0
               withBytes:pixels
             bytesPerRow:(NSUInteger)width * 4];
        if (mipmap && t.mipmapLevelCount > 1) {
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b generateMipmapsForTexture:t];
            [b endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
        }
        textures[slot] = t;
    }
    return slot;
}

void r3d_texture_free(int texture)
{
    if (texture > 0 && texture < MAX_TEXTURES)
        textures[texture] = nil;
}
#endif
