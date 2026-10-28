/*
 * The Standard Image Compression dialog component ('scdi' 'imag'): the
 * settings a program compresses with, and the compressing itself through
 * the Image Compression Manager (qt_icm_compress.c).
 *
 * There is no dialog.  SCRequestImageSettings and SCRequestSequenceSettings
 * answer as if OK were clicked on the settings as they stand: those from
 * SCDefaultPixMapSettings and the like (Photo-JPEG for deep stills when the
 * host can write JPEG, Animation otherwise and for sequences), or SCSetInfo.
 *
 * Component calls arrive as ComponentParameters: flags, the arguments'
 * size, the selector, then the arguments last first, then the instance.
 * SCSpatialSettings: codecType 0, codec 4, depth 8, spatialQuality 10.
 * SCTemporalSettings: temporalQuality 0, frameRate 4, keyFrameRate 8.
 * SCDataRateSettings: dataRate 0, frameDuration 4, the minimum spatial and
 * temporal qualities 8 and 12.  SCParams (SCGetCompression): flags 0,
 * codec type 4, codec 8, spatial 12 and temporal quality 16, depth 20,
 * frameRate 22, keyFrameRate 26.
 */
#include <stdlib.h>
#include <string.h>

#include "m68k.h"
#include "qd_internal.h"
#include "qt_codec.h"
#include "tb_internal.h"
#include "trap.h"

enum {
    RLE = 0x726C6520, JPEG = 0x6A706567,
    codecNormalQuality = 0x200, codecFlagUpdatePrevious = 1 << 2,
    codecUnimpErr = -8962, badComponentInstance = (int32_t)0x80008001, badComponentSelector = (int32_t)0x80008002,
    scUserCancelled = 1,
};

#define MAX_SC 8

typedef struct {
    bool used;
    uint32_t ci;
    /* 'sptl' */
    uint32_t type;
    int32_t codec;
    int16_t depth;
    int32_t spatial;
    /* 'tprl' */
    int32_t temporal, frame_rate, key_rate;
    /* 'drat' */
    int32_t data_rate, frame_duration, min_spatial, min_temporal;
    gaddr clut;          /* 'clut' */
    uint8_t prog[8];     /* 'prog': ProgressProcRecord */
    uint8_t xprc[44];    /* 'xprc': SCExtendedProcs */
    int32_t prefs;       /* 'pref' */
    int32_t cflags;      /* 'cflg' */
    int32_t compress_flags; /* SCSetCompressFlags */
    int32_t seq;         /* 'sequ' */
    gaddr seq_desc, seq_data;
} sc;

static sc scs[MAX_SC];

static sc *sc_of(uint32_t ci)
{
    for (int i = 0; i < MAX_SC; i++)
        if (scs[i].used && scs[i].ci == ci)
            return &scs[i];
    return NULL;
}

static void defaults(sc *s, int depth, bool motion)
{
    bool jpeg = !motion && depth >= 16 && qt_venc_supported(JPEG, 24);
    s->type = jpeg ? JPEG : RLE;
    s->codec = 0;
    s->depth = (int16_t)(jpeg ? 24 : depth);
    s->spatial = codecNormalQuality;
    s->temporal = motion ? codecNormalQuality : 0;
    s->frame_rate = motion ? 15 << 16 : 0;
    s->key_rate = motion ? 15 : 0;
}

static void sequence_end(sc *s)
{
    if (s->seq)
        tb_CDSequenceEnd(s->seq);
    if (s->seq_desc)
        tb_DisposeHandle(s->seq_desc);
    if (s->seq_data)
        tb_DisposeHandle(s->seq_data);
    s->seq = 0;
    s->seq_desc = s->seq_data = 0;
}

/* The settings state ('ssta'): a tag, then the settings as SCGetInfo gives them. */
enum { STATE_SIZE = 4 + 14 + 12 + 16 + 4 + 4 };

static void put_spatial(sc *s, gaddr p)
{
    wr32(p, s->type);
    wr32(p + 4, (uint32_t)s->codec);
    wr16(p + 8, (uint16_t)s->depth);
    wr32(p + 10, (uint32_t)s->spatial);
}

static void get_spatial(sc *s, gaddr p)
{
    s->type = rd32(p);
    s->codec = (int32_t)rd32(p + 4);
    s->depth = (int16_t)rd16(p + 8);
    s->spatial = (int32_t)rd32(p + 10);
}

static void put_temporal(sc *s, gaddr p)
{
    wr32(p, (uint32_t)s->temporal);
    wr32(p + 4, (uint32_t)s->frame_rate);
    wr32(p + 8, (uint32_t)s->key_rate);
}

static void get_temporal(sc *s, gaddr p)
{
    s->temporal = (int32_t)rd32(p);
    s->frame_rate = (int32_t)rd32(p + 4);
    s->key_rate = (int32_t)rd32(p + 8);
}

static void put_rate(sc *s, gaddr p)
{
    wr32(p, (uint32_t)s->data_rate);
    wr32(p + 4, (uint32_t)s->frame_duration);
    wr32(p + 8, (uint32_t)s->min_spatial);
    wr32(p + 12, (uint32_t)s->min_temporal);
}

static void get_rate(sc *s, gaddr p)
{
    s->data_rate = (int32_t)rd32(p);
    s->frame_duration = (int32_t)rd32(p + 4);
    s->min_spatial = (int32_t)rd32(p + 8);
    s->min_temporal = (int32_t)rd32(p + 12);
}

static int32_t get_info(sc *s, uint32_t type, gaddr info)
{
    if (!info)
        return paramErr;
    switch (type) {
    case 0x7370746C: put_spatial(s, info); break;           /* 'sptl' */
    case 0x7470726C: put_temporal(s, info); break;          /* 'tprl' */
    case 0x64726174: put_rate(s, info); break;              /* 'drat' */
    case 0x636C7574: wr32(info, s->clut); break;            /* 'clut' */
    case 0x70726F67: gput(info, s->prog, 8); break;         /* 'prog' */
    case 0x78707263: gput(info, s->xprc, 44); break;        /* 'xprc' */
    case 0x70726566: wr32(info, (uint32_t)s->prefs); break; /* 'pref' */
    case 0x63666C67: wr32(info, (uint32_t)s->cflags); break; /* 'cflg' */
    case 0x73657175: wr32(info, (uint32_t)s->seq); break;   /* 'sequ' */
    case 0x73737461: {                                      /* 'ssta' */
        gaddr h = tb_NewHandle(STATE_SIZE, false, true);
        if (!h)
            return memFullErr;
        gaddr p = rd32(h);
        wr32(p, 0x42427363); /* 'BBsc' */
        put_spatial(s, p + 4);
        put_temporal(s, p + 18);
        put_rate(s, p + 30);
        wr32(p + 46, (uint32_t)s->prefs);
        wr32(p + 50, (uint32_t)s->cflags);
        wr32(info, h);
        break;
    }
    case 0x776E6477: break; /* 'wndw': the dialog's place, which there isn't */
    default: return paramErr;
    }
    return noErr;
}

static int32_t set_info(sc *s, uint32_t type, gaddr info)
{
    if (!info)
        return paramErr;
    switch (type) {
    case 0x7370746C: get_spatial(s, info); break;
    case 0x7470726C: get_temporal(s, info); break;
    case 0x64726174: get_rate(s, info); break;
    case 0x636C7574: s->clut = rd32(info); break;
    case 0x70726F67: gget(s->prog, info, 8); break;
    case 0x78707263: gget(s->xprc, info, 44); break;
    case 0x70726566: s->prefs = (int32_t)rd32(info); break;
    case 0x63666C67: s->cflags = (int32_t)rd32(info); break;
    case 0x73737461: {
        gaddr h = rd32(info);
        if (h && rd32(h) && tb_GetHandleSize(h) >= STATE_SIZE && rd32(rd32(h)) == 0x42427363) {
            gaddr p = rd32(h);
            get_spatial(s, p + 4);
            get_temporal(s, p + 18);
            get_rate(s, p + 30);
            s->prefs = (int32_t)rd32(p + 46);
            s->cflags = (int32_t)rd32(p + 50);
        }
        break;
    }
    case 0x776E6477: case 0x73657175: break;
    default: return paramErr;
    }
    return noErr;
}

/* SCCompressImage(src, srcRect, &desc, &data): new handles for the caller. */
static int32_t compress_image(sc *s, gaddr src, gaddr rect, gaddr descp, gaddr datap)
{
    if (!src || !descp || !datap)
        return paramErr;
    gaddr size = tb_NewPtr(4, false, true);
    int16_t err = tb_GetMaxCompressionSize((int32_t)src, rect, s->depth, s->spatial, (int32_t)s->type, s->codec, size);
    uint32_t max = rd32(size);
    tb_DisposePtr(size);
    if (err)
        return err;
    gaddr desc = tb_NewHandle(86, false, true), data = tb_NewHandle((int32_t)max, false, false);
    if (!desc || !data) {
        if (desc)
            tb_DisposeHandle(desc);
        if (data)
            tb_DisposeHandle(data);
        return memFullErr;
    }
    tb_HLock(data);
    err = tb_FCompressImage((int32_t)src, rect, s->depth, s->spatial, (int32_t)s->type, s->codec, (int32_t)s->clut,
                            0, 0, 0, 0, (int32_t)desc, (int32_t)rd32(data));
    tb_HUnlock(data);
    if (err) {
        tb_DisposeHandle(desc);
        tb_DisposeHandle(data);
        return err;
    }
    tb_SetHandleSize(data, (int32_t)rd32(rd32(desc) + 44));
    wr32(descp, desc);
    wr32(datap, data);
    return noErr;
}

static int32_t sequence_begin(sc *s, gaddr src, gaddr rect, gaddr descp)
{
    sequence_end(s);
    gaddr tmp = tb_NewPtr(8, false, true);
    s->seq_desc = tb_NewHandle(86, false, true);
    int16_t err = tb_CompressSequenceBegin(tmp, (int32_t)src, 0, rect, 0, s->depth, (int32_t)s->type, s->codec,
                                           s->spatial, s->temporal, s->key_rate, (int32_t)s->clut,
                                           codecFlagUpdatePrevious, (int32_t)s->seq_desc);
    s->seq = (int32_t)rd32(tmp);
    if (!err)
        err = tb_GetCSequenceMaxCompressionSize(s->seq, (int32_t)src, tmp + 4);
    if (!err && !(s->seq_data = tb_NewHandle((int32_t)rd32(tmp + 4), false, false)))
        err = memFullErr;
    tb_DisposePtr(tmp);
    if (err) {
        sequence_end(s);
        return err;
    }
    if (descp)
        wr32(descp, s->seq_desc);
    return noErr;
}

static int32_t sequence_frame(sc *s, gaddr src, gaddr rect, gaddr datap, gaddr sizep, gaddr notSync)
{
    if (!s->seq)
        return paramErr;
    gaddr tmp = tb_NewPtr(8, false, true);
    tb_HLock(s->seq_data);
    int16_t err = tb_CompressSequenceFrame(s->seq, (int32_t)src, rect, 0, (int32_t)rd32(s->seq_data), tmp, tmp + 4, 0);
    tb_HUnlock(s->seq_data);
    if (!err) {
        if (datap)
            wr32(datap, s->seq_data);
        if (sizep)
            wr32(sizep, rd32(tmp));
        if (notSync)
            wr16(notSync, rd8(tmp + 4) ? 1 : 0);
    }
    tb_DisposePtr(tmp);
    return err;
}

/* SCGetSettingsAsText: "Animation, Normal quality" and the like. */
static int32_t settings_text(sc *s, gaddr textp)
{
    if (!textp)
        return paramErr;
    gaddr info = tb_NewPtr(80, false, true);
    const char *name = "None";
    if (!tb_GetCodecInfo(info, (int32_t)s->type, 0)) {
        static char buf[40];
        uint8_t n = rd8(info);
        gget(buf, info + 1, n < 39 ? n : 39);
        buf[n < 39 ? n : 39] = 0;
        name = buf;
    }
    tb_DisposePtr(info);
    const char *q = s->spatial >= 0x400 ? "Lossless" : s->spatial >= 0x3FF ? "Best" : s->spatial >= 0x300 ? "High"
                    : s->spatial >= 0x200 ? "Normal" : s->spatial >= 0x100 ? "Low" : "Least";
    char text[96];
    size_t len = (size_t)snprintf(text, sizeof text, "%s, %s quality", name, q);
    gaddr h = tb_NewHandle((int32_t)len, false, false);
    if (!h)
        return memFullErr;
    gput(rd32(h), text, (uint32_t)len);
    wr32(textp, h);
    return noErr;
}

static int32_t sc_call(sc *s, int16_t what, gaddr a, uint32_t size)
{
    switch (what) {
    case 1: { /* SCGetCompression(Extended)(params, where, ...): the settings as SCParams */
        gaddr p = size >= 8 ? rd32(a + size - 4) : 0;
        if (!p)
            return paramErr;
        wr32(p + 4, s->type);
        wr32(p + 8, (uint32_t)s->codec);
        wr32(p + 12, (uint32_t)s->spatial);
        wr32(p + 16, (uint32_t)s->temporal);
        wr16(p + 20, (uint16_t)s->depth);
        wr32(p + 22, (uint32_t)s->frame_rate);
        wr32(p + 26, (uint32_t)s->key_rate);
        return noErr;
    }
    case 2: /* SCPositionRect(rp, where): centred on the screen */
        if (rd32(a + 4)) {
            gaddr rp = rd32(a + 4);
            qrect r = qrect_get(rp);
            int w = r.right - r.left, h = r.bottom - r.top;
            int16_t left = (int16_t)((g_screen_width - w) / 2), top = (int16_t)((g_screen_height - h) / 3);
            qrect_put(rp, (qrect){ top, left, (int16_t)(top + h), (int16_t)(left + w) });
            if (rd32(a))
                wr32(rd32(a), make_pt(top, left));
        }
        return noErr;
    case 3: case 4: case 5: case 6: /* SCPositionDialog, the test images */
        return noErr;
    case 7: /* SCGetBestDeviceRect(r) */
        if (rd32(a))
            qrect_put(rd32(a), (qrect){ 0, 0, (int16_t)g_screen_height, (int16_t)g_screen_width });
        return noErr;
    case 10: case 14: /* SCRequestImageSettings, SCRequestSequenceSettings: OK, as they are */
        return noErr;
    case 11: /* SCCompressImage(src, srcRect, &desc, &data) */
        return compress_image(s, rd32(a + 12), rd32(a + 8), rd32(a + 4), rd32(a));
    case 12: /* SCCompressPicture(src, dst) */
        return tb_FCompressPicture((int32_t)rd32(a + 4), (int32_t)rd32(a), s->depth, (int32_t)s->clut, s->spatial,
                                   0, 0, 0, (int32_t)s->type, s->codec);
    case 13: /* SCCompressPictureFile */
        return codecUnimpErr;
    case 15: /* SCCompressSequenceBegin(src, srcRect, &desc) */
        return sequence_begin(s, rd32(a + 8), rd32(a + 4), rd32(a));
    case 16: /* SCCompressSequenceFrame(src, srcRect, &data, &dataSize, &notSyncFlag) */
        return sequence_frame(s, rd32(a + 16), rd32(a + 12), rd32(a + 8), rd32(a + 4), rd32(a));
    case 17: /* SCCompressSequenceEnd */
        sequence_end(s);
        return noErr;
    case 18: case 19: /* SCDefaultPictHandleSettings, SCDefaultPictFileSettings(src, motion) */
        defaults(s, 32, rd16(a) != 0);
        return noErr;
    case 20: { /* SCDefaultPixMapSettings(src, motion) */
        gaddr pmh = rd32(a + 2);
        int depth = pmh && rd32(pmh) && (rd16(rd32(pmh) + 4) & 0x8000) ? PixMap_pixelSize(rd32(pmh)) : 1;
        defaults(s, depth, rd16(a) != 0);
        if (depth <= 8 && pmh && rd32(pmh))
            s->clut = 0; /* the pixel map's own colours */
        return noErr;
    }
    case 21: return get_info(s, rd32(a + 4), rd32(a)); /* SCGetInfo(type, info) */
    case 22: return set_info(s, rd32(a + 4), rd32(a)); /* SCSetInfo(type, info) */
    case 23: { /* SCNewGWorld(&gw, rect, flags) */
        int16_t depth = s->depth == 24 || s->depth == 0 ? 32 : s->depth > 32 ? 8 : s->depth;
        return tb_NewGWorld(rd32(a + 8), depth, rd32(a + 4), s->clut, 0, (int32_t)rd32(a));
    }
    case 24: s->compress_flags = (int32_t)rd32(a); return noErr; /* SCSetCompressFlags */
    case 25: if (rd32(a)) wr32(rd32(a), (uint32_t)s->compress_flags); return noErr;
    case 26: return settings_text(s, rd32(a)); /* SCGetSettingsAsText(&text) */
    default: return badComponentSelector;
    }
}

/* pascal ComponentResult (ComponentParameters *params, Handle storage) */
static void sc_component(cpu68k *cpu, uint16_t tw)
{
    (void)tw;
    uint32_t ret = cpu_pop32(cpu);
    cpu_pop32(cpu); /* storage */
    gaddr params = cpu_pop32(cpu);
    int16_t what = (int16_t)rd16(params + 2);
    uint32_t size = rd8(params + 1);
    gaddr a = params + 4;
    int32_t r = noErr;
    if (what == -1) { /* open: the argument is the instance */
        r = memFullErr;
        for (int i = 0; i < MAX_SC; i++)
            if (!scs[i].used) {
                memset(&scs[i], 0, sizeof scs[i]);
                scs[i].used = true;
                scs[i].ci = rd32(a);
                defaults(&scs[i], 32, false);
                r = noErr;
                break;
            }
    } else if (what == -2) { /* close */
        sc *s = sc_of(rd32(a));
        if (s) {
            sequence_end(s);
            s->used = false;
        }
    } else if (what == -3) { /* can do */
        int16_t sel = (int16_t)rd16(a);
        r = (sel >= -4 && sel <= -1) || (sel >= 1 && sel <= 7) || (sel >= 10 && sel <= 26);
    } else if (what == -4) { /* version */
        r = 0x00030000;
    } else if (what < 0) {
        r = badComponentSelector;
    } else {
        sc *s = sc_of(rd32(a + size));
        r = s ? sc_call(s, what, a, size) : badComponentInstance;
    }
    wr32(cpu->a[7], (uint32_t)r);
    cpu_push32(cpu, ret);
}

void icm_components_reset(void)
{
    memset(scs, 0, sizeof scs);
    /* The natives are registered afresh each boot (trap_init). */
    gaddr entry = native_register(sc_component, "StandardCompression", 0);
    components_register_native(fourcc("scdi"), fourcc("imag"), fourcc("appl"), entry);
    qt_codec_components_register(); /* the codecs (qt_codec_comp.c) */
}
