/*
 * hostvideo.h on macOS: VideoToolbox for the video codecs it has (H.264,
 * HEVC, MPEG-4 Part 2, H.263, DV, MPEG-1 and 2 where the system has them),
 * ImageIO for the still-image ones (Motion JPEG A, PNG, TIFF, GIF,
 * JPEG 2000), AudioToolbox's converters for MP3, AAC, Apple Lossless and
 * whatever else of QuickTime's sound formats the system still decodes.
 *
 * Sorenson Video (SVQ1, SVQ3), Motion JPEG B and QDesign Music are not in
 * macOS any more: those movies stay with libvlc (qt_vlc.c).
 */
#ifdef __APPLE__
#import <AudioToolbox/AudioToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <VideoToolbox/VideoToolbox.h>

#include <stdlib.h>
#include <string.h>

#include "hostvideo.h"

#if !__has_feature(objc_arc)
#error "hostvideo_apple.m needs ARC"
#endif

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return be16(p) << 16 | be16(p + 2); }

#define FOUR(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

enum {
    AVC1 = FOUR('a', 'v', 'c', '1'), HVC1 = FOUR('h', 'v', 'c', '1'), HEV1 = FOUR('h', 'e', 'v', '1'),
    MP4V = FOUR('m', 'p', '4', 'v'), H263 = FOUR('h', '2', '6', '3'), S263 = FOUR('s', '2', '6', '3'),
    DVC = FOUR('d', 'v', 'c', ' '), DVCP = FOUR('d', 'v', 'c', 'p'), MP1V = FOUR('m', 'p', '1', 'v'),
    MP2V = FOUR('m', 'p', '2', 'v'), MJPA = FOUR('m', 'j', 'p', 'a'), PNG = FOUR('p', 'n', 'g', ' '),
    TIFF = FOUR('t', 'i', 'f', 'f'), GIF = FOUR('g', 'i', 'f', ' '), MJP2 = FOUR('m', 'j', 'p', '2'),
    JPEG = FOUR('j', 'p', 'e', 'g'),
};

/* ---- Still images (ImageIO) ---- */

static bool image_kind(uint32_t f) { return f == MJPA || f == PNG || f == TIFF || f == GIF || f == MJP2 || f == JPEG; }

/* An image's pixels, as RGBA scaled into w by h. */
static bool image_rgba(const uint8_t *data, size_t len, uint8_t *out, int w, int h, int row_step, int first_row)
{
    bool ok = false;
    CFDataRef d = CFDataCreateWithBytesNoCopy(NULL, data, (CFIndex)len, kCFAllocatorNull);
    CGImageSourceRef src = d ? CGImageSourceCreateWithData(d, NULL) : NULL;
    CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, NULL) : NULL;
    if (img) {
        int rows = (h - first_row + row_step - 1) / row_step;
        uint8_t *buf = calloc((size_t)w * (size_t)rows, 4);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = buf ? CGBitmapContextCreate(buf, (size_t)w, (size_t)rows, 8, (size_t)w * 4, cs,
                                                       kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big)
                               : NULL;
        if (ctx) {
            CGContextDrawImage(ctx, CGRectMake(0, 0, w, rows), img);
            for (int y = 0; y < rows; y++) {
                uint8_t *o = out + (size_t)(first_row + y * row_step) * (size_t)w * 4;
                memcpy(o, buf + (size_t)y * (size_t)w * 4, (size_t)w * 4);
                for (int x = 0; x < w; x++)
                    o[x * 4 + 3] = 255;
            }
            CGContextRelease(ctx);
            ok = true;
        }
        CGColorSpaceRelease(cs);
        free(buf);
        CGImageRelease(img);
    }
    if (src)
        CFRelease(src);
    if (d)
        CFRelease(d);
    return ok;
}

/* Motion JPEG A: one JPEG per field; two fields interleave, the first on
 * the even rows.  The second starts at the next start-of-image marker
 * past the first's end. */
static bool mjpa_decode(const uint8_t *data, size_t len, uint8_t *frame, int w, int h)
{
    size_t second = 0;
    for (size_t i = 4; i + 3 < len; i++)
        if (data[i] == 0xFF && data[i + 1] == 0xD9 && i + 4 <= len && data[i + 2] == 0xFF && data[i + 3] == 0xD8) {
            second = i + 2;
            break;
        }
    if (!second)
        return image_rgba(data, len, frame, w, h, 1, 0);
    return image_rgba(data, second, frame, w, h, 2, 0) && image_rgba(data + second, len - second, frame, w, h, 2, 1);
}

/* ---- VideoToolbox ---- */

struct hostvideo_dec {
    uint32_t format;
    int w, h;
    CMVideoFormatDescriptionRef desc;
    VTDecompressionSessionRef session;
    uint8_t *out;   /* the frame the callback fills */
    bool got;
};

static CMVideoCodecType vt_type(uint32_t f)
{
    if (f == S263)
        return kCMVideoCodecType_H263;
    if (f == HEV1)
        return kCMVideoCodecType_HEVC;
    if (f == MP1V)
        return kCMVideoCodecType_MPEG1Video;
    if (f == MP2V)
        return kCMVideoCodecType_MPEG2Video;
    return (CMVideoCodecType)f;
}

/* The description's extension atoms (after the fixed part and any colour
 * table), as CoreMedia wants them. */
static NSDictionary *extension_atoms(const uint8_t *d, size_t n)
{
    NSMutableDictionary *atoms = [NSMutableDictionary dictionary];
    if (!d || n < 86)
        return atoms;
    size_t at = 86;
    int depth = (int16_t)be16(d + 82);
    if ((int16_t)be16(d + 84) == 0 && depth >= 1 && depth <= 8 && n >= 94)
        at = 86 + 8 + ((size_t)be16(d + 86 + 6) + 1) * 8;
    while (at + 8 <= n) {
        uint32_t sz = be32(d + at);
        if (sz < 8 || at + sz > n)
            break;
        char type[5] = { (char)d[at + 4], (char)d[at + 5], (char)d[at + 6], (char)d[at + 7], 0 };
        NSString *key = [NSString stringWithCString:type encoding:NSMacOSRomanStringEncoding];
        if (key && sz > 8)
            atoms[key] = [NSData dataWithBytes:d + at + 8 length:sz - 8];
        at += sz;
    }
    return atoms;
}

static void vt_output(void *ref, void *frame_ref, OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef image,
                      CMTime pts, CMTime dur)
{
    hostvideo_dec *d = ref;
    if (status != noErr || !image)
        return;
    CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    const uint8_t *base = CVPixelBufferGetBaseAddress(image);
    size_t rb = CVPixelBufferGetBytesPerRow(image), iw = CVPixelBufferGetWidth(image),
           ih = CVPixelBufferGetHeight(image);
    for (int y = 0; y < d->h; y++) {
        uint8_t *o = d->out + (size_t)y * (size_t)d->w * 4;
        if ((size_t)y >= ih) {
            memset(o, 0, (size_t)d->w * 4);
            continue;
        }
        const uint8_t *s = base + (size_t)y * rb;
        for (int x = 0; x < d->w; x++, o += 4) {
            if ((size_t)x >= iw) {
                o[0] = o[1] = o[2] = 0;
            } else { /* BGRA */
                o[0] = s[x * 4 + 2];
                o[1] = s[x * 4 + 1];
                o[2] = s[x * 4];
            }
            o[3] = 255;
        }
    }
    CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    d->got = true;
}

static bool vt_make(hostvideo_dec *d, const uint8_t *desc, size_t desc_len)
{
    NSDictionary *atoms = extension_atoms(desc, desc_len);
    NSDictionary *ext = atoms.count ? @{ (__bridge NSString *)kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms : atoms }
                                    : nil;
    if (CMVideoFormatDescriptionCreate(NULL, vt_type(d->format), d->w, d->h, (__bridge CFDictionaryRef)ext,
                                       &d->desc) != noErr)
        return false;
    NSDictionary *attrs = @{
        (__bridge NSString *)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
        (__bridge NSString *)kCVPixelBufferWidthKey : @(d->w),
        (__bridge NSString *)kCVPixelBufferHeightKey : @(d->h),
    };
    VTDecompressionOutputCallbackRecord cb = { vt_output, d };
    return VTDecompressionSessionCreate(NULL, d->desc, NULL, (__bridge CFDictionaryRef)attrs, &cb, &d->session) ==
           noErr;
}

static bool vt_kind(uint32_t f)
{
    return f == AVC1 || f == HVC1 || f == HEV1 || f == MP4V || f == H263 || f == S263 || f == DVC || f == DVCP ||
           f == MP1V || f == MP2V;
}

bool hostvideo_supported(uint32_t format)
{
    if (image_kind(format))
        return format != JPEG; /* Photo-JPEG is ours (qt_codec.c), on the image decoder */
    if (!vt_kind(format))
        return false;
    /* Whether the system has the decoder: H.264 and HEVC need their
     * parameter sets to start a session, and every Mac has them; the rest
     * are tried once. */
    static uint32_t tried[16], answer[16];
    static int ntried;
    if (format == AVC1 || format == HVC1 || format == HEV1)
        return true;
    for (int i = 0; i < ntried; i++)
        if (tried[i] == format)
            return answer[i];
    hostvideo_dec probe = { format, 320, 240 };
    bool ok = vt_make(&probe, NULL, 0);
    if (probe.session) {
        VTDecompressionSessionInvalidate(probe.session);
        CFRelease(probe.session);
    }
    if (probe.desc)
        CFRelease(probe.desc);
    if (ntried < 16) {
        tried[ntried] = format;
        answer[ntried++] = ok;
    }
    return ok;
}

hostvideo_dec *hostvideo_open(uint32_t format, int width, int height, const uint8_t *desc, size_t desc_len)
{
    if (!hostvideo_supported(format) || width <= 0 || height <= 0)
        return NULL;
    hostvideo_dec *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->format = format;
    d->w = width;
    d->h = height;
    if (image_kind(format))
        return d;
    if (!vt_make(d, desc, desc_len)) {
        hostvideo_close(d);
        return NULL;
    }
    return d;
}

bool hostvideo_decode(hostvideo_dec *d, const uint8_t *data, size_t len, uint8_t *frame)
{
    if (!d || !data || !len)
        return false;
    if (d->format == MJPA)
        return mjpa_decode(data, len, frame, d->w, d->h);
    if (image_kind(d->format))
        return image_rgba(data, len, frame, d->w, d->h, 1, 0);
    CMBlockBufferRef block = NULL;
    CMSampleBufferRef sample = NULL;
    void *copy = malloc(len);
    if (!copy)
        return false;
    memcpy(copy, data, len);
    if (CMBlockBufferCreateWithMemoryBlock(NULL, copy, len, kCFAllocatorMalloc, NULL, 0, len, 0, &block) != noErr) {
        free(copy);
        return false;
    }
    size_t size = len;
    bool ok = CMSampleBufferCreateReady(NULL, block, d->desc, 1, 0, NULL, 1, &size, &sample) == noErr;
    if (ok) {
        d->out = frame;
        d->got = false;
        VTDecodeInfoFlags info = 0;
        ok = VTDecompressionSessionDecodeFrame(d->session, sample, 0, NULL, &info) == noErr;
        VTDecompressionSessionWaitForAsynchronousFrames(d->session);
        ok = ok && d->got;
        d->out = NULL;
        CFRelease(sample);
    }
    CFRelease(block);
    return ok;
}

void hostvideo_close(hostvideo_dec *d)
{
    if (!d)
        return;
    if (d->session) {
        VTDecompressionSessionInvalidate(d->session);
        CFRelease(d->session);
    }
    if (d->desc)
        CFRelease(d->desc);
    free(d);
}

/* ---- Still images out (ImageIO) ---- */

static CFStringRef image_uti(uint32_t t)
{
    switch (t) {
    case FOUR('J', 'P', 'E', 'G'): return CFSTR("public.jpeg");
    case FOUR('P', 'N', 'G', 'f'): return CFSTR("public.png");
    case FOUR('T', 'I', 'F', 'F'): return CFSTR("public.tiff");
    case FOUR('B', 'M', 'P', 'f'): return CFSTR("com.microsoft.bmp");
    case FOUR('G', 'I', 'F', 'f'): return CFSTR("com.compuserve.gif");
    case FOUR('8', 'B', 'P', 'S'): return CFSTR("com.adobe.photoshop-image");
    case FOUR('T', 'P', 'I', 'C'): return CFSTR("com.truevision.tga-image");
    case FOUR('S', 'G', 'I', ' '): return CFSTR("com.sgi.sgi-image");
    case FOUR('j', 'p', '2', ' '): return CFSTR("public.jpeg-2000");
    }
    return NULL;
}

bool hostimage_can_encode(uint32_t file_type)
{
    CFStringRef uti = image_uti(file_type);
    if (!uti)
        return false;
    static CFArrayRef types;
    if (!types)
        types = CGImageDestinationCopyTypeIdentifiers();
    return types && CFArrayContainsValue(types, CFRangeMake(0, CFArrayGetCount(types)), uti);
}

bool hostimage_encode(uint32_t file_type, const uint8_t *rgba, int w, int h, double quality, double dpi, uint8_t **out,
                      size_t *len)
{
    *out = NULL;
    *len = 0;
    if (!hostimage_can_encode(file_type) || w <= 0 || h <= 0)
        return false;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CFDataRef pixels = CFDataCreate(NULL, rgba, (CFIndex)w * h * 4);
    CGDataProviderRef prov = CGDataProviderCreateWithCFData(pixels);
    CGImageRef img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, cs,
                                   kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big, prov, NULL, false,
                                   kCGRenderingIntentDefault);
    CFMutableDataRef data = CFDataCreateMutable(NULL, 0);
    CGImageDestinationRef dst = CGImageDestinationCreateWithData(data, image_uti(file_type), 1, NULL);
    bool ok = false;
    if (dst && img) {
        NSDictionary *props = @{ (__bridge NSString *)kCGImageDestinationLossyCompressionQuality : @(quality),
                                 (__bridge NSString *)kCGImagePropertyDPIWidth : @(dpi),
                                 (__bridge NSString *)kCGImagePropertyDPIHeight : @(dpi) };
        CGImageDestinationAddImage(dst, img, (__bridge CFDictionaryRef)props);
        ok = CGImageDestinationFinalize(dst);
    }
    if (ok) {
        *len = (size_t)CFDataGetLength(data);
        *out = malloc(*len ? *len : 1);
        if (*out)
            memcpy(*out, CFDataGetBytePtr(data), *len);
        ok = *out != NULL;
    }
    if (dst)
        CFRelease(dst);
    CFRelease(data);
    if (img)
        CGImageRelease(img);
    CGDataProviderRelease(prov);
    CFRelease(pixels);
    CGColorSpaceRelease(cs);
    return ok;
}

/* ---- Sound (AudioToolbox) ---- */

struct hostaudio_dec {
    AudioConverterRef conv;
    AudioStreamBasicDescription in, out;
    /* the packets being fed */
    const uint8_t *data;
    AudioStreamPacketDescription *pkts;
    int npkts, next;
};

static AudioFormatID audio_id(uint32_t f)
{
    switch (f) {
    case FOUR('.', 'm', 'p', '3'): case FOUR('m', 's', 0, 0x55): return kAudioFormatMPEGLayer3;
    case FOUR('.', 'm', 'p', '2'): return kAudioFormatMPEGLayer2;
    case FOUR('.', 'm', 'p', '1'): return kAudioFormatMPEGLayer1;
    case FOUR('m', 'p', '4', 'a'): return kAudioFormatMPEG4AAC;
    case FOUR('a', 'l', 'a', 'c'): return kAudioFormatAppleLossless;
    case FOUR('Q', 'D', 'M', 'C'): return kAudioFormatQDesign;
    case FOUR('Q', 'D', 'M', '2'): return kAudioFormatQDesign2;
    case FOUR('Q', 'c', 'l', 'p'): return kAudioFormatQUALCOMM;
    case FOUR('s', 'a', 'm', 'r'): return kAudioFormatAMR;
    }
    return 0;
}

static uint32_t frames_per_packet(AudioFormatID id)
{
    switch (id) {
    case kAudioFormatMPEGLayer1: return 384;
    case kAudioFormatMPEGLayer2: case kAudioFormatMPEGLayer3: return 1152;
    case kAudioFormatMPEG4AAC: return 1024;
    case kAudioFormatAppleLossless: return 4096;
    }
    return 0;
}

static bool make_converter(hostaudio_dec *d, AudioFormatID id, int channels, double rate, const uint8_t *cookie,
                           size_t cookie_len)
{
    d->in = (AudioStreamBasicDescription){ .mSampleRate = rate, .mFormatID = id,
                                           .mFramesPerPacket = frames_per_packet(id),
                                           .mChannelsPerFrame = (UInt32)channels };
    if (cookie && cookie_len) { /* the format itself from the cookie, where it says */
        UInt32 n = sizeof d->in;
        AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, (UInt32)cookie_len, cookie, &n, &d->in);
        if (!d->in.mSampleRate)
            d->in.mSampleRate = rate;
        if (!d->in.mChannelsPerFrame)
            d->in.mChannelsPerFrame = (UInt32)channels;
    }
    d->out = (AudioStreamBasicDescription){ .mSampleRate = d->in.mSampleRate, .mFormatID = kAudioFormatLinearPCM,
                                            .mFormatFlags = kAudioFormatFlagIsSignedInteger |
                                                            kAudioFormatFlagIsPacked,
                                            .mBytesPerPacket = 2 * (UInt32)channels, .mFramesPerPacket = 1,
                                            .mBytesPerFrame = 2 * (UInt32)channels,
                                            .mChannelsPerFrame = (UInt32)channels, .mBitsPerChannel = 16 };
    if (AudioConverterNew(&d->in, &d->out, &d->conv) != noErr)
        return false;
    if (cookie && cookie_len)
        AudioConverterSetProperty(d->conv, kAudioConverterDecompressionMagicCookie, (UInt32)cookie_len, cookie);
    return true;
}

bool hostaudio_supported(uint32_t format)
{
    AudioFormatID id = audio_id(format);
    if (!id)
        return false;
    static uint32_t tried[16], answer[16];
    static int ntried;
    for (int i = 0; i < ntried; i++)
        if (tried[i] == format)
            return answer[i];
    hostaudio_dec probe = { 0 };
    bool ok = make_converter(&probe, id, 2, 44100, NULL, 0);
    if (probe.conv)
        AudioConverterDispose(probe.conv);
    if (ntried < 16) {
        tried[ntried] = format;
        answer[ntried++] = ok;
    }
    return ok;
}

hostaudio_dec *hostaudio_open(uint32_t format, int channels, double rate, const uint8_t *cookie, size_t cookie_len)
{
    AudioFormatID id = audio_id(format);
    if (!id || channels < 1 || channels > 2)
        return NULL;
    hostaudio_dec *d = calloc(1, sizeof *d);
    if (d && !make_converter(d, id, channels, rate, cookie, cookie_len)) {
        hostaudio_close(d);
        return NULL;
    }
    return d;
}

static OSStatus feed(AudioConverterRef conv, UInt32 *count, AudioBufferList *io, AudioStreamPacketDescription **desc,
                     void *ref)
{
    hostaudio_dec *d = ref;
    if (d->next >= d->npkts) {
        *count = 0;
        return 1; /* no more for now */
    }
    AudioStreamPacketDescription *p = &d->pkts[d->next++];
    io->mNumberBuffers = 1;
    io->mBuffers[0].mData = (void *)(d->data + p->mStartOffset);
    io->mBuffers[0].mDataByteSize = p->mDataByteSize;
    io->mBuffers[0].mNumberChannels = d->in.mChannelsPerFrame;
    static AudioStreamPacketDescription one;
    one = (AudioStreamPacketDescription){ 0, 0, p->mDataByteSize };
    if (desc)
        *desc = &one;
    *count = 1;
    return noErr;
}

/* MPEG audio frames: their lengths from their headers. */
static int mpeg_packets(const uint8_t *data, size_t len, AudioStreamPacketDescription **out)
{
    static const int rates[3][4] = { { 44100, 48000, 32000, 0 }, { 22050, 24000, 16000, 0 }, { 11025, 12000, 8000, 0 } };
    static const int kbps[2][3][16] = {
        { { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0 },
          { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0 },
          { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 } },
        { { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0 },
          { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 },
          { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 } },
    };
    int cap = 64, n = 0;
    AudioStreamPacketDescription *p = malloc((size_t)cap * sizeof *p);
    size_t at = 0;
    while (p && at + 4 <= len) {
        const uint8_t *h = data + at;
        if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) {
            at++;
            continue;
        }
        int ver = (h[1] >> 3) & 3, layer = (h[1] >> 1) & 3, br = h[2] >> 4, sr = (h[2] >> 2) & 3, pad = (h[2] >> 1) & 1;
        if (ver == 1 || layer == 0 || br == 0 || br == 15 || sr == 3) {
            at++;
            continue;
        }
        int v = ver == 3 ? 0 : ver == 2 ? 1 : 2, l = 3 - layer; /* 0: layer I */
        int rate = rates[v][sr], bitrate = kbps[v ? 1 : 0][l][br] * 1000;
        size_t size = l == 0 ? (size_t)((12 * bitrate / rate + pad) * 4)
                             : (size_t)((l == 2 && v ? 72 : 144) * bitrate / rate + pad);
        if (size < 4 || at + size > len)
            break;
        if (n == cap) {
            cap *= 2;
            AudioStreamPacketDescription *q = realloc(p, (size_t)cap * sizeof *p);
            if (!q)
                break;
            p = q;
        }
        p[n++] = (AudioStreamPacketDescription){ (SInt64)at, 0, (UInt32)size };
        at += size;
    }
    *out = p;
    return n;
}

uint32_t hostaudio_decode(hostaudio_dec *d, const uint8_t *data, size_t len, const uint32_t *sizes, int npackets,
                          int16_t **pcm)
{
    *pcm = NULL;
    if (!d || !data || !len)
        return 0;
    AudioStreamPacketDescription *pk = NULL;
    int n = 0;
    if (sizes) {
        pk = malloc((size_t)(npackets > 0 ? npackets : 1) * sizeof *pk);
        size_t at = 0;
        for (int i = 0; pk && i < npackets && at + sizes[i] <= len; i++) {
            pk[n++] = (AudioStreamPacketDescription){ (SInt64)at, 0, sizes[i] };
            at += sizes[i];
        }
    } else if (d->in.mFormatID == kAudioFormatMPEGLayer1 || d->in.mFormatID == kAudioFormatMPEGLayer2 ||
               d->in.mFormatID == kAudioFormatMPEGLayer3) {
        n = mpeg_packets(data, len, &pk);
    } else {
        pk = malloc(sizeof *pk);
        if (pk)
            pk[n++] = (AudioStreamPacketDescription){ 0, 0, (UInt32)len };
    }
    if (!pk || !n) {
        free(pk);
        return 0;
    }
    uint32_t per = d->in.mFramesPerPacket ? d->in.mFramesPerPacket : 4096;
    size_t cap = (size_t)n * per + 4096, ch = d->out.mChannelsPerFrame;
    int16_t *out = malloc(cap * ch * sizeof *out);
    d->data = data;
    d->pkts = pk;
    d->npkts = n;
    d->next = 0;
    AudioConverterReset(d->conv);
    size_t got = 0;
    while (out && got < cap) {
        UInt32 frames = (UInt32)(cap - got);
        AudioBufferList list = { 1, { { (UInt32)ch, (UInt32)(frames * ch * 2), out + got * ch } } };
        OSStatus st = AudioConverterFillComplexBuffer(d->conv, feed, d, &frames, &list, NULL);
        got += frames;
        if (st != noErr || !frames)
            break;
    }
    free(pk);
    if (!got) {
        free(out);
        return 0;
    }
    *pcm = out;
    return (uint32_t)got;
}

void hostaudio_close(hostaudio_dec *d)
{
    if (!d)
        return;
    if (d->conv)
        AudioConverterDispose(d->conv);
    free(d);
}
#endif
