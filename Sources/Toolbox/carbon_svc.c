/*
 * CarbonLib's services: what the Printing Manager, Navigation, Scrap,
 * ATSUI and MLTE exports share, their universal procedure pointers, and
 * the one table lookup the Code Fragment Manager asks.
 *
 * CarbonLib's New...UPP calls made routine descriptors on Mac OS 9 (code
 * built for Carbon could still be called from 68k code that way), and
 * Invoke...UPP called through them; here too.  Invoke's arguments are
 * the procedure's, then the UPP.
 */
#include <string.h>

#include "carbon_svc.h"
#include "forks.h"

enum { kM68kISA = 0, kPowerPCISA = 1 };
enum { kCFStringEncodingUnicode = 0x0100 };

/* ---------------------------------------------------------------------- */
/* Universal procedure pointers */

static gaddr new_descriptor(gaddr proc, uint32_t procinfo, int isa)
{
    gaddr rd = tb_NewPtr(32, false, true);
    if (!rd)
        return 0;
    wr16(rd, 0xAAFE); /* _MixedModeMagic */
    wr8(rd + 2, 7);   /* kRoutineDescriptorVersion */
    wr16(rd + 10, 0); /* one routine */
    wr32(rd + 12, procinfo);
    wr8(rd + 17, (uint8_t)isa);
    wr32(rd + 20, proc);
    return rd;
}

uint32_t svc_call_upp(gaddr upp, uint32_t procinfo, const uint32_t *args, int nargs)
{
    if (!upp)
        return 0;
    if (mm_is_descriptor(upp))
        return mm_call(upp, procinfo, args, nargs);
    return ppc_call(upp, args, nargs); /* a transition vector */
}

enum { MAX_WRAPPED = 32 };
static struct {
    gaddr code, upp;
} wrapped[MAX_WRAPPED];
static int nwrapped;

gaddr svc_upp_for_68k(gaddr code, uint32_t procinfo)
{
    if (!code || mm_is_descriptor(code))
        return code;
    for (int i = 0; i < nwrapped; i++)
        if (wrapped[i].code == code)
            return wrapped[i].upp;
    gaddr upp = new_descriptor(code, procinfo, kM68kISA);
    if (upp && nwrapped < MAX_WRAPPED) {
        wrapped[nwrapped].code = code;
        wrapped[nwrapped++].upp = upp;
    }
    return upp;
}

static gaddr new_upp(gaddr proc, uint32_t procinfo) { return proc ? new_descriptor(proc, procinfo, kPowerPCISA) : 0; }

static void dispose_upp(gaddr upp)
{
    if (mm_is_descriptor(upp))
        tb_DisposePtr(upp);
}

static uint32_t invoke(ppc_cpu *c, uint32_t procinfo, int nargs)
{
    uint32_t args[13];
    for (int i = 0; i < nargs; i++)
        args[i] = A(i);
    return svc_call_upp(A(nargs), procinfo, args, nargs);
}

/* New, Dispose and Invoke for each kind: its procInfo and argument count. */
#define UPP_KIND(kind, procinfo, nargs)                                                                                \
    static void new_##kind(ppc_cpu *c) { RET(new_upp(A(0), procinfo)); }                                              \
    static void dispose_##kind(ppc_cpu *c)                                                                             \
    {                                                                                                                  \
        dispose_upp(A(0));                                                                                             \
        DONE();                                                                                                        \
    }                                                                                                                  \
    static void invoke_##kind(ppc_cpu *c) { RET(invoke(c, procinfo, nargs)); }
#define UPP_EXPORTS(kind)                                                                                              \
    { "New" #kind "UPP", new_##kind }, { "Dispose" #kind "UPP", dispose_##kind }, { "Invoke" #kind "UPP", invoke_##kind }

/* Navigation.h */
UPP_KIND(NavEvent, 0x00000FC0, 3)
UPP_KIND(NavPreview, 0x000003D0, 2)
UPP_KIND(NavObjectFilter, 0x00002FD0, 4)
/* PMApplication.h, PMCore.h: pascal, void */
UPP_KIND(PMItem, 0x000002C0, 2)
UPP_KIND(PMPrintDialogInit, 0x000003C0, 2)
UPP_KIND(PMPageSetupDialogInit, 0x000003C0, 2)
UPP_KIND(PMSheetDone, 0x000007C0, 3)
UPP_KIND(PMIdle, 0x00000000, 0)
/* Scrap.h */
UPP_KIND(ScrapPromiseKeeper, 0x00000FF0, 3)
/* MacTextEditor.h */
UPP_KIND(TXNFind, 0x0FFFFFF0, 11)
UPP_KIND(TXNActionKeyMapper, 0x000003F0, 2)
/* ATSUnicode.h: glyph outlines */
UPP_KIND(ATSQuadraticNewPath, 0x000000F0, 1)
UPP_KIND(ATSQuadraticLine, 0x00000FF0, 3)
UPP_KIND(ATSQuadraticCurve, 0x00003FF0, 4)
UPP_KIND(ATSQuadraticClosePath, 0x000000F0, 1)
UPP_KIND(ATSCubicMoveTo, 0x000003F0, 2)
UPP_KIND(ATSCubicLineTo, 0x000003F0, 2)
UPP_KIND(ATSCubicCurveTo, 0x00003FF0, 4)
UPP_KIND(ATSCubicClosePath, 0x000000F0, 1)

static const ppc_export upp_exports[] = {
    UPP_EXPORTS(NavEvent),
    UPP_EXPORTS(NavPreview),
    UPP_EXPORTS(NavObjectFilter),
    UPP_EXPORTS(PMItem),
    UPP_EXPORTS(PMPrintDialogInit),
    UPP_EXPORTS(PMPageSetupDialogInit),
    UPP_EXPORTS(PMSheetDone),
    UPP_EXPORTS(PMIdle),
    UPP_EXPORTS(ScrapPromiseKeeper),
    UPP_EXPORTS(TXNFind),
    UPP_EXPORTS(TXNActionKeyMapper),
    UPP_EXPORTS(ATSQuadraticNewPath),
    UPP_EXPORTS(ATSQuadraticLine),
    UPP_EXPORTS(ATSQuadraticCurve),
    UPP_EXPORTS(ATSQuadraticClosePath),
    UPP_EXPORTS(ATSCubicMoveTo),
    UPP_EXPORTS(ATSCubicLineTo),
    UPP_EXPORTS(ATSCubicCurveTo),
    UPP_EXPORTS(ATSCubicClosePath),
};

/* ---------------------------------------------------------------------- */
/* Core Foundation, through its exports */

uint32_t svc_call_export(const char *name, const uint32_t *args, int nargs)
{
    void (*fn)(ppc_cpu *) = cfm_export_fn(name);
    if (!fn)
        return 0;
    ppc_cpu c = g_ppc;
    for (int i = 0; i < nargs && i < 8; i++)
        c.gpr[3 + i] = args[i];
    c.lr = c.pc;
    fn(&c);
    return c.gpr[3];
}

gaddr svc_cfstring(const uint16_t *u, uint32_t n)
{
    gaddr buf = tb_NewPtr((int32_t)(2 * n + 2), false, true);
    if (!buf)
        return 0;
    for (uint32_t i = 0; i < n; i++)
        wr16(buf + 2 * i, u[i]);
    uint32_t args[5] = { 0, buf, 2 * n, kCFStringEncodingUnicode, 0 };
    gaddr s = svc_call_export("CFStringCreateWithBytes", args, 5);
    tb_DisposePtr(buf);
    return s;
}

gaddr svc_cfstring_pascal(gaddr pstr)
{
    uint8_t b[256];
    uint16_t u[256];
    uint32_t n = pstr ? rd8(pstr) : 0;
    gget(b, pstr + 1, n);
    return svc_cfstring(u, (uint32_t)svc_macroman_to_utf16(b, n, u));
}

uint32_t svc_cfstring_get(gaddr s, uint16_t *out, uint32_t max)
{
    if (!s)
        return 0;
    uint32_t a[1] = { s };
    uint32_t n = svc_call_export("CFStringGetLength", a, 1);
    uint32_t k = n < max ? n : max;
    if (!k || !out)
        return n;
    gaddr buf = tb_NewPtr((int32_t)(2 * k), false, true);
    if (!buf)
        return 0;
    uint32_t g[4] = { s, 0, k, buf }; /* the CFRange in r4 and r5 */
    svc_call_export("CFStringGetCharacters", g, 4);
    for (uint32_t i = 0; i < k; i++)
        out[i] = rd16(buf + 2 * i);
    tb_DisposePtr(buf);
    return n;
}

void svc_cfstring_to_pascal(gaddr s, uint8_t *out)
{
    uint16_t u[255];
    uint32_t n = svc_cfstring_get(s, u, 255);
    if (n > 255)
        n = 255;
    out[0] = (uint8_t)svc_utf16_to_macroman(u, n, out + 1);
}

bool svc_cfstring_equals(gaddr s, const char *ascii)
{
    uint16_t u[256];
    size_t len = strlen(ascii);
    uint32_t n = svc_cfstring_get(s, u, 256);
    if (!s || n != len)
        return false;
    for (size_t i = 0; i < len; i++)
        if (u[i] != (uint8_t)ascii[i])
            return false;
    return true;
}

void svc_cfretain(gaddr cf)
{
    uint32_t a[1] = { cf };
    if (cf)
        svc_call_export("CFRetain", a, 1);
}

void svc_cfrelease(gaddr cf)
{
    uint32_t a[1] = { cf };
    if (cf)
        svc_call_export("CFRelease", a, 1);
}

/* ---------------------------------------------------------------------- */
/* Text and numbers */

size_t svc_utf16_to_macroman(const uint16_t *u, size_t n, uint8_t *out)
{
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = u[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] < 0xE000) {
            out[k++] = '?';
            i++;
            continue;
        }
        if (cp < 0x80) {
            out[k++] = (uint8_t)cp;
            continue;
        }
        char utf8[4] = { (char)(0xE0 | cp >> 12), (char)(0x80 | (cp >> 6 & 0x3F)), (char)(0x80 | (cp & 0x3F)), 0 };
        if (cp < 0x800) {
            utf8[0] = (char)(0xC0 | cp >> 6);
            utf8[1] = (char)(0x80 | (cp & 0x3F));
            utf8[2] = 0;
        }
        uint8_t b[8];
        out[k++] = utf8_to_macroman(utf8, b, sizeof b) == 1 ? b[0] : '?';
    }
    return k;
}

size_t svc_macroman_to_utf16(const uint8_t *b, size_t n, uint16_t *out)
{
    for (size_t i = 0; i < n; i++) {
        if (b[i] < 0x80) {
            out[i] = b[i];
            continue;
        }
        char utf8[8];
        size_t m = macroman_to_utf8(b + i, 1, utf8, sizeof utf8);
        const uint8_t *s = (const uint8_t *)utf8;
        out[i] = m == 2   ? (uint16_t)((s[0] & 0x1F) << 6 | (s[1] & 0x3F))
                 : m == 3 ? (uint16_t)((s[0] & 0x0F) << 12 | (s[1] & 0x3F) << 6 | (s[2] & 0x3F))
                          : '?';
    }
    return n;
}

double svc_rdd(gaddr p)
{
    uint64_t v = (uint64_t)rd32(p) << 32 | rd32(p + 4);
    double d;
    memcpy(&d, &v, 8);
    return d;
}

void svc_wrd(gaddr p, double d)
{
    uint64_t v;
    memcpy(&v, &d, 8);
    wr32(p, (uint32_t)(v >> 32));
    wr32(p + 4, (uint32_t)v);
}

float svc_f32(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

uint32_t svc_f32_bits(float f)
{
    uint32_t bits;
    memcpy(&bits, &f, 4);
    return bits;
}

/* ---------------------------------------------------------------------- */
/* The lookup */

static const struct {
    const ppc_export *table;
    const unsigned *count;
} tables[] = {
    { carbon_print_exports, &carbon_print_export_count }, { carbon_nav_exports, &carbon_nav_export_count },
    { carbon_scrap_exports, &carbon_scrap_export_count }, { carbon_atsui_exports, &carbon_atsui_export_count },
    { carbon_mlte_exports, &carbon_mlte_export_count },
};

void (*carbonsvc_export_fn(const char *name))(ppc_cpu *)
{
    for (unsigned i = 0; i < sizeof upp_exports / sizeof *upp_exports; i++)
        if (!strcmp(upp_exports[i].name, name))
            return upp_exports[i].fn;
    for (unsigned t = 0; t < sizeof tables / sizeof *tables; t++)
        for (unsigned i = 0; i < *tables[t].count; i++)
            if (!strcmp(tables[t].table[i].name, name))
                return tables[t].table[i].fn;
    return NULL;
}

void carbonsvc_reset(void)
{
    nwrapped = 0;
    carbon_print_reset();
    carbon_nav_reset();
    carbon_scrap_reset();
    carbon_atsui_reset();
    carbon_mlte_reset();
}
