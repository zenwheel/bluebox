/*
 * The Movie Toolbox's and Image Compression Manager's utilities that stand
 * apart from movies: GWorlds by pixel format, a PixMap's QuickTime row
 * bytes and gamma levels, device scaling, MIME types and file name
 * extensions, international text in atom containers, text HREFs, access
 * keys, QuickTime preferences, QuickTime 6's restrictions, idle manager and
 * bandwidth requests, the wall clock time base and the movie errors list.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "forks.h"
#include "qt_atoms.h"
#include "qt_errors.h"
#include "qt_movie_internal.h"
#include "tb_internal.h"

enum { qtUnsupportedPixelFormat = -2096 /* (unsupportedOSErr: no such format here) */ };

/* ---- Pixel formats ---- */

/* A pixel format's depth for QuickDraw (1-32, grey 33-40), or 0. */
static int pixel_depth(uint32_t f)
{
    switch (f) {
    case 1: case 2: case 4: case 8: case 16: case 24: case 32:
        return (int)f;
    case 33: case 34: case 36: case 40:
        return (int)f;
    }
    return 0;
}

int16_t tb_QTGetPixelSize(int32_t PixelFormat)
{
    uint32_t f = (uint32_t)PixelFormat;
    if (f >= 33 && f <= 40)
        return (int16_t)(f - 32);
    if (pixel_depth(f))
        return (int16_t)f;
    static const struct {
        const char *format;
        int16_t bits;
    } sizes[] = {
        { "b16g", 16 }, { "L555", 16 }, { "L565", 16 }, { "B565", 16 }, { "2vuy", 16 }, { "yuvs", 16 },
        { "yuvu", 16 }, { "24BG", 24 }, { "v308", 24 }, { "BGRA", 32 }, { "ABGR", 32 }, { "RGBA", 32 },
        { "b32a", 32 }, { "v408", 32 }, { "r408", 32 }, { "y408", 32 }, { "b48r", 48 }, { "b64a", 64 },
        { "y420", 12 }, { "YVU9", 9 },
    };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
        if (f == fourcc(sizes[i].format))
            return sizes[i].bits;
    return 0;
}

int16_t tb_QTGetPixelFormatDepthForImageDescription(int32_t pixelFormat)
{
    uint32_t f = (uint32_t)pixelFormat;
    if (pixel_depth(f))
        return (int16_t)f;
    int16_t bits = tb_QTGetPixelSize(pixelFormat);
    return bits >= 24 ? 24 : bits;
}

static int16_t new_gworld(gaddr out, uint32_t format, gaddr bounds, gaddr ctab, gaddr gdh, int32_t flags, bool update)
{
    int depth = pixel_depth(format);
    if (!depth) /* the other formats as the nearest QuickDraw has */
        depth = tb_QTGetPixelSize((int32_t)format) >= 24 ? 32 : tb_QTGetPixelSize((int32_t)format) == 16 ? 16 : 0;
    if (!depth)
        return update ? (int16_t)-2096 : (int16_t)paramErr;
    if (update)
        return (int16_t)tb_UpdateGWorld(out, (int16_t)depth, bounds, ctab, gdh, flags);
    return tb_NewGWorld(out, (int16_t)depth, bounds, ctab, gdh, flags);
}

int16_t tb_QTNewGWorld(gaddr offscreenGWorld, int32_t PixelFormat, gaddr boundsRect, int32_t cTable, int32_t aGDevice,
                       int32_t flags)
{
    return new_gworld(offscreenGWorld, (uint32_t)PixelFormat, boundsRect, (gaddr)cTable, (gaddr)aGDevice, flags, false);
}

int32_t tb_QTUpdateGWorld(gaddr offscreenGWorld, int32_t PixelFormat, gaddr boundsRect, int32_t cTable,
                          int32_t aGDevice, int32_t flags)
{
    return new_gworld(offscreenGWorld, (uint32_t)PixelFormat, boundsRect, (gaddr)cTable, (gaddr)aGDevice, flags, true);
}

/* NewImageGWorld: a GWorld the size of an image description, at its depth. */
int16_t tb_NewImageGWorld(gaddr gworld, int32_t idh, int32_t flags)
{
    if (!idh || !rd32((gaddr)idh))
        return badImageDescription;
    gaddr d = rd32((gaddr)idh);
    gaddr r = tb_NewPtr(8, false, true);
    wr16(r + 4, rd16(d + 34)); /* height */
    wr16(r + 6, rd16(d + 32)); /* width */
    int16_t depth = (int16_t)rd16(d + 82);
    if (depth > 32 && depth <= 40)
        depth = (int16_t)(depth - 32);
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16 && depth != 32)
        depth = depth == 24 ? 32 : 32;
    int16_t err = tb_NewGWorld(gworld, depth, r, 0, 0, flags);
    tb_DisposePtr(r);
    return err;
}

/* A PixMap's row bytes and the high flag bits apart. */
int32_t tb_QTGetPixMapPtrRowBytes(int32_t pm) { return pm ? (int32_t)(rd16((gaddr)pm + 4) & 0x3FFF) : 0; }

int32_t tb_QTGetPixMapHandleRowBytes(int32_t pm)
{
    return pm && rd32((gaddr)pm) ? tb_QTGetPixMapPtrRowBytes((int32_t)rd32((gaddr)pm)) : 0;
}

int16_t tb_QTSetPixMapPtrRowBytes(int32_t pm, int32_t rowBytes)
{
    if (!pm || rowBytes < 0 || rowBytes > 0x3FFE)
        return paramErr;
    gaddr p = (gaddr)pm;
    wr16(p + 4, (uint16_t)((rd16(p + 4) & 0xC000) | rowBytes));
    return noErr;
}

int16_t tb_QTSetPixMapHandleRowBytes(int32_t pm, int32_t rowBytes)
{
    return pm && rd32((gaddr)pm) ? tb_QTSetPixMapPtrRowBytes((int32_t)rd32((gaddr)pm), rowBytes) : paramErr;
}

/* Gamma levels (QuickTime 5) are kept beside each PixMap: what it holds,
 * and what decompressing into it should give. */
#define MAX_GAMMAS 64
static struct {
    gaddr pm;
    int32_t level, requested;
} gammas[MAX_GAMMAS];

static int gamma_slot(gaddr pm, bool make)
{
    int free_at = -1;
    for (int i = 0; i < MAX_GAMMAS; i++) {
        if (gammas[i].pm == pm)
            return i;
        if (!gammas[i].pm && free_at < 0)
            free_at = i;
    }
    if (!make)
        return -1;
    if (free_at < 0)
        free_at = (int)(pm % MAX_GAMMAS);
    gammas[free_at].pm = pm;
    gammas[free_at].level = gammas[free_at].requested = 0; /* kQTUsePlatformDefaultGammaLevel */
    return free_at;
}

int32_t tb_QTGetPixMapPtrGammaLevel(int32_t pm)
{
    int i = gamma_slot((gaddr)pm, false);
    return i >= 0 ? gammas[i].level : 0;
}

int16_t tb_QTSetPixMapPtrGammaLevel(int32_t pm, int32_t gammaLevel)
{
    if (!pm)
        return paramErr;
    gammas[gamma_slot((gaddr)pm, true)].level = gammaLevel;
    return noErr;
}

int32_t tb_QTGetPixMapPtrRequestedGammaLevel(int32_t pm)
{
    int i = gamma_slot((gaddr)pm, false);
    return i >= 0 ? gammas[i].requested : 0;
}

int16_t tb_QTSetPixMapPtrRequestedGammaLevel(int32_t pm, int32_t requestedGammaLevel)
{
    if (!pm)
        return paramErr;
    gammas[gamma_slot((gaddr)pm, true)].requested = requestedGammaLevel;
    return noErr;
}

int32_t tb_QTGetPixMapHandleGammaLevel(int32_t pm)
{
    return pm && rd32((gaddr)pm) ? tb_QTGetPixMapPtrGammaLevel((int32_t)rd32((gaddr)pm)) : 0;
}

int16_t tb_QTSetPixMapHandleGammaLevel(int32_t pm, int32_t gammaLevel)
{
    return pm && rd32((gaddr)pm) ? tb_QTSetPixMapPtrGammaLevel((int32_t)rd32((gaddr)pm), gammaLevel) : paramErr;
}

int32_t tb_QTGetPixMapHandleRequestedGammaLevel(int32_t pm)
{
    return pm && rd32((gaddr)pm) ? tb_QTGetPixMapPtrRequestedGammaLevel((int32_t)rd32((gaddr)pm)) : 0;
}

int16_t tb_QTSetPixMapHandleRequestedGammaLevel(int32_t pm, int32_t requestedGammaLevel)
{
    return pm && rd32((gaddr)pm) ? tb_QTSetPixMapPtrRequestedGammaLevel((int32_t)rd32((gaddr)pm), requestedGammaLevel)
                                 : paramErr;
}

/* ---- PixMap copies (track mattes) ---- */

#define MAX_COPIES 32
static struct {
    gaddr gw, pm;
} copies[MAX_COPIES];

/* A copy of a PixMap: a GWorld of its depth and size with its pixels; the
 * copy is the GWorld's PixMapHandle. */
gaddr qt_copy_pixmap(gaddr pmh)
{
    if (!pmh || !rd32(pmh))
        return 0;
    gaddr pm = rd32(pmh);
    int16_t depth = (int16_t)rd16(pm + 32);
    gaddr bounds = tb_NewPtr(8, false, false);
    gmemcpy(bounds, pm + 6, 8);
    gaddr out = tb_NewPtr(4, false, true);
    int slot = -1;
    for (int i = 0; i < MAX_COPIES && slot < 0; i++)
        if (!copies[i].gw)
            slot = i;
    gaddr result = 0;
    if (slot >= 0 && tb_NewGWorld(out, depth ? depth : 32, bounds, 0, 0, 0) == noErr && rd32(out)) {
        gaddr gw = rd32(out);
        gaddr dst = tb_GetGWorldPixMap(gw);
        tb_CopyBits(pm, rd32(dst), bounds, bounds, 0 /* srcCopy */, 0);
        copies[slot].gw = gw;
        copies[slot].pm = dst;
        result = dst;
    }
    tb_DisposePtr(out);
    tb_DisposePtr(bounds);
    return result;
}

void qt_dispose_pixmap(gaddr pm)
{
    for (int i = 0; i < MAX_COPIES; i++)
        if (copies[i].pm == pm && pm) {
            tb_DisposeGWorld(copies[i].gw);
            copies[i].gw = copies[i].pm = 0;
            return;
        }
}

/* ---- Device scaling: no device of ours scales in hardware ---- */

int16_t tb_GDHasScale(int32_t gdh, int16_t depth, gaddr scale)
{
    (void)gdh, (void)depth;
    if (scale)
        wr32(scale, 0x10000);
    return noErr;
}

int16_t tb_GDGetScale(int32_t gdh, gaddr scale, gaddr flags)
{
    (void)gdh;
    if (scale)
        wr32(scale, 0x10000);
    if (flags)
        wr16(flags, 0);
    return noErr;
}

int16_t tb_GDSetScale(int32_t gdh, int32_t scale, int16_t flags)
{
    (void)gdh, (void)flags;
    return scale == 0x10000 ? noErr : codecConditionErr;
}

/* ---- MIME types and file name extensions ---- */

static bool mime_is(const char *mime, size_t n, const char *want)
{
    return strlen(want) == n && !strncasecmp(mime, want, n);
}

int16_t tb_QTGetMIMETypeInfo(gaddr mimeStringStart, int16_t mimeStringLength, int32_t infoSelector, gaddr infoDataPtr,
                             gaddr infoDataSize)
{
    char mime[256];
    size_t n = mimeStringLength > 0 && mimeStringLength < 256 ? (size_t)mimeStringLength : 0;
    if (n && mimeStringStart)
        gget(mime, mimeStringStart, (uint32_t)n);
    mime[n] = 0;
    bool answer;
    if ((uint32_t)infoSelector == fourcc("moov"))
        answer = mime_is(mime, n, "video/quicktime") || mime_is(mime, n, "application/x-quicktimeplayer");
    else if ((uint32_t)infoSelector == fourcc("dumb"))
        answer = mime_is(mime, n, "application/octet-stream") || mime_is(mime, n, "text/plain") ||
                 mime_is(mime, n, "application/x-unknown") || n == 0;
    else
        return paramErr;
    if (infoDataSize && rd32(infoDataSize) < 1)
        return paramErr;
    if (infoDataPtr)
        wr8(infoDataPtr, answer);
    if (infoDataSize)
        wr32(infoDataSize, 1);
    return noErr;
}

/* A file name's extension (after the last '.', up to four characters, as
 * an OSType padded with spaces), or 0. */
int16_t tb_QTGetFileNameExtension(int32_t fileName, int32_t fileType, gaddr extension)
{
    (void)fileType;
    uint32_t ext = 0;
    if (fileName) {
        uint8_t n = rd8((gaddr)fileName);
        int dot = -1;
        for (int i = 0; i < n; i++)
            if (rd8((gaddr)fileName + 1 + (uint32_t)i) == '.')
                dot = i;
        if (dot >= 0 && n - dot - 1 >= 1 && n - dot - 1 <= 4) {
            for (int i = 0; i < 4; i++) {
                int k = dot + 1 + i;
                uint8_t c = k < n ? rd8((gaddr)fileName + 1 + (uint32_t)k) : ' ';
                ext = ext << 8 | c;
            }
        }
    }
    if (extension)
        wr32(extension, ext);
    return noErr;
}

/* ---- International text in atom containers ----
 * Under the parent, an 'itxt' atom holds a 'text' atom per region code
 * (its ID), the string its data. */
static uint32_t itext_atom(gaddr container, uint32_t parent, bool make)
{
    int32_t a = tb_QTFindChildByID((int32_t)container, (int32_t)parent, (int32_t)fourcc("itxt"), 1, 0);
    if (!a && make)
        tb_QTInsertChild((int32_t)container, (int32_t)parent, (int32_t)fourcc("itxt"), 1, 0, 0, 0, 0),
            a = tb_QTFindChildByID((int32_t)container, (int32_t)parent, (int32_t)fourcc("itxt"), 1, 0);
    return (uint32_t)a;
}

int16_t tb_ITextAddString(int32_t container, int32_t parentAtom, int16_t theRegionCode, int32_t theString)
{
    uint32_t it = itext_atom((gaddr)container, (uint32_t)parentAtom, true);
    if (!it)
        return invalidAtomContainerErr;
    uint8_t n = theString ? rd8((gaddr)theString) : 0;
    int32_t old = tb_QTFindChildByID(container, (int32_t)it, (int32_t)fourcc("text"), (uint16_t)theRegionCode, 0);
    if (old)
        return tb_QTSetAtomData(container, old, n, (gaddr)theString + 1);
    return tb_QTInsertChild(container, (int32_t)it, (int32_t)fourcc("text"), (uint16_t)theRegionCode, 0, n,
                            n ? (gaddr)theString + 1 : 0, 0);
}

int16_t tb_ITextGetString(int32_t container, int32_t parentAtom, int16_t requestedRegion, gaddr foundRegion,
                          int32_t theString)
{
    uint32_t it = itext_atom((gaddr)container, (uint32_t)parentAtom, false);
    if (theString)
        wr8((gaddr)theString, 0);
    if (!it)
        return cannotFindAtomErr;
    uint32_t id = (uint16_t)requestedRegion;
    int32_t a = tb_QTFindChildByID(container, (int32_t)it, (int32_t)fourcc("text"), (int32_t)id, 0);
    if (!a) { /* another language's, if that's all there is */
        gaddr idp = tb_NewPtr(4, false, true);
        a = tb_QTFindChildByIndex(container, (int32_t)it, (int32_t)fourcc("text"), 1, idp);
        id = rd32(idp);
        tb_DisposePtr(idp);
    }
    if (!a)
        return cannotFindAtomErr;
    if (foundRegion)
        wr16(foundRegion, (uint16_t)id);
    gaddr size = tb_NewPtr(4, false, true);
    gaddr buf = tb_NewPtr(256, false, true);
    int16_t err = tb_QTCopyAtomDataToPtr(container, a, 1, 255, buf, size);
    if (!err && theString) {
        uint32_t n = rd32(size);
        wr8((gaddr)theString, (uint8_t)n);
        gmemcpy((gaddr)theString + 1, buf, n);
    }
    tb_DisposePtr(buf);
    tb_DisposePtr(size);
    return err;
}

int16_t tb_ITextRemoveString(int32_t container, int32_t parentAtom, int16_t theRegionCode, int32_t flags)
{
    uint32_t it = itext_atom((gaddr)container, (uint32_t)parentAtom, false);
    if (!it)
        return cannotFindAtomErr;
    (void)flags; /* either way, the string for the region stays */
    for (;;) {
        gaddr idp = tb_NewPtr(4, false, true);
        int32_t a = 0;
        for (int16_t i = 1;; i++) {
            int32_t x = tb_QTFindChildByIndex(container, (int32_t)it, (int32_t)fourcc("text"), i, idp);
            if (!x)
                break;
            if ((uint16_t)rd32(idp) != (uint16_t)theRegionCode) {
                a = x;
                break;
            }
        }
        tb_DisposePtr(idp);
        if (!a)
            break;
        tb_QTRemoveAtom(container, a);
    }
    return noErr;
}

/* QuickTime's text HREFs: [A]<url>[ T<target>][ E<embed tags>], a click
 * point and base URL from the container; the parts out as atoms. */
static void add_string_atom(gaddr c, uint32_t type, const char *s, size_t n)
{
    gaddr p = tb_NewPtr((int32_t)(n ? n : 1), false, false);
    if (n)
        gput(p, s, (uint32_t)n);
    tb_QTInsertChild((int32_t)c, 0, (int32_t)type, 1, 0, (int32_t)n, p, 0);
    tb_DisposePtr(p);
}

int16_t tb_QTParseTextHREF(gaddr href, int32_t hrefLen, int32_t inContainer, gaddr outContainer)
{
    if (outContainer)
        wr32(outContainer, 0);
    if (!href || hrefLen < 0)
        return paramErr;
    char *s = malloc((size_t)hrefLen + 1);
    gget(s, href, (uint32_t)hrefLen);
    s[hrefLen] = 0;
    gaddr out = qt_atoms_new();
    if (!out) {
        free(s);
        return memFullErr;
    }
    char base[1024] = "";
    if (inContainer) {
        int32_t b = tb_QTFindChildByID(inContainer, 0, (int32_t)fourcc("burl"), 1, 0);
        if (b) {
            gaddr size = tb_NewPtr(4, false, true), buf = tb_NewPtr(1023, false, true);
            if (!tb_QTCopyAtomDataToPtr(inContainer, b, 1, 1023, buf, size)) {
                gget(base, buf, rd32(size));
                base[rd32(size)] = 0;
            }
            tb_DisposePtr(size);
            tb_DisposePtr(buf);
        }
    }
    const char *p = s;
    while (*p == ' ')
        p++;
    bool autohref = false;
    if (*p == 'A' && p[1] == '<') {
        autohref = true;
        p++;
    }
    char url[2048] = "", target[512] = "", embed[1024] = "";
    if (*p == '<') {
        const char *e = strchr(p + 1, '>');
        size_t n = e ? (size_t)(e - p - 1) : strlen(p + 1);
        snprintf(url, sizeof url, "%.*s", (int)n, p + 1);
        p = e ? e + 1 : p + strlen(p);
    } else {
        snprintf(url, sizeof url, "%s", p);
        p += strlen(p);
    }
    while (*p) {
        while (*p == ' ')
            p++;
        if ((*p == 'T' || *p == 'E') && p[1] == '<') {
            const char *e = strchr(p + 2, '>');
            size_t n = e ? (size_t)(e - p - 2) : strlen(p + 2);
            if (*p == 'T')
                snprintf(target, sizeof target, "%.*s", (int)n, p + 2);
            else
                snprintf(embed, sizeof embed, "%.*s", (int)n, p + 2);
            p = e ? e + 1 : p + strlen(p);
        } else if (*p) {
            p++;
        }
    }
    /* a relative URL against the base */
    char full[3072];
    if (*base && !strstr(url, "://") && url[0] != '/') {
        const char *slash = strrchr(base, '/');
        size_t keep = slash ? (size_t)(slash - base + 1) : strlen(base);
        snprintf(full, sizeof full, "%.*s%s", (int)keep, base, url);
    } else {
        snprintf(full, sizeof full, "%s", url);
    }
    char *hash = strchr(full, '#');
    if (hash) {
        add_string_atom(out, fourcc("chap"), hash + 1, strlen(hash + 1));
        *hash = 0;
    }
    add_string_atom(out, fourcc("url "), full, strlen(full));
    if (*target)
        add_string_atom(out, fourcc("targ"), target, strlen(target));
    if (*embed)
        add_string_atom(out, fourcc("mbed"), embed, strlen(embed));
    uint8_t yes = autohref;
    tb_QTInsertChild((int32_t)out, 0, (int32_t)fourcc("auto"), 1, 0, 1, 0, 0);
    int32_t a = tb_QTFindChildByID((int32_t)out, 0, (int32_t)fourcc("auto"), 1, 0);
    gaddr y = tb_NewPtr(1, false, false);
    wr8(y, yes);
    tb_QTSetAtomData((int32_t)out, a, 1, y);
    tb_DisposePtr(y);
    char rhref[4096];
    snprintf(rhref, sizeof rhref, "<%s%s%s>%s%s%s", full, hash ? "#" : "", hash ? hash + 1 : "", *target ? " T<" : "",
             target, *target ? ">" : "");
    add_string_atom(out, fourcc("href"), rhref, strlen(rhref));
    free(s);
    if (outContainer)
        wr32(outContainer, out);
    return noErr;
}

/* QTTextToNativeText: Unicode (UTF-16 or UTF-8) text made Mac Roman, in place. */
int16_t tb_QTTextToNativeText(int32_t theText, int32_t encoding, int32_t flags)
{
    (void)flags;
    gaddr h = (gaddr)theText;
    if (!h || !rd32(h))
        return paramErr;
    uint32_t base = (uint32_t)encoding & 0xFFFF, form = (uint32_t)encoding >> 26 & 0x3;
    if (base == 0) /* kTextEncodingMacRoman */
        return noErr;
    if (base != 0x100 && base != 0x103) /* Unicode 2.0 and its forms */
        return paramErr;
    uint32_t n = (uint32_t)tb_GetHandleSize(h);
    uint8_t *in = malloc(n + 2);
    gget(in, rd32(h), n);
    char *utf8 = malloc((size_t)n * 3 + 1);
    size_t u = 0;
    if (form == 2 || base == 0x103 || (encoding & 0x0C000000) == 0x08000000) { /* UTF-8 */
        memcpy(utf8, in, n);
        u = n;
    } else { /* UTF-16, big-endian unless marked otherwise */
        bool le = n >= 2 && in[0] == 0xFF && in[1] == 0xFE;
        for (uint32_t i = (n >= 2 && (in[0] == 0xFE || in[0] == 0xFF) && (in[1] == 0xFF || in[1] == 0xFE)) ? 2 : 0;
             i + 1 < n; i += 2) {
            uint32_t c = le ? (uint32_t)(in[i + 1] << 8 | in[i]) : (uint32_t)(in[i] << 8 | in[i + 1]);
            if (c < 0x80) {
                utf8[u++] = (char)c;
            } else if (c < 0x800) {
                utf8[u++] = (char)(0xC0 | c >> 6);
                utf8[u++] = (char)(0x80 | (c & 0x3F));
            } else {
                utf8[u++] = (char)(0xE0 | c >> 12);
                utf8[u++] = (char)(0x80 | (c >> 6 & 0x3F));
                utf8[u++] = (char)(0x80 | (c & 0x3F));
            }
        }
    }
    utf8[u] = 0;
    uint8_t *mac = malloc(u + 1);
    size_t m = utf8_to_macroman(utf8, mac, u + 1);
    tb_SetHandleSize(h, (int32_t)m);
    if (m)
        gput(rd32(h), mac, (uint32_t)m);
    free(in);
    free(utf8);
    free(mac);
    return noErr;
}

/* ---- Access keys (QuickTime 3): registered per type, given as an atom container ---- */

#define MAX_KEYS 32
static struct {
    uint32_t type;
    int32_t flags;
    uint8_t *key;
    uint32_t len;
} keys[MAX_KEYS];

static uint8_t *handle_copy_bytes(gaddr h, uint32_t *n)
{
    *n = h && rd32(h) ? (uint32_t)tb_GetHandleSize(h) : 0;
    uint8_t *b = malloc(*n ? *n : 1);
    if (*n)
        gget(b, rd32(h), *n);
    return b;
}

int16_t tb_QTRegisterAccessKey(int32_t accessKeyType, int32_t flags, int32_t accessKey)
{
    uint32_t n;
    uint8_t *b = handle_copy_bytes((gaddr)accessKey, &n);
    for (int i = 0; i < MAX_KEYS; i++)
        if (keys[i].key && keys[i].type == (uint32_t)accessKeyType && keys[i].len == n && !memcmp(keys[i].key, b, n)) {
            free(b);
            return noErr;
        }
    for (int i = 0; i < MAX_KEYS; i++)
        if (!keys[i].key) {
            keys[i].type = (uint32_t)accessKeyType;
            keys[i].flags = flags;
            keys[i].key = b;
            keys[i].len = n;
            return noErr;
        }
    free(b);
    return memFullErr;
}

int16_t tb_QTUnregisterAccessKey(int32_t accessKeyType, int32_t flags, int32_t accessKey)
{
    (void)flags;
    uint32_t n;
    uint8_t *b = handle_copy_bytes((gaddr)accessKey, &n);
    int16_t err = paramErr;
    for (int i = 0; i < MAX_KEYS; i++)
        if (keys[i].key && keys[i].type == (uint32_t)accessKeyType && keys[i].len == n && !memcmp(keys[i].key, b, n)) {
            free(keys[i].key);
            keys[i].key = NULL;
            err = noErr;
        }
    free(b);
    return err;
}

int16_t tb_QTGetAccessKeys(int32_t accessKeyType, int32_t flags, gaddr out)
{
    gaddr c = qt_atoms_new();
    if (!c)
        return memFullErr;
    for (int i = 0; i < MAX_KEYS; i++) {
        if (!keys[i].key || keys[i].type != (uint32_t)accessKeyType)
            continue;
        if ((flags & 1) && !(keys[i].flags & 1)) /* kAccessKeySystemFlag: the system's only */
            continue;
        int16_t err;
        qt_atoms_insert(c, 0, fourcc("acky"), 0, 0, keys[i].key, keys[i].len, &err);
    }
    if (out)
        wr32(out, c);
    return noErr;
}

/* ---- Preferences: per type, an atom container ---- */

#define MAX_PREFS 16
static struct {
    uint32_t type;
    gaddr atom;
} prefs[MAX_PREFS];

int16_t tb_GetQuickTimePreference(int32_t preferenceType, gaddr preferenceAtom)
{
    if (preferenceAtom)
        wr32(preferenceAtom, 0);
    for (int i = 0; i < MAX_PREFS; i++)
        if (prefs[i].atom && prefs[i].type == (uint32_t)preferenceType) {
            gaddr out;
            gaddr p = tb_NewPtr(4, false, true);
            tb_QTCopyAtom((int32_t)prefs[i].atom, 0, p);
            out = rd32(p);
            tb_DisposePtr(p);
            if (preferenceAtom)
                wr32(preferenceAtom, out);
            return noErr;
        }
    if ((uint32_t)preferenceType == fourcc("cspd")) { /* the connection: as fast as a LAN (QuickTime's T1) */
        gaddr c = qt_atoms_new();
        uint8_t speed[8] = { 0, 0, 0, 0, 0, 0x16, 0xE3, 0x60 }; /* ConnectionSpeedPrefsRecord: 1500000 bit/s */
        int16_t err;
        qt_atoms_insert(c, 0, fourcc("cspd"), 1, 0, speed + 4, 4, &err);
        if (preferenceAtom)
            wr32(preferenceAtom, c);
        return noErr;
    }
    return paramErr;
}

int16_t tb_SetQuickTimePreference(int32_t preferenceType, int32_t preferenceAtom)
{
    if (!preferenceAtom)
        return paramErr;
    int at = -1;
    for (int i = 0; i < MAX_PREFS; i++)
        if (prefs[i].atom && prefs[i].type == (uint32_t)preferenceType)
            at = i;
    for (int i = 0; i < MAX_PREFS && at < 0; i++)
        if (!prefs[i].atom)
            at = i;
    if (at < 0)
        return memFullErr;
    if (prefs[at].atom)
        tb_DisposeHandle(prefs[at].atom);
    gaddr p = tb_NewPtr(4, false, true);
    int16_t err = tb_QTCopyAtom(preferenceAtom, 0, p);
    prefs[at].atom = err ? 0 : rd32(p);
    prefs[at].type = (uint32_t)preferenceType;
    tb_DisposePtr(p);
    return err;
}

/* QuickTime Pro's registration: nothing to check. */
void tb_CheckQuickTimeRegistration(gaddr registrationKey, int32_t flags) { (void)registrationKey, (void)flags; }

/* ---- Restrictions (QuickTime 6): movies here carry none ---- */

int16_t tb_QTGetMovieRestrictions(int32_t theMovie, gaddr outRestrictionSet, gaddr outSeed)
{
    if (outRestrictionSet)
        wr32(outRestrictionSet, 0);
    if (outSeed)
        wr32(outSeed, 0);
    return qt_movie_model((gaddr)theMovie) ? noErr : invalidMovie;
}

int16_t tb_QTRestrictionsGetInfo(int32_t inRestrictionSet, gaddr outRestrictionClassCount, gaddr outSeed)
{
    (void)inRestrictionSet;
    if (outRestrictionClassCount)
        wr32(outRestrictionClassCount, 0);
    if (outSeed)
        wr32(outSeed, 0);
    return noErr;
}

int16_t tb_QTRestrictionsGetIndClass(int32_t inRestrictionSet, int32_t inIndex, gaddr outClass)
{
    (void)inRestrictionSet, (void)inIndex;
    if (outClass)
        wr32(outClass, 0);
    return paramErr;
}

int16_t tb_QTRestrictionsGetItem(int32_t inRestrictionSet, int32_t inRestrictionClass, gaddr outRestrictions)
{
    (void)inRestrictionSet, (void)inRestrictionClass;
    if (outRestrictions)
        wr32(outRestrictions, 0);
    return noErr;
}

int16_t tb_QTGetSupportedRestrictions(int32_t inRestrictionClass, gaddr outRestrictionIDs)
{
    (void)inRestrictionClass;
    if (outRestrictionIDs)
        wr32(outRestrictionIDs, 0);
    return noErr;
}

/* ---- The idle manager (QuickTime 6): when each idler wants time next ---- */

#define MAX_IDLERS 32
static struct {
    gaddr token;
    int64_t next_ns;  /* INT64_MAX: never */
    gaddr parent;
} idlers[MAX_IDLERS];

static int idler(int32_t im)
{
    for (int i = 0; im && i < MAX_IDLERS; i++)
        if (idlers[i].token == (gaddr)im)
            return i;
    return -1;
}

int32_t tb_QTIdleManagerOpen(void)
{
    for (int i = 0; i < MAX_IDLERS; i++)
        if (!idlers[i].token) {
            idlers[i].token = tb_NewHandle(4, false, true);
            idlers[i].next_ns = INT64_MAX;
            idlers[i].parent = 0;
            return (int32_t)idlers[i].token;
        }
    return 0;
}

int16_t tb_QTIdleManagerClose(int32_t im)
{
    int i = idler(im);
    if (i < 0)
        return paramErr;
    tb_DisposeHandle(idlers[i].token);
    idlers[i].token = 0;
    return noErr;
}

/* Times are TimeRecords on the wall clock. */
static int64_t record_ns(gaddr tr)
{
    int64_t v = (int64_t)((uint64_t)rd32(tr) << 32 | rd32(tr + 4));
    int32_t s = (int32_t)rd32(tr + 8);
    return s > 0 ? (int64_t)((__int128)v * 1000000000 / s) : v;
}

static void put_record_ns(gaddr tr, int64_t ns)
{
    int32_t s = 600;
    int64_t v = ns == INT64_MAX ? 0x7FFFFFFFFFFFFFFFLL : ns * s / 1000000000;
    wr32(tr, (uint32_t)((uint64_t)v >> 32));
    wr32(tr + 4, (uint32_t)v);
    wr32(tr + 8, (uint32_t)s);
    wr32(tr + 12, 0);
}

int16_t tb_QTIdleManagerGetNextIdleTime(int32_t im, gaddr nextIdle)
{
    int i = idler(im);
    if (i < 0)
        return paramErr;
    if (nextIdle)
        put_record_ns(nextIdle, idlers[i].next_ns);
    return noErr;
}

static void set_next(int i, int64_t ns)
{
    idlers[i].next_ns = ns;
    int p = idler((int32_t)idlers[i].parent); /* a parent wants time as soon as any child does */
    if (p >= 0 && ns < idlers[p].next_ns)
        set_next(p, ns);
}

int16_t tb_QTIdleManagerSetNextIdleTime(int32_t im, gaddr nextIdle)
{
    int i = idler(im);
    if (i < 0 || !nextIdle)
        return paramErr;
    set_next(i, (int64_t)machine_time_ns() + record_ns(nextIdle));
    return noErr;
}

int16_t tb_QTIdleManagerSetNextIdleTimeNever(int32_t im)
{
    int i = idler(im);
    if (i < 0)
        return paramErr;
    idlers[i].next_ns = INT64_MAX;
    return noErr;
}

int16_t tb_QTIdleManagerSetNextIdleTimeNow(int32_t im)
{
    int i = idler(im);
    if (i < 0)
        return paramErr;
    set_next(i, (int64_t)machine_time_ns());
    return noErr;
}

int16_t tb_QTIdleManagerSetNextIdleTimeDelta(int32_t im, int32_t duration, int32_t scale)
{
    int i = idler(im);
    if (i < 0 || scale <= 0)
        return paramErr;
    set_next(i, (int64_t)machine_time_ns() + (int64_t)duration * 1000000000 / scale);
    return noErr;
}

int16_t tb_QTIdleManagerSetParent(int32_t im, int32_t parent)
{
    int i = idler(im);
    if (i < 0)
        return paramErr;
    idlers[i].parent = (gaddr)parent;
    set_next(i, idlers[i].next_ns);
    return noErr;
}

int16_t tb_QTIdleManagerNeedsAnIdle(int32_t im, gaddr needsOne)
{
    int i = idler(im);
    if (i < 0)
        return paramErr;
    if (needsOne)
        wr8(needsOne, idlers[i].next_ns <= (int64_t)machine_time_ns());
    return noErr;
}

/* QTGetTimeUntilNextTask: a playing movie wants time at once; otherwise
 * not for a second. */
int16_t tb_QTGetTimeUntilNextTask(gaddr duration, int32_t scale)
{
    if (scale <= 0)
        return paramErr;
    if (duration)
        wr32(duration, (uint32_t)(qt_any_movie_playing() ? 0 : scale));
    return noErr;
}

#define MAX_SOONER 8
static struct {
    gaddr proc, refcon;
    int32_t scale, flags;
} sooner[MAX_SOONER];

int16_t tb_QTInstallNextTaskNeededSoonerCallback(int32_t callbackProc, int32_t scale, int32_t flags, gaddr refcon)
{
    for (int i = 0; i < MAX_SOONER; i++)
        if (!sooner[i].proc) {
            sooner[i].proc = (gaddr)callbackProc;
            sooner[i].refcon = refcon;
            sooner[i].scale = scale;
            sooner[i].flags = flags;
            return noErr;
        }
    return memFullErr;
}

int16_t tb_QTUninstallNextTaskNeededSoonerCallback(int32_t callbackProc, gaddr refcon)
{
    for (int i = 0; i < MAX_SOONER; i++)
        if (sooner[i].proc == (gaddr)callbackProc && sooner[i].refcon == refcon) {
            sooner[i].proc = 0;
            return noErr;
        }
    return paramErr;
}

/* A movie started: those waiting are told a task is needed now
 * (pascal void (TimeValue duration, unsigned long flags, void *refcon)). */
void qt_task_needed_sooner(void)
{
    for (int i = 0; i < MAX_SOONER; i++)
        if (sooner[i].proc) {
            cpu68k *c = g_cpu;
            cpu_push32(c, 0);
            cpu_push32(c, 0);
            cpu_push32(c, sooner[i].refcon);
            guest_call(sooner[i].proc);
        }
}

/* ---- Bandwidth (QuickTime 5): there is always enough ---- */

int16_t tb_QTBandwidthRequest(int32_t priority, int32_t callback, gaddr refcon, gaddr bwRef, int32_t flags)
{
    (void)priority, (void)callback, (void)refcon, (void)flags;
    gaddr h = tb_NewHandle(4, false, true);
    if (bwRef)
        wr32(bwRef, h);
    return h ? noErr : memFullErr;
}

int16_t tb_QTBandwidthRequestForTimeBase(int32_t tb, int32_t priority, int32_t callback, gaddr refcon, gaddr bwRef,
                                         int32_t flags)
{
    (void)tb;
    return tb_QTBandwidthRequest(priority, callback, refcon, bwRef, flags);
}

int16_t tb_QTBandwidthRelease(int32_t bwRef, int32_t flags)
{
    (void)flags;
    if (bwRef)
        tb_DisposeHandle((gaddr)bwRef);
    return noErr;
}

int16_t tb_QTScheduledBandwidthRequest(int32_t scheduleRec, int32_t notificationCallback, gaddr refcon, gaddr sbwRef,
                                       int32_t flags)
{
    (void)scheduleRec;
    return tb_QTBandwidthRequest(0, notificationCallback, refcon, sbwRef, flags);
}

int16_t tb_QTScheduledBandwidthRelease(int32_t sbwRef, int32_t flags) { return tb_QTBandwidthRelease(sbwRef, flags); }

/* ---- The wall clock: a time base that keeps the machine's time ---- */

static gaddr wall_clock;

int16_t tb_QTGetWallClockTimeBase(gaddr wallClockTimeBase)
{
    if (!wall_clock) {
        wall_clock = (gaddr)tb_NewTimeBase();
        if (wall_clock) {
            tb_SetTimeBaseValue((int32_t)wall_clock, (int32_t)(machine_time_ns() / 1000000), 1000);
            tb_SetTimeBaseRate((int32_t)wall_clock, 0x10000);
        }
    }
    if (wallClockTimeBase)
        wr32(wallClockTimeBase, wall_clock);
    return wall_clock ? noErr : memFullErr;
}

/* ---- Movie errors (QuickTime 6): added to the movie, nothing shown ---- */

int16_t tb_QTAddMovieError(int32_t movieH, int32_t c, int32_t errorCode, int32_t stringReplacements)
{
    (void)c, (void)errorCode, (void)stringReplacements;
    return qt_movie_model((gaddr)movieH) ? noErr : invalidMovie;
}

/* A new run: nothing registered. */
void qt_util_reset(void)
{
    for (int i = 0; i < MAX_KEYS; i++)
        free(keys[i].key);
    memset(keys, 0, sizeof keys);
    memset(prefs, 0, sizeof prefs);
    memset(idlers, 0, sizeof idlers);
    memset(sooner, 0, sizeof sooner);
    memset(gammas, 0, sizeof gammas);
    memset(copies, 0, sizeof copies);
    wall_clock = 0;
}
