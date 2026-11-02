/*
 * The screen's video driver (.Display_Video_Apple_BlueBox): the main
 * device's gdRefNum and its slot resource's, answering the Control and
 * Status calls of Designing Cards and Drivers and Video.h.  Programs call
 * it directly to switch depth (cscSetMode, cscSwitchMode), to fade the
 * screen through the gamma table (cscGetGamma, cscSetGamma), to load the
 * colour table (cscSetEntries, cscDirectSetEntries) and to learn what the
 * display can do (cscGetNextResolution, cscGetVideoParameters,
 * cscGetModeTiming).  Each call's csParam holds a pointer to its record.
 *
 * One resolution, the screen's, at the depths it can have (modes $80 to
 * $85 for 1 to 32 bits) and one page.  The hardware colour table and the
 * gamma table are the screen's (qd_screen.c): QuickDraw's SetEntries
 * changes the device's table and loads the hardware's; a program calling
 * the driver changes only the hardware's.
 */
#include <string.h>

#include "qd_internal.h"
#include "tb_internal.h"

enum { controlErr = -17, statusErr = -18 };
enum {
    cscReset = 0, cscKillIO = 1, cscSetMode = 2, cscSetEntries = 3, cscSetGamma = 4, cscGrayPage = 5, cscSetGray = 6,
    cscSetInterrupt = 7, cscDirectSetEntries = 8, cscSetDefaultMode = 9, cscSwitchMode = 10, cscSetSync = 11,
    cscSavePreferredConfiguration = 16, cscSetHardwareCursor = 22, cscDrawHardwareCursor = 23,
    cscSetConvolution = 24, cscSetPowerState = 25, cscPrivateControlCall = 26, cscSetMultiConnect = 28,
    cscSetClutBehavior = 29, cscSetDetailedTiming = 31,
};
enum {
    cscGetMode = 2, cscGetEntries = 3, cscGetPageCnt = 4, cscGetPageBase = 5, cscGetGray = 6, cscGetInterrupt = 7,
    cscGetGamma = 8, cscGetDefaultMode = 9, cscGetCurMode = 10, cscGetSync = 11, cscGetConnection = 12,
    cscGetModeTiming = 13, cscGetModeBaseAddress = 14, cscGetScanProc = 15, cscGetPreferredConfiguration = 16,
    cscGetNextResolution = 17, cscGetVideoParameters = 18, cscGetGammaInfoList = 20, cscRetrieveGammaTable = 21,
    cscSupportsHardwareCursor = 22, cscGetHardwareCursorDrawState = 23, cscGetConvolution = 24,
    cscGetPowerState = 25, cscPrivateStatusCall = 26, cscGetDDCBlock = 27, cscGetMultiConnect = 28,
    cscGetClutBehavior = 29, cscGetTimingRanges = 30, cscGetDetailedTiming = 31,
};
/* The one resolution's DisplayModeID, and the iterator's markers. */
enum { MODE_ID = 0x80 };
#define kDisplayModeIDCurrent 0u
#define kDisplayModeIDFindFirstResolution 0xFFFFFFFEu
#define kDisplayModeIDNoMoreResolutions 0xFFFFFFFDu
#define kGammaTableIDFindFirst 0xFFFFFFFEu
#define kGammaTableIDNoMoreTables 0xFFFFFFFDu
enum { kDeclROMtables = 0x6465636C /* 'decl' */, timingApple_FixedRateLCD = 42, timingVESA_1024x768_60hz = 190 };
enum { kModeValid = 1, kModeSafe = 2, kModeDefault = 4 };
enum { kGenericCRT = 19 };
enum { kAllModesValid = 1, kAllModesSafe = 2 };
enum { kAVPowerOn = 3 };
enum { clutType = 0, directType = 2 };

static struct {
    bool gray;          /* cscSetGray: colours loaded as their luminance */
    bool vbl_off;       /* cscSetInterrupt */
    uint8_t default_mode, sync;
    uint32_t power, power_flags, clut_behavior;
    uint16_t preferred_mode;
    uint32_t preferred_id;
    gaddr gamma;        /* the gamma table in use, the driver's own copy */
    uint32_t gamma_size;
} vd;

static const int depths[] = { 1, 2, 4, 8, 16, 32 };

static int mode_of(int depth)
{
    for (int i = 0; i < 6; i++)
        if (depths[i] == depth)
            return 0x80 + i;
    return 0x80;
}

static int depth_of(int mode) { return mode >= 0x80 && mode <= 0x85 ? depths[mode - 0x80] : 0; }

static bool mode_ok(int mode) { return depth_of(mode) && qd_screen_supports(depth_of(mode)); }

static gaddr param(gaddr pb) { return rd32(pb + CntrlParam_csParam_off); }

static gaddr screen_base(void) { return rd32(LM_ScrnBase); }

/* The depth changes as SetDepth would change it: the device and the ports
 * on the screen follow. */
static int16_t switch_mode(int mode)
{
    if (!mode_ok(mode))
        return paramErr;
    gaddr gdh = rd32(LM_MainDevice);
    return tb_SetDepth(gdh, (int16_t)mode, 0, 0) == noErr ? noErr : paramErr;
}

/* A gamma table (GammaTbl): version, type, formula size, channels, entries
 * a channel and their width in bits, the formula, then the data.  Applied
 * to the screen as 256 steps a channel; the driver keeps its own copy for
 * cscGetGamma. */
static uint32_t gamma_size(gaddr t)
{
    uint32_t chans = rd16(t + 6), count = rd16(t + 8), width = rd16(t + 10);
    return 12 + rd16(t + 4) + chans * count * ((width + 7) / 8);
}

static gaddr linear_gamma(void)
{
    gaddr t = mm_new_ptr_in(rd32(LM_SysZone), 12 + 256, true);
    if (!t)
        return 0;
    wr16(t + 6, 1);   /* gChanCnt */
    wr16(t + 8, 256); /* gDataCnt */
    wr16(t + 10, 8);  /* gDataWidth */
    for (int i = 0; i < 256; i++)
        wr8(t + 12 + (uint32_t)i, (uint8_t)i);
    return t;
}

static int16_t set_gamma(gaddr t)
{
    gaddr own = t ? 0 : linear_gamma();
    if (!t)
        t = own;
    if (!t)
        return paramErr;
    int chans = rd16(t + 6), count = rd16(t + 8), width = rd16(t + 10);
    if ((chans != 1 && chans != 3) || count < 2 || count > 1024 || width < 1 || width > 16) {
        if (own)
            tb_DisposePtr(own);
        return paramErr;
    }
    uint8_t lut[3][256];
    gaddr data = t + 12 + rd16(t + 4);
    uint32_t bytes = (uint32_t)(width + 7) / 8;
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < 256; i++) {
            int e = i * (count - 1) / 255;
            gaddr p = data + ((uint32_t)(chans == 3 ? k : 0) * (uint32_t)count + (uint32_t)e) * bytes;
            uint32_t v = bytes == 1 ? rd8(p) : rd16(p);
            lut[k][i] = (uint8_t)(width >= 8 ? v >> (width - 8) : v * 255 / ((1u << width) - 1));
        }
    qd_screen_set_gamma(lut);
    /* Ours to keep, for cscGetGamma (the program's may go away). */
    uint32_t size = gamma_size(t);
    gaddr copy = own ? own : mm_new_ptr_in(rd32(LM_SysZone), (int32_t)size, false);
    if (copy && !own)
        gmemcpy(copy, t, size);
    if (vd.gamma && vd.gamma != copy)
        tb_DisposePtr(vd.gamma);
    vd.gamma = copy;
    vd.gamma_size = size;
    return noErr;
}

/* cscSetEntries and cscDirectSetEntries (VDSetEntryRecord): ColorSpecs
 * loaded in order from csStart, or each at its value when csStart is -1;
 * csCount is one less than the number.  Only the hardware changes. */
static int16_t set_entries(gaddr rec, bool direct)
{
    if (!rec)
        return paramErr;
    int depth = qd_screen_depth();
    if (direct ? depth < 16 : depth > 8)
        return controlErr;
    gaddr table = rd32(rec);
    int start = (int16_t)rd16(rec + 4), count = (int16_t)rd16(rec + 6) + 1;
    int n = depth == 16 ? 32 : depth == 32 ? 256 : 1 << depth;
    if (!table || count < 0)
        return paramErr;
    for (int i = 0; i < count; i++) {
        gaddr spec = table + (uint32_t)i * 8;
        int index = start < 0 ? (int16_t)rd16(spec) : start + i;
        if (index < 0 || index >= n)
            return paramErr;
        uint8_t rgb[3] = { (uint8_t)(rd16(spec + 2) >> 8), (uint8_t)(rd16(spec + 4) >> 8), (uint8_t)(rd16(spec + 6) >> 8) };
        qd_screen_set_hw_entry(index, rgb);
    }
    return noErr;
}

/* cscGrayPage: the page filled with the 50% gray dither of black and white. */
static void gray_page(void)
{
    int depth = qd_screen_depth(), w = g_screen_width, h = g_screen_height;
    uint32_t rb = qd_screen_row_bytes();
    gaddr base = screen_base();
    uint32_t black = depth >= 16 ? 0 : (1u << depth) - 1, white = depth == 32 ? 0xFFFFFF : depth == 16 ? 0x7FFF : 0;
    pixdst d = { base, rb, { 0, 0, (int16_t)h, (int16_t)w }, depth, 0 };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            put_px(&d, x, y, ((x ^ y) & 1) ? black : white);
}

/* A VPBlock for a depth: the screen's rectangle and row bytes, 72 dpi. */
static void put_vpblock(gaddr vp, int depth)
{
    gmemset(vp, 0, 42);
    wr32(vp, 0);                                                       /* vpBaseOffset */
    wr16(vp + 4, (uint16_t)((g_screen_width * depth + 15) / 16 * 2)); /* vpRowBytes */
    wr16(vp + 10, (uint16_t)g_screen_height);                         /* vpBounds.bottom */
    wr16(vp + 12, (uint16_t)g_screen_width);                          /* vpBounds.right */
    wr32(vp + 18, 72u << 16);                                         /* vpHRes */
    wr32(vp + 22, 72u << 16);                                         /* vpVRes */
    wr16(vp + 26, depth > 8 ? 16 : 0);                                /* vpPixelType */
    wr16(vp + 28, (uint16_t)depth);                                   /* vpPixelSize */
    wr16(vp + 30, depth > 8 ? 3 : 1);                                 /* vpCmpCount */
    wr16(vp + 32, (uint16_t)(depth == 16 ? 5 : depth == 32 ? 8 : depth));
}

static int max_mode(void)
{
    int m = 0x80;
    for (int i = 0; i < 6; i++)
        if (qd_screen_supports(depths[i]))
            m = 0x80 + i;
    return m;
}

static int16_t video_control(gaddr pb, bool async)
{
    gaddr rec = param(pb);
    switch ((int16_t)rd16(pb + CntrlParam_csCode_off)) {
    case cscReset: { /* the first mode, one bit; VDPageInfo back */
        int16_t e = switch_mode(0x80);
        if (e == noErr && rec) {
            wr16(rec, 0x80);
            wr16(rec + 6, 0);
            wr32(rec + 8, screen_base());
        }
        return e;
    }
    case cscKillIO: return noErr;
    case cscSetMode: { /* VDPageInfo: csMode, csPage in; csBaseAddr out */
        if (!rec || (int16_t)rd16(rec + 6) != 0)
            return paramErr;
        int16_t e = switch_mode((int16_t)rd16(rec));
        if (e == noErr)
            wr32(rec + 8, screen_base());
        return e;
    }
    case cscSetEntries: return set_entries(rec, false);
    case cscDirectSetEntries: return set_entries(rec, true);
    case cscSetGamma: return rec ? set_gamma(rd32(rec)) : paramErr;
    case cscGrayPage:
        if (rec && (int16_t)rd16(rec + 6) != 0)
            return paramErr;
        gray_page();
        return noErr;
    case cscSetGray:
        if (!rec)
            return paramErr;
        vd.gray = rd8(rec) != 0;
        qd_screen_set_gray_mode(vd.gray);
        return noErr;
    case cscSetInterrupt:
        if (!rec)
            return paramErr;
        vd.vbl_off = rd8(rec) != 0;
        return noErr;
    case cscSetDefaultMode:
        if (!rec)
            return paramErr;
        vd.default_mode = rd8(rec);
        return noErr;
    case cscSwitchMode: { /* VDSwitchInfoRec: csMode, csData (the DisplayModeID), csPage */
        if (!rec || (rd32(rec + 2) != MODE_ID && rd32(rec + 2) != kDisplayModeIDCurrent) || rd16(rec + 6) != 0)
            return paramErr;
        int16_t e = switch_mode(rd16(rec));
        if (e == noErr)
            wr32(rec + 8, screen_base());
        return e;
    }
    case cscSetSync:
        if (!rec)
            return paramErr;
        vd.sync = rd8(rec);
        return noErr;
    case cscSavePreferredConfiguration:
        if (!rec)
            return paramErr;
        vd.preferred_mode = rd16(rec);
        vd.preferred_id = rd32(rec + 2);
        return noErr;
    case cscSetPowerState:
        if (!rec)
            return paramErr;
        vd.power = rd32(rec);
        vd.power_flags = rd32(rec + 4);
        return noErr;
    case cscSetClutBehavior:
        if (!rec)
            return paramErr;
        vd.clut_behavior = rd32(rec);
        return noErr;
    /* No hardware cursor, convolution, other connections or programmable
     * timings: what a card without them answered. */
    case cscSetHardwareCursor: case cscDrawHardwareCursor: case cscSetConvolution:
    case cscPrivateControlCall: case cscSetMultiConnect: case cscSetDetailedTiming:
    default: return controlErr;
    }
}

static int16_t video_status(gaddr pb, bool async)
{
    gaddr rec = param(pb);
    int depth = qd_screen_depth(), mode = mode_of(depth);
    int16_t code = (int16_t)rd16(pb + CntrlParam_csCode_off);
    if (!rec && code != cscGetScanProc)
        return paramErr;
    switch (code) {
    case cscGetMode: /* VDPageInfo */
        wr16(rec, (uint16_t)mode);
        wr32(rec + 2, 0);
        wr16(rec + 6, 0);
        wr32(rec + 8, screen_base());
        return noErr;
    case cscGetEntries: { /* the hardware's colours, as cscSetEntries took them */
        gaddr table = rd32(rec);
        int start = (int16_t)rd16(rec + 4), count = (int16_t)rd16(rec + 6) + 1;
        int n = depth == 16 ? 32 : depth == 32 ? 256 : 1 << depth;
        if (!table || count < 0)
            return paramErr;
        for (int i = 0; i < count; i++) {
            gaddr spec = table + (uint32_t)i * 8;
            int index = start < 0 ? (int16_t)rd16(spec) : start + i;
            if (index < 0 || index >= n)
                return paramErr;
            uint8_t rgb[3];
            qd_screen_hw_entry(index, rgb);
            if (start >= 0)
                wr16(spec, (uint16_t)index);
            for (int k = 0; k < 3; k++)
                wr16(spec + 2 + (uint32_t)k * 2, (uint16_t)(rgb[k] * 0x101));
        }
        return noErr;
    }
    case cscGetPageCnt: /* VDPageInfo: csMode in, csPage out */
        if (!mode_ok((int16_t)rd16(rec)))
            return paramErr;
        wr16(rec + 6, 1);
        return noErr;
    case cscGetPageBase:
        if ((int16_t)rd16(rec + 6) != 0)
            return paramErr;
        wr32(rec + 8, screen_base());
        return noErr;
    case cscGetGray: wr8(rec, vd.gray ? 1 : 0); return noErr;
    case cscGetInterrupt: wr8(rec, vd.vbl_off ? 1 : 0); return noErr;
    case cscGetGamma:
        if (!vd.gamma)
            set_gamma(0);
        wr32(rec, vd.gamma);
        return noErr;
    case cscGetDefaultMode: wr8(rec, vd.default_mode); return noErr;
    case cscGetCurMode: /* VDSwitchInfoRec */
        wr16(rec, (uint16_t)mode);
        wr32(rec + 2, MODE_ID);
        wr16(rec + 6, 0);
        wr32(rec + 8, screen_base());
        wr32(rec + 12, 0);
        return noErr;
    case cscGetSync: wr8(rec, vd.sync); wr8(rec + 1, 0); return noErr;
    case cscGetConnection: /* VDDisplayConnectInfoRec */
        wr16(rec, kGenericCRT);
        wr8(rec + 2, 0);
        wr8(rec + 3, 0);
        wr32(rec + 4, kAllModesValid | kAllModesSafe);
        wr32(rec + 8, 0);
        wr32(rec + 12, 0);
        return noErr;
    case cscGetModeTiming: /* VDTimingInfoRec: csTimingMode in */
        if (rd32(rec) != MODE_ID && rd32(rec) != kDisplayModeIDCurrent)
            return paramErr;
        wr32(rec + 8, kDeclROMtables);
        wr32(rec + 12, g_screen_width == 1024 && g_screen_height == 768 ? timingVESA_1024x768_60hz
                                                                         : timingApple_FixedRateLCD);
        wr32(rec + 16, kModeValid | kModeSafe | kModeDefault);
        return noErr;
    case cscGetModeBaseAddress: /* VDBaseAddressInfoRec: csDevData (the mode) in */
        if (!mode_ok((int16_t)rd32(rec)))
            return paramErr;
        wr32(rec + 4, screen_base());
        wr16(rec + 8, 0);
        wr32(rec + 10, 0);
        return noErr;
    case cscGetPreferredConfiguration: /* VDSwitchInfoRec */
        wr16(rec, vd.preferred_mode ? vd.preferred_mode : (uint16_t)mode);
        wr32(rec + 2, vd.preferred_id ? vd.preferred_id : MODE_ID);
        wr16(rec + 6, 0);
        wr32(rec + 8, 0);
        wr32(rec + 12, 0);
        return noErr;
    case cscGetNextResolution: { /* VDResolutionInfoRec: csPreviousDisplayModeID in */
        uint32_t prev = rd32(rec);
        if (prev != kDisplayModeIDFindFirstResolution && prev != kDisplayModeIDCurrent && prev != MODE_ID)
            return paramErr;
        if (prev == MODE_ID) {
            wr32(rec + 4, kDisplayModeIDNoMoreResolutions);
            return noErr;
        }
        wr32(rec + 4, MODE_ID);
        wr32(rec + 8, (uint32_t)g_screen_width);
        wr32(rec + 12, (uint32_t)g_screen_height);
        wr32(rec + 16, 60u << 16);
        wr16(rec + 20, (uint16_t)max_mode());
        wr32(rec + 22, 0);
        wr32(rec + 26, 0);
        return noErr;
    }
    case cscGetVideoParameters: { /* VDVideoParametersInfoRec */
        uint32_t id = rd32(rec);
        int m = rd16(rec + 4);
        if ((id != MODE_ID && id != kDisplayModeIDCurrent) || !mode_ok(m))
            return paramErr;
        gaddr vp = rd32(rec + 6);
        if (vp)
            put_vpblock(vp, depth_of(m));
        wr32(rec + 10, 1);
        wr32(rec + 14, depth_of(m) > 8 ? directType : clutType);
        wr32(rec + 18, 0);
        return noErr;
    }
    case cscSupportsHardwareCursor: wr32(rec, 0); return noErr;
    case cscGetPowerState:
        wr32(rec, vd.power);
        wr32(rec + 4, vd.power_flags);
        wr32(rec + 8, 0);
        wr32(rec + 12, 0);
        return noErr;
    case cscGetClutBehavior: wr32(rec, vd.clut_behavior); return noErr;
    case cscGetGammaInfoList: /* the one table, the one in use */
        if (rd32(rec) != kGammaTableIDFindFirst && rd32(rec) != 1)
            return paramErr;
        if (!vd.gamma)
            set_gamma(0);
        wr32(rec + 4, rd32(rec) == 1 ? kGammaTableIDNoMoreTables : 1);
        wr32(rec + 8, vd.gamma_size);
        wr32(rec + 12, 0);
        return noErr;
    case cscRetrieveGammaTable: /* VDRetrieveGammaRec */
        if (rd32(rec) != 1 || !rd32(rec + 4))
            return paramErr;
        if (!vd.gamma)
            set_gamma(0);
        gmemcpy(rd32(rec + 4), vd.gamma, vd.gamma_size);
        return noErr;
    case cscGetScanProc: case cscGetHardwareCursorDrawState: case cscGetConvolution: case cscPrivateStatusCall:
    case cscGetDDCBlock: case cscGetMultiConnect: case cscGetTimingRanges: case cscGetDetailedTiming:
    default: return statusErr;
    }
}

static int16_t video_open(gaddr pb) { return noErr; }

void video_reset(void)
{
    memset(&vd, 0, sizeof vd);
    qd_screen_set_gray_mode(false);
    vd.default_mode = MODE_ID;
    vd.power = kAVPowerOn;
    uint8_t lut[3][256];
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < 256; i++)
            lut[k][i] = (uint8_t)i;
    qd_screen_set_gamma(lut);
}

const native_driver video_driver = { ".Display_Video_Apple_BlueBox", VIDEO_UNIT, video_open, NULL, video_control,
                                     video_status, NULL, NULL, NULL };
