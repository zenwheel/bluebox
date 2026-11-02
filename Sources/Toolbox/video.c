/*
 * The screen's video driver (.Display_Video_Apple_BlueBox), the main
 * device's gdRefNum, with the Control and Status calls of Designing Cards
 * and Drivers and the Display Manager's (Video.h): modes and pages, the
 * colour table, gamma, gray, retrace interrupts, sync and power, the
 * connection, timing and resolution queries.  It drives the one screen of
 * qd_screen.c: a mode is a depth there (128 for 1 bit up to 133 for 32,
 * one page), the colour table the colours the screen is shown in, and a
 * gamma table ('gama' format), the direct modes' colour table, gray and
 * a display switched off change the colours the host shows, the pixels
 * staying as they are: games fade the screen through the gamma table.
 */
#include <string.h>

#include "qd_internal.h"
#include "tb_internal.h"

enum {
    controlErr = -17, statusErr = -18,
    /* Control */
    cscReset = 0, cscKillIO = 1, cscSetMode = 2, cscSetEntries = 3, cscSetGamma = 4, cscGrayPage = 5,
    cscSetGray = 6, cscSetInterrupt = 7, cscDirectSetEntries = 8, cscSetDefaultMode = 9, cscSwitchMode = 10,
    cscSetSync = 11, cscSavePreferredConfiguration = 16, cscSetHardwareCursor = 22, cscDrawHardwareCursor = 23,
    cscSetConvolution = 24, cscSetPowerState = 25, cscPrivateControlCall = 26, cscSetMultiConnect = 28,
    cscSetClutBehavior = 29, cscSetDetailedTiming = 31, cscDoCommunication = 33, cscProbeConnection = 34,
    /* Status (beside those numbered as their Control calls) */
    cscGetMode = 2, cscGetEntries = 3, cscGetPageCnt = 4, cscGetPageBase = 5, cscGetGray = 6,
    cscGetInterrupt = 7, cscGetGamma = 8, cscGetDefaultMode = 9, cscGetCurMode = 10, cscGetSync = 11,
    cscGetConnection = 12, cscGetModeTiming = 13, cscGetModeBaseAddress = 14, cscGetScanProc = 15,
    cscGetPreferredConfiguration = 16, cscGetNextResolution = 17, cscGetVideoParameters = 18,
    cscGetGammaInfoList = 20, cscRetrieveGammaTable = 21, cscSupportsHardwareCursor = 22,
    cscGetHardwareCursorDrawState = 23, cscGetConvolution = 24, cscGetPowerState = 25,
    cscPrivateStatusCall = 26, cscGetDDCBlock = 27, cscGetMultiConnect = 28, cscGetClutBehavior = 29,
    cscGetTimingRanges = 30, cscGetDetailedTiming = 31, cscGetCommunicationInfo = 32,
};

/* The one resolution (the screen's), as a decl ROM driver numbers it: its
 * sResource in the slot ROM (slots.c). */
enum { MODE_ID = 0x80, FIRST_DEPTH_MODE = 0x80 };
enum {
    kDisplayModeIDCurrent = 0, kDisplayModeIDFindFirstResolution = 0xFFFFFFFE,
    kDisplayModeIDNoMoreResolutions = 0xFFFFFFFD,
};
enum { kGammaTableIDFindFirst = -2, kGammaTableIDNoMoreTables = -1, kGammaTableIDSpecific = 0, LINEAR_GAMMA_ID = 1 };
enum { kGenericLCD = 20 };
enum { kAllModesValid = 0, kAllModesSafe = 1, kBuiltInConnection = 11 };
enum { kModeValid = 0, kModeSafe = 1, kModeDefault = 2, kModeShowNow = 3 };
enum { timingApple_FixedRateLCD = 42 };
enum { kAVPowerOff = 0, kAVPowerStandby = 1, kAVPowerSuspend = 2, kAVPowerOn = 3 };
enum { clutType = 0, directType = 2 };

/* The gamma table: the driver's copy (cscGetGamma hands out a pointer to
 * it), in the system heap. */
enum { GAMMA_HEADER = 12, MAX_GAMMA = GAMMA_HEADER + 1024 + 3 * 4096 * 2 };
static gaddr gamma_table;

static bool gray_mode, vbl_disabled;
static uint8_t default_mode = MODE_ID, sync_state;
static uint16_t preferred_depth_mode;
static uint32_t power_state = kAVPowerOn, clut_behavior;
/* The direct modes' colour table: what each component value is shown as. */
static uint8_t direct_lut[3][256];
/* The colours shown: a component through the direct table and the gamma
 * table (or black, with the display off); identity when nothing changes
 * them. */
static uint8_t out_lut[3][256];
/* A fade (DrawSprocket's) over all of that: the percent of the colours
 * shown, the rest the zero-intensity colour. */
static int fade_percent = 100;
static uint8_t fade_zero[3];
static bool out_identity = true, out_dirty = true;

static int depth_of_mode(int mode)
{
    static const int depths[] = { 1, 2, 4, 8, 16, 32 };
    int i = mode - FIRST_DEPTH_MODE;
    return i >= 0 && i < 6 && qd_screen_supports(depths[i]) ? depths[i] : 0;
}

static int mode_of_depth(int depth)
{
    int mode = FIRST_DEPTH_MODE;
    for (int d = depth; d > 1; d >>= 1)
        mode++;
    return mode;
}

static int current_mode(void) { return mode_of_depth(qd_screen_depth()); }

static int deepest_mode(void)
{
    int best = FIRST_DEPTH_MODE;
    for (int m = FIRST_DEPTH_MODE; m < FIRST_DEPTH_MODE + 6; m++)
        if (depth_of_mode(m))
            best = m;
    return best;
}

static void reset_direct_lut(void)
{
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < 256; i++)
            direct_lut[k][i] = qd_screen_depth() == 16 ? (i < 32 ? (uint8_t)(i << 3 | i >> 2) : 0xFF) : (uint8_t)i;
    out_dirty = true;
}

static void set_linear_gamma(void)
{
    if (!gamma_table)
        return;
    gmemset(gamma_table, 0, GAMMA_HEADER);
    wr16(gamma_table + 6, 1);   /* gChanCnt */
    wr16(gamma_table + 8, 256); /* gDataCnt */
    wr16(gamma_table + 10, 8);  /* gDataWidth */
    for (int i = 0; i < 256; i++)
        wr8(gamma_table + GAMMA_HEADER + (gaddr)i, (uint8_t)i);
    out_dirty = true;
}

/* A gamma table's size, or 0 when it is not one a driver takes: version
 * 0, one channel or three, up to 4096 entries of up to 16 bits. */
static uint32_t gamma_size(gaddr t)
{
    int version = (int16_t)rd16(t), formula = (int16_t)rd16(t + 4), chans = (int16_t)rd16(t + 6);
    int count = (int16_t)rd16(t + 8), width = (int16_t)rd16(t + 10);
    if (version != 0 || formula < 0 || formula > 1024 || (chans != 1 && chans != 3) || count < 2 ||
        count > 4096 || width < 1 || width > 16)
        return 0;
    return GAMMA_HEADER + (uint32_t)formula + (uint32_t)(chans * count * (width > 8 ? 2 : 1));
}

static int16_t set_gamma(gaddr t)
{
    if (!t) {
        set_linear_gamma();
        return noErr;
    }
    uint32_t n = gamma_size(t);
    if (!n || !gamma_table)
        return paramErr;
    gmemcpy(gamma_table, t, n);
    out_dirty = true;
    return noErr;
}

/* The Display Manager's gamma calls (dmgr.c): the driver's table, set
 * or copied into a new handle. */
int16_t video_set_gamma(gaddr table) { return set_gamma(table); }

gaddr video_copy_gamma(void)
{
    uint32_t n = gamma_table ? gamma_size(gamma_table) : 0;
    gaddr h = n ? tb_NewHandle((int32_t)n, false, false) : 0;
    if (h && rd32(h))
        gmemcpy(rd32(h), gamma_table, n);
    return h;
}

/* One component value through the gamma table. */
static uint8_t gamma_of(int k, int v)
{
    gaddr t = gamma_table;
    int chans = (int16_t)rd16(t + 6), count = (int16_t)rd16(t + 8), width = (int16_t)rd16(t + 10);
    int bytes = width > 8 ? 2 : 1;
    gaddr data = t + GAMMA_HEADER + rd16(t + 4) + (gaddr)((chans == 3 ? k : 0) * count * bytes);
    int i = v * count / 256;
    uint32_t e = bytes == 2 ? rd16(data + (gaddr)(i * 2)) : rd8(data + (gaddr)i);
    e &= (1u << width) - 1;
    return (uint8_t)(width >= 8 ? e >> (width - 8) : e * 255 / ((1u << width) - 1));
}

const uint8_t (*video_output_lut(void))[256]
{
    if (out_dirty) {
        out_dirty = false;
        bool off = power_state != kAVPowerOn || (sync_state & 3) == 3; /* both syncs off: no picture */
        int depth = qd_screen_depth();
        out_identity = true;
        for (int k = 0; k < 3; k++)
            for (int v = 0; v < 256; v++) {
                int in = depth == 32 ? direct_lut[k][v] : depth == 16 ? direct_lut[k][v >> 3] : v;
                uint8_t o = off ? 0 : gamma_table ? gamma_of(k, in) : (uint8_t)in;
                if (fade_percent != 100 && !off)
                    o = (uint8_t)((o * fade_percent + fade_zero[k] * (100 - fade_percent)) / 100);
                out_lut[k][v] = o;
                out_identity &= o == v;
            }
    }
    return out_identity ? NULL : (const uint8_t (*)[256])out_lut;
}

/* The colours shown faded to a percent of themselves (0-100), towards
 * zero (an RGBColor; nil is black). */
void video_fade(int percent, gaddr zero)
{
    fade_percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    for (int k = 0; k < 3; k++)
        fade_zero[k] = zero ? (uint8_t)(rd16(zero + (gaddr)k * 2) >> 8) : 0;
    out_dirty = true;
}

/* The screen changed depth: the direct colour table starts linear again. */
void video_mode_changed(void) { reset_direct_lut(); }

bool video_gray(void) { return gray_mode; }

void video_reset(void)
{
    gray_mode = vbl_disabled = false;
    default_mode = MODE_ID;
    sync_state = 0;
    preferred_depth_mode = 0;
    power_state = kAVPowerOn;
    clut_behavior = 0;
    fade_percent = 100;
    gamma_table = tb_NewPtr(MAX_GAMMA, true, true);
    set_linear_gamma();
    reset_direct_lut();
}

/* ---- Colour tables ---- */

/* VDSetEntryRecord: csTable (ColorSpecs), csStart (-1: each entry's value
 * is its index), csCount (the number less one). */
static int16_t entries(gaddr rec, bool set, bool direct)
{
    int depth = qd_screen_depth();
    if (direct != (depth > 8))
        return set ? controlErr : statusErr;
    gaddr table = rd32(rec);
    int start = (int16_t)rd16(rec + 4), count = (int16_t)rd16(rec + 6) + 1;
    int n = depth == 16 ? 32 : depth == 32 ? 256 : 1 << depth;
    if (!table || count < 1 || start < -1 || (start >= 0 && start + count > n))
        return paramErr;
    for (int i = 0; i < count; i++) {
        gaddr s = table + (gaddr)i * 8;
        int index = start < 0 ? (int16_t)rd16(s) : start + i;
        if (index < 0 || index >= n)
            return paramErr;
        uint16_t rgb[3];
        if (set) {
            for (int k = 0; k < 3; k++)
                rgb[k] = rd16(s + 2 + (gaddr)k * 2);
            if (direct) {
                for (int k = 0; k < 3; k++)
                    direct_lut[k][index] = (uint8_t)(rgb[k] >> 8);
                out_dirty = true;
            } else {
                qd_screen_set_hw_entry(index, rgb); /* luminance-mapped in gray mode */
            }
        } else {
            if (direct)
                for (int k = 0; k < 3; k++)
                    rgb[k] = (uint16_t)(direct_lut[k][index] * 0x101);
            else
                qd_screen_get_hw_entry(index, rgb);
            if (start < 0)
                wr16(s, (uint16_t)index);
            for (int k = 0; k < 3; k++)
                wr16(s + 2 + (gaddr)k * 2, rgb[k]);
        }
    }
    return noErr;
}

/* The page filled with gray: a dither of the table's first and last
 * colours at the indexed depths, middle gray at the direct ones. */
static void gray_page(void)
{
    int depth = qd_screen_depth(), w, h;
    machine_screen_size(&w, &h);
    uint32_t rb = qd_screen_row_bytes();
    for (int y = 0; y < h; y++) {
        gaddr row = GA_SCREEN + (gaddr)y * rb;
        if (depth == 32) {
            for (int x = 0; x < w; x++)
                wr32(row + (gaddr)x * 4, 0x00808080);
        } else if (depth == 16) {
            for (int x = 0; x < w; x++)
                wr16(row + (gaddr)x * 2, 0x4210);
        } else {
            static const uint8_t pat[] = { 0, 0xAA, 0xCC, 0, 0xF0, 0, 0, 0, 0xFF };
            uint8_t b = pat[depth];
            if (y & 1)
                b = (uint8_t)~b;
            if (depth == 8) {
                for (uint32_t x = 0; x < rb; x++)
                    wr8(row + x, (uint8_t)(((x + (uint32_t)y) & 1) ? 0xFF : 0));
            } else {
                gmemset(row, b, rb);
            }
        }
    }
    display_note_screen_draw((qrect){ 0, 0, (int16_t)h, (int16_t)w });
}

/* ---- Modes ---- */

static int16_t set_mode(int mode, int page)
{
    int depth = depth_of_mode(mode);
    if (!depth || page != 0)
        return paramErr;
    if (!qd_screen_set_depth(depth))
        return controlErr;
    return noErr;
}

/* VPBlock for a depth: base offset, rowBytes, bounds, version, packing,
 * resolution, pixel type and size, components. */
static void put_vpblock(gaddr vp, int depth)
{
    int w, h;
    machine_screen_size(&w, &h);
    gmemset(vp, 0, 42);
    wr16(vp + 4, (uint16_t)((w * depth + 15) / 16 * 2));
    wr16(vp + 10, (uint16_t)h);
    wr16(vp + 12, (uint16_t)w);
    wr32(vp + 22, 72u << 16);
    wr32(vp + 26, 72u << 16);
    wr16(vp + 30, depth > 8 ? 16 : 0); /* RGBDirect or chunky */
    wr16(vp + 32, (uint16_t)depth);
    wr16(vp + 34, depth > 8 ? 3 : 1);
    wr16(vp + 36, (uint16_t)(depth == 16 ? 5 : depth == 32 ? 8 : depth));
}

static bool our_mode(uint32_t id) { return id == MODE_ID || id == kDisplayModeIDCurrent; }

/* ---- The calls ---- */

static int16_t video_control(gaddr pb, bool async)
{
    int code = (int16_t)rd16(pb + CntrlParam_csCode_off);
    gaddr p = rd32(pb + CntrlParam_csParam_off);
    /* A nil csParam is read as the record at address 0, as a Mac's driver
     * read it: Beyond the Wall's fades store their gamma table's address
     * through the nil csParam of a cleared parameter block, into address
     * 0, and the call finds it there. */
    switch (code) {
    case cscReset: { /* VDPageInfo: the first mode, page 0, grayed */
        int16_t r = set_mode(FIRST_DEPTH_MODE, 0);
        if (r != noErr)
            return r;
        gray_page();
        wr16(p, FIRST_DEPTH_MODE);
        wr16(p + 6, 0);
        wr32(p + 8, GA_SCREEN);
        return noErr;
    }
    case cscKillIO: return noErr;
    case cscSetMode: { /* VDPageInfo: csMode, csPage in; csBaseAddr out */
        int16_t r = set_mode((int16_t)rd16(p), (int16_t)rd16(p + 6));
        if (r == noErr)
            wr32(p + 8, GA_SCREEN);
        return r;
    }
    case cscSetEntries: return entries(p, true, false);
    case cscDirectSetEntries: return entries(p, true, true);
    case cscSetGamma: return set_gamma(rd32(p));
    case cscGrayPage:
        if ((int16_t)rd16(p + 6) != 0)
            return paramErr;
        gray_page();
        return noErr;
    case cscSetGray: /* VDGrayRecord: luminance-mapped from now on */
        gray_mode = rd8(p) != 0;
        return noErr;
    case cscSetInterrupt: /* VDFlagRecord: 1 disables the retrace interrupt */
        vbl_disabled = rd8(p) != 0;
        return noErr;
    case cscSetDefaultMode: /* VDDefMode: the sResource to start in */
        default_mode = rd8(p);
        return noErr;
    case cscSwitchMode: { /* VDSwitchInfoRec: depth mode, display mode, page */
        uint32_t id = rd32(p + 2);
        if (!our_mode(id))
            return paramErr;
        int16_t r = set_mode((int16_t)rd16(p), (int16_t)rd16(p + 6));
        if (r == noErr)
            wr32(p + 8, GA_SCREEN);
        return r;
    }
    case cscSetSync: { /* VDSyncInfoRec: csMode the bits, csFlags which of them */
        uint8_t mask = rd8(p + 1);
        sync_state = (uint8_t)((sync_state & ~mask) | (rd8(p) & mask));
        out_dirty = true;
        return noErr;
    }
    case cscSavePreferredConfiguration:
        if (!our_mode(rd32(p + 2)) || !depth_of_mode((int16_t)rd16(p)))
            return paramErr;
        preferred_depth_mode = rd16(p);
        return noErr;
    case cscSetPowerState: { /* VDPowerStateRec */
        uint32_t s = rd32(p);
        if (s > kAVPowerOn)
            return paramErr;
        power_state = s;
        out_dirty = true;
        wr32(p + 4, 0); /* powerFlags: nothing to refresh */
        return noErr;
    }
    case cscSetClutBehavior: /* VDClutBehavior: when SetEntries takes effect */
        clut_behavior = rd32(p);
        return noErr;
    case cscProbeConnection: return noErr; /* the display is where it was */
    /* No hardware cursor, convolution, other connections, programmable
     * timings or display communication; nothing private. */
    case cscSetHardwareCursor:
    case cscDrawHardwareCursor:
    case cscSetConvolution:
    case cscPrivateControlCall:
    case cscSetMultiConnect:
    case cscSetDetailedTiming:
    case cscDoCommunication:
    default: return controlErr;
    }
}

static int16_t video_status(gaddr pb, bool async)
{
    int code = (int16_t)rd16(pb + CntrlParam_csCode_off);
    gaddr p = rd32(pb + CntrlParam_csParam_off);
    if (!p)
        return paramErr;
    switch (code) {
    case cscGetMode: /* VDPageInfo */
        wr16(p, (uint16_t)current_mode());
        wr16(p + 6, 0);
        wr32(p + 8, GA_SCREEN);
        return noErr;
    case cscGetEntries: return entries(p, false, qd_screen_depth() > 8);
    case cscGetPageCnt: /* VDPageInfo: csMode in, csPage the count out */
        if (!depth_of_mode((int16_t)rd16(p)))
            return paramErr;
        wr16(p + 6, 1);
        return noErr;
    case cscGetPageBase: /* VDPageInfo: csPage in, csBaseAddr out */
        if ((int16_t)rd16(p + 6) != 0)
            return paramErr;
        wr32(p + 8, GA_SCREEN);
        return noErr;
    case cscGetGray:
        wr8(p, gray_mode);
        return noErr;
    case cscGetInterrupt:
        wr8(p, vbl_disabled);
        return noErr;
    case cscGetGamma: /* VDGammaRecord: the driver's own table */
        if (!gamma_table)
            return statusErr;
        wr32(p, gamma_table);
        return noErr;
    case cscGetDefaultMode:
        wr8(p, default_mode);
        return noErr;
    case cscGetCurMode: /* VDSwitchInfoRec */
        wr16(p, (uint16_t)current_mode());
        wr32(p + 2, MODE_ID);
        wr16(p + 6, 0);
        wr32(p + 8, GA_SCREEN);
        wr32(p + 12, 0);
        return noErr;
    case cscGetSync: /* VDSyncInfoRec: csMode 0xFF asks what can be controlled */
        if (rd8(p) == 0xFF)
            wr8(p, 0x03); /* the horizontal and vertical syncs */
        else
            wr8(p, sync_state);
        return noErr;
    case cscGetConnection: /* VDDisplayConnectInfoRec */
        wr16(p, kGenericLCD);
        wr8(p + 2, 0);
        wr8(p + 3, 0);
        wr32(p + 4, 1u << kAllModesValid | 1u << kAllModesSafe | 1u << kBuiltInConnection);
        wr32(p + 8, 0);
        wr32(p + 12, 0);
        return noErr;
    case cscGetModeTiming: /* VDTimingInfoRec: csTimingMode in */
        if (!our_mode(rd32(p)))
            return paramErr;
        wr32(p + 8, 0x6465636C); /* 'decl' */
        wr32(p + 12, timingApple_FixedRateLCD);
        wr32(p + 16, 1u << kModeValid | 1u << kModeSafe | 1u << kModeDefault | 1u << kModeShowNow);
        return noErr;
    case cscGetModeBaseAddress: /* VDBaseAddressInfoRec: csDevData the mode in, csDevBase out */
        if (!our_mode(rd32(p)))
            return paramErr;
        wr32(p + 4, GA_SCREEN);
        return noErr;
    case cscGetPreferredConfiguration: /* VDSwitchInfoRec */
        wr16(p, (uint16_t)(preferred_depth_mode ? preferred_depth_mode : current_mode()));
        wr32(p + 2, MODE_ID);
        wr16(p + 6, 0);
        wr32(p + 8, 0);
        return noErr;
    case cscGetNextResolution: { /* VDResolutionInfoRec */
        uint32_t prev = rd32(p);
        if (prev == MODE_ID) {
            wr32(p + 4, kDisplayModeIDNoMoreResolutions);
            return noErr;
        }
        if (prev != kDisplayModeIDFindFirstResolution && prev != kDisplayModeIDCurrent)
            return paramErr;
        int w, h;
        machine_screen_size(&w, &h);
        wr32(p + 4, MODE_ID);
        wr32(p + 8, (uint32_t)w);
        wr32(p + 12, (uint32_t)h);
        wr32(p + 16, 60u << 16);
        wr16(p + 20, (uint16_t)deepest_mode());
        wr32(p + 22, 0);
        wr32(p + 26, 0);
        return noErr;
    }
    case cscGetVideoParameters: { /* VDVideoParametersInfoRec */
        int depth = depth_of_mode((int16_t)rd16(p + 4));
        if (!our_mode(rd32(p)) || !depth)
            return paramErr;
        if (rd32(p + 6))
            put_vpblock(rd32(p + 6), depth);
        wr32(p + 10, 1);
        wr32(p + 14, depth > 8 ? directType : clutType);
        wr32(p + 18, 0);
        return noErr;
    }
    case cscGetGammaInfoList: { /* VDGetGammaListRec: one table, linear */
        int32_t prev = (int32_t)rd32(p);
        int32_t id = prev == kGammaTableIDFindFirst ? LINEAR_GAMMA_ID
                     : prev == kGammaTableIDSpecific ? (int32_t)rd32(p + 4)
                                                      : kGammaTableIDNoMoreTables;
        if (prev == kGammaTableIDSpecific && id != LINEAR_GAMMA_ID)
            return paramErr;
        wr32(p + 4, (uint32_t)id);
        if (id == kGammaTableIDNoMoreTables)
            return noErr;
        wr32(p + 8, GAMMA_HEADER + 256);
        if (rd32(p + 12)) {
            static const char name[] = "Linear";
            gput(rd32(p + 12), name, sizeof name);
        }
        return noErr;
    }
    case cscRetrieveGammaTable: { /* VDRetrieveGammaRec: the table copied out */
        gaddr to = rd32(p + 4);
        if ((int32_t)rd32(p) != LINEAR_GAMMA_ID || !to)
            return paramErr;
        gmemset(to, 0, GAMMA_HEADER);
        wr16(to + 6, 1);
        wr16(to + 8, 256);
        wr16(to + 10, 8);
        for (int i = 0; i < 256; i++)
            wr8(to + GAMMA_HEADER + (gaddr)i, (uint8_t)i);
        return noErr;
    }
    case cscSupportsHardwareCursor: /* VDSupportsHardwareCursorRec */
        wr8(p, 0);
        return noErr;
    case cscGetPowerState:
        wr32(p, power_state);
        wr32(p + 4, 0);
        return noErr;
    case cscGetClutBehavior:
        wr32(p, clut_behavior);
        return noErr;
    case cscGetScanProc:
    case cscGetHardwareCursorDrawState:
    case cscGetConvolution:
    case cscPrivateStatusCall:
    case cscGetDDCBlock:
    case cscGetMultiConnect:
    case cscGetTimingRanges:
    case cscGetDetailedTiming:
    case cscGetCommunicationInfo:
    default: return statusErr;
    }
}

const native_driver video_driver = { ".Display_Video_Apple_BlueBox", VIDEO_UNIT, NULL, NULL, video_control,
                                     video_status, NULL, NULL, NULL };
