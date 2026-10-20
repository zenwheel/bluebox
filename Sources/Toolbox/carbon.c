/*
 * CarbonLib's additions that sit on what is here: Core Foundation's
 * strings, URLs and bundles at their simplest, the Window Manager 2.0
 * accessors (collapsing, regions, features), and the Control Manager's
 * activation and visibility calls.  (InterfaceLib 8.x and its companions
 * export some of the same names.)
 *
 * A CFString is a block in guest memory: 'CFst', its retain count, its
 * length in UTF-16 units and a pointer to them (after the header, unless
 * made with NoCopy).  A CFURL is 'CFur', its count, its path's CFString,
 * the path style and whether it names a directory.
 */
#include <stdlib.h>
#include <string.h>

#include "forks.h"
#include "ppc_bridge.h"
#include "qd_internal.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

enum { CF_STRING = 0x43467374, CF_URL = 0x43467572, CF_CONSTANT = 0x7FFFFFFF };
enum { cf_kind = 0, cf_count = 4, cf_length = 8, cf_chars = 12, cf_header = 16 };
enum { cf_path = 8, cf_style = 12, cf_dir = 16, cf_url_size = 20 };

/* CFStringEncodings */
enum { kCFStringEncodingMacRoman = 0, kCFStringEncodingUnicode = 0x0100, kCFStringEncodingISOLatin1 = 0x0201,
       kCFStringEncodingASCII = 0x0600, kCFStringEncodingUTF8 = 0x08000100 };

static bool is_kind(gaddr cf, uint32_t kind) { return cf && rd32(cf + cf_kind) == kind; }

/* A string of n UTF-16 units, copied in. */
static gaddr new_string(const uint16_t *u, uint32_t n)
{
    gaddr s = tb_NewPtr((int32_t)(cf_header + 2 * n), false, true);
    if (!s)
        return 0;
    wr32(s + cf_kind, CF_STRING);
    wr32(s + cf_count, 1);
    wr32(s + cf_length, n);
    wr32(s + cf_chars, s + cf_header);
    for (uint32_t i = 0; i < n; i++)
        wr16(s + cf_header + 2 * i, u[i]);
    return s;
}

/* Bytes in an encoding to UTF-16; the units written, or -1 when the bytes
 * aren't in the encoding. */
static long decode(const uint8_t *b, size_t n, uint32_t enc, uint16_t *out)
{
    long k = 0;
    if (enc == kCFStringEncodingMacRoman) {
        for (size_t i = 0; i < n; i++) {
            char utf8[8];
            size_t m = macroman_to_utf8(b + i, 1, utf8, sizeof utf8);
            long r = decode((const uint8_t *)utf8, m, kCFStringEncodingUTF8, out + k);
            k += r > 0 ? r : 0;
        }
        return k;
    }
    if (enc == kCFStringEncodingASCII || enc == kCFStringEncodingISOLatin1) {
        for (size_t i = 0; i < n; i++) {
            if (enc == kCFStringEncodingASCII && b[i] > 127)
                return -1;
            out[k++] = b[i];
        }
        return k;
    }
    if (enc == kCFStringEncodingUnicode) {
        for (size_t i = 0; i + 1 < n; i += 2)
            out[k++] = (uint16_t)(b[i] << 8 | b[i + 1]);
        return k;
    }
    if (enc != kCFStringEncodingUTF8)
        return -1;
    for (size_t i = 0; i < n;) {
        uint32_t cp = b[i], extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
        if (cp >= 0x80 && cp < 0xC0)
            return -1;
        if (i + extra >= n + (extra ? 0 : 1))
            return -1;
        cp &= extra == 3 ? 0x07 : extra == 2 ? 0x0F : extra == 1 ? 0x1F : 0x7F;
        for (uint32_t e = 1; e <= extra; e++)
            cp = cp << 6 | (b[i + e] & 0x3F);
        i += 1 + extra;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out[k++] = (uint16_t)(0xD800 | cp >> 10);
            out[k++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
        } else {
            out[k++] = (uint16_t)cp;
        }
    }
    return k;
}

static gaddr string_from_bytes(gaddr bytes, uint32_t n, uint32_t enc)
{
    if (n > 0x100000)
        return 0;
    uint8_t *b = malloc(n ? n : 1);
    uint16_t *u = malloc(sizeof *u * (n * 2 + 1));
    gget(b, bytes, n);
    long k = decode(b, n, enc, u);
    gaddr s = k >= 0 ? new_string(u, (uint32_t)k) : 0;
    free(b);
    free(u);
    return s;
}

/* CFStringCreateWithCString(alloc, cStr, encoding) */
static void cf_CFStringCreateWithCString(ppc_cpu *c)
{
    uint32_t n = 0;
    while (R(1) && rd8(R(1) + n))
        n++;
    RETURN(R(1) ? string_from_bytes(R(1), n, R(2)) : 0);
}

/* CFStringCreateWithBytes(alloc, bytes, numBytes, encoding, isExternalRepresentation):
 * an external UTF-16 representation may start with its byte order mark. */
static void cf_CFStringCreateWithBytes(ppc_cpu *c)
{
    gaddr b = R(1);
    uint32_t n = R(2);
    if (R(3) == kCFStringEncodingUnicode && n >= 2 && rd16(b) == 0xFEFF)
        b += 2, n -= 2;
    RETURN(b || !n ? string_from_bytes(b, n, R(3)) : 0);
}

/* CFStringCreateWithCharactersNoCopy(alloc, chars, numChars, contentsDeallocator):
 * the characters stay where they are. */
static void cf_CFStringCreateWithCharactersNoCopy(ppc_cpu *c)
{
    gaddr s = tb_NewPtr(cf_header, false, true);
    if (s) {
        wr32(s + cf_kind, CF_STRING);
        wr32(s + cf_count, 1);
        wr32(s + cf_length, R(2));
        wr32(s + cf_chars, R(1));
    }
    RETURN(s);
}

/* __CFStringMakeConstantString(cStr): CFSTR("..."), one string a literal,
 * never freed. */
enum { MAX_CONSTANTS = 256 };
static struct {
    gaddr literal, string;
} constants[MAX_CONSTANTS];
static int nconstants;

static void cf_MakeConstantString(ppc_cpu *c)
{
    for (int i = 0; i < nconstants; i++)
        if (constants[i].literal == R(0)) {
            RETURN(constants[i].string);
            return;
        }
    uint32_t n = 0;
    while (R(0) && rd8(R(0) + n))
        n++;
    gaddr s = R(0) ? string_from_bytes(R(0), n, kCFStringEncodingMacRoman) : 0;
    if (s) {
        wr32(s + cf_count, CF_CONSTANT);
        if (nconstants < MAX_CONSTANTS) {
            constants[nconstants].literal = R(0);
            constants[nconstants++].string = s;
        }
    }
    RETURN(s);
}

static void cf_CFStringGetLength(ppc_cpu *c) { RETURN(is_kind(R(0), CF_STRING) ? rd32(R(0) + cf_length) : 0); }

/* CFStringGetCharacters(string, CFRange {location, length}, buffer): the
 * range comes in r4 and r5. */
static void cf_CFStringGetCharacters(ppc_cpu *c)
{
    gaddr s = R(0), buf = R(3);
    uint32_t loc = R(1), len = R(2);
    if (is_kind(s, CF_STRING) && buf && loc <= rd32(s + cf_length) && len <= rd32(s + cf_length) - loc)
        for (uint32_t i = 0; i < len; i++)
            wr16(buf + 2 * i, rd16(rd32(s + cf_chars) + 2 * (loc + i)));
    c->pc = c->lr;
}

static void cf_CFRetain(ppc_cpu *c)
{
    gaddr cf = R(0);
    if ((is_kind(cf, CF_STRING) || is_kind(cf, CF_URL)) && rd32(cf + cf_count) != CF_CONSTANT)
        wr32(cf + cf_count, rd32(cf + cf_count) + 1);
    RETURN(cf);
}

static void release(gaddr cf)
{
    if (!(is_kind(cf, CF_STRING) || is_kind(cf, CF_URL)) || rd32(cf + cf_count) == CF_CONSTANT)
        return;
    uint32_t n = rd32(cf + cf_count);
    if (n > 1) {
        wr32(cf + cf_count, n - 1);
        return;
    }
    if (is_kind(cf, CF_URL))
        release(rd32(cf + cf_path));
    wr32(cf + cf_kind, 0);
    tb_DisposePtr(cf);
}

static void cf_CFRelease(ppc_cpu *c)
{
    release(R(0));
    c->pc = c->lr;
}

/* CFURLCreateWithFileSystemPath(alloc, filePath, pathStyle, isDirectory) */
static void cf_CFURLCreateWithFileSystemPath(ppc_cpu *c)
{
    gaddr path = R(1);
    if (!is_kind(path, CF_STRING)) {
        RETURN(0);
        return;
    }
    gaddr u = tb_NewPtr(cf_url_size, false, true);
    if (u) {
        wr32(u + cf_kind, CF_URL);
        wr32(u + cf_count, 1);
        if (rd32(path + cf_count) != CF_CONSTANT)
            wr32(path + cf_count, rd32(path + cf_count) + 1);
        wr32(u + cf_path, path);
        wr32(u + cf_style, R(2));
        wr8(u + cf_dir, (uint8_t)R(3));
    }
    RETURN(u);
}

/* CFBundleCreate(alloc, bundleURL): bundles (and the Mach-O code in them,
 * CFBundleGetFunctionPointerForName) are Mac OS X's; there are none. */
static void cf_null(ppc_cpu *c) { RETURN(0); }

/* ---- Windows ---- */

enum { errWindowRegionCodeInvalid = -30593, errInvalidWindowRef = -5600 };

static void wm_IsWindowCollapsed(ppc_cpu *c) { RETURN(R(0) && wm_is_collapsed(R(0))); }

/* CollapseWindow(window, collapse) */
static void wm_CollapseWindow(ppc_cpu *c)
{
    if (!R(0)) {
        RETURN((uint32_t)errInvalidWindowRef);
        return;
    }
    RETURN(wm_set_collapsed(R(0), (R(1) & 0xFF) != 0) ? 0 : (uint32_t)(int32_t)-5602); /* errWindowDoesNotHaveProxy: no collapse box */
}

static void wm_GetWindowFeatures(ppc_cpu *c)
{
    if (R(1))
        wr32(R(1), R(0) ? wm_window_features(R(0)) : 0);
    RETURN(R(0) ? 0 : (uint32_t)errInvalidWindowRef);
}

/* GetWindowRegion(window, regionCode, rgn): the structure, content and
 * update regions, the port's bounds, and the title bar (its drag region
 * too) between the structure's top and the content's. */
static void wm_GetWindowRegion(ppc_cpu *c)
{
    gaddr w = R(0), rgn = R(2);
    uint16_t code = (uint16_t)R(1);
    if (!w || !rgn) {
        RETURN((uint32_t)errInvalidWindowRef);
        return;
    }
    gaddr struc = WindowRecord_strucRgn(w), cont = WindowRecord_contRgn(w);
    switch (code) {
    case 32: /* kWindowStructureRgn */
        tb_CopyRgn(struc, rgn);
        break;
    case 33: /* kWindowContentRgn: empty when collapsed */
        if (wm_is_collapsed(w))
            tb_SetEmptyRgn(rgn);
        else
            tb_CopyRgn(cont, rgn);
        break;
    case 34: /* kWindowUpdateRgn */
        tb_CopyRgn(WindowRecord_updateRgn(w), rgn);
        break;
    case 40: { /* kWindowGlobalPortRgn */
        qrect r = qrect_get(w + GrafPort_portRect_off);
        pixdst d = qd_port_pixels(w);
        tb_SetRectRgn(rgn, (int16_t)(r.left - d.bounds.left), (int16_t)(r.top - d.bounds.top),
                      (int16_t)(r.right - d.bounds.left), (int16_t)(r.bottom - d.bounds.top));
        break;
    }
    case 0: /* kWindowTitleBarRgn */
    case 5: { /* kWindowDragRgn */
        qrect s = qrect_get(rd32(struc) + 2), k = qrect_get(rd32(cont) + 2);
        if (!(wm_window_features(w) & 64) || k.top <= s.top)
            tb_SetEmptyRgn(rgn);
        else
            tb_SetRectRgn(rgn, s.left, s.top, s.right, k.top);
        break;
    }
    default:
        RETURN((uint32_t)errWindowRegionCodeInvalid);
        return;
    }
    RETURN(0);
}

/* ---- Controls ---- */

static void cm_ActivateControl(ppc_cpu *c)
{
    if (R(0))
        tb_HiliteControl(R(0), 0);
    RETURN(0);
}
static void cm_DeactivateControl(ppc_cpu *c)
{
    if (R(0))
        tb_HiliteControl(R(0), 255);
    RETURN(0);
}
static void cm_IsControlActive(ppc_cpu *c) { RETURN(R(0) && rd32(R(0)) && ControlRecord_contrlHilite(rd32(R(0))) != 255); }
static void cm_IsControlVisible(ppc_cpu *c) { RETURN(R(0) && rd32(R(0)) && ControlRecord_contrlVis(rd32(R(0))) != 0); }

/* SetControlVisibility(control, isVisible, doDraw): ShowControl and
 * HideControl when drawing, else the flag alone. */
static void cm_SetControlVisibility(ppc_cpu *c)
{
    gaddr h = R(0);
    bool vis = (R(1) & 0xFF) != 0, draw = (R(2) & 0xFF) != 0;
    if (h && rd32(h)) {
        if (draw)
            vis ? tb_ShowControl(h) : tb_HideControl(h);
        else
            wr8(rd32(h) + 16, vis ? 0xFF : 0); /* contrlVis */
    }
    RETURN(0);
}

void carbon_reset(void) { nconstants = 0; }

const ppc_export carbon_exports[] = {
    { "CFBundleCreate", cf_null },
    { "CFBundleGetFunctionPointerForName", cf_null },
    { "CFRelease", cf_CFRelease },
    { "CFRetain", cf_CFRetain },
    { "CFStringCreateWithBytes", cf_CFStringCreateWithBytes },
    { "CFStringCreateWithCString", cf_CFStringCreateWithCString },
    { "CFStringCreateWithCharactersNoCopy", cf_CFStringCreateWithCharactersNoCopy },
    { "CFStringGetCharacters", cf_CFStringGetCharacters },
    { "CFStringGetLength", cf_CFStringGetLength },
    { "CFURLCreateWithFileSystemPath", cf_CFURLCreateWithFileSystemPath },
    { "__CFStringMakeConstantString", cf_MakeConstantString },
    { "CollapseWindow", wm_CollapseWindow },
    { "GetWindowFeatures", wm_GetWindowFeatures },
    { "GetWindowRegion", wm_GetWindowRegion },
    { "IsWindowCollapsed", wm_IsWindowCollapsed },
    { "ActivateControl", cm_ActivateControl },
    { "DeactivateControl", cm_DeactivateControl },
    { "IsControlActive", cm_IsControlActive },
    { "IsControlVisible", cm_IsControlVisible },
    { "SetControlVisibility", cm_SetControlVisibility },
};
const unsigned carbon_export_count = sizeof carbon_exports / sizeof carbon_exports[0];
