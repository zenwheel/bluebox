/*
 * The system's PowerPC libraries that aren't traps: MathLib (the C math
 * functions and SANE's decimal conversions), StdCLib (MPW's C library:
 * strings, memory, formatted output, setjmp), and the InterfaceLib calls
 * that 68k programs had as glue linked into them (GetIndString, c2pstr,
 * EqualString, MemError ...).
 *
 * The PowerPC calling convention: integer and pointer arguments in r3 to
 * r10, floating-point ones in f1 to f13 (each also taking one GPR slot for
 * a float, two for a double), the rest at 24(r1) on; results in r3 or f1.
 */
#include <math.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "forks.h"
#include "legacy.h"
#include "machine.h"
#include "ppc_bridge.h"
#include "qd_internal.h"
#include "tb_internal.h"

double sane_ext_get(gaddr p);
void sane_ext_put(gaddr p, double v);
double sane_dec_get(gaddr d);
void sane_dec_put(gaddr d, gaddr form, double v);

#define R(n) (c->gpr[3 + (n)])
#define F(n) ppc_fpr(c, 1 + (n))
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)
#define RETURN_F(v) do { ppc_set_fpr(c, 1, (v)); c->pc = c->lr; } while (0)

/* Argument word n of a call (GPR slots, then the parameter area). */
static uint32_t arg_word(const ppc_cpu *c, unsigned n)
{
    return n < 8 ? c->gpr[3 + n] : rd32(c->gpr[1] + 24 + 4 * n);
}

static double rd_double(gaddr p)
{
    uint64_t bits = (uint64_t)rd32(p) << 32 | rd32(p + 4);
    double d;
    memcpy(&d, &bits, 8);
    return d;
}

static void wr_double(gaddr p, double d)
{
    uint64_t bits;
    memcpy(&bits, &d, 8);
    wr32(p, (uint32_t)(bits >> 32));
    wr32(p + 4, (uint32_t)bits);
}

static size_t gstrlen(gaddr s)
{
    size_t n = 0;
    while (rd8(s + (gaddr)n))
        n++;
    return n;
}

/* ---------------------------------------------------------------------- */
/* MathLib */

#define UNARY(name, expr) \
    static void m_##name(ppc_cpu *c) { double x = F(0); (void)x; RETURN_F(expr); }
#define BINARY(name, expr) \
    static void m_##name(ppc_cpu *c) { double x = F(0), y = F(1); (void)y; RETURN_F(expr); }

UNARY(sin, sin(x))
UNARY(cos, cos(x))
UNARY(tan, tan(x))
UNARY(asin, asin(x))
UNARY(acos, acos(x))
UNARY(atan, atan(x))
UNARY(sinh, sinh(x))
UNARY(cosh, cosh(x))
UNARY(tanh, tanh(x))
UNARY(asinh, asinh(x))
UNARY(acosh, acosh(x))
UNARY(atanh, atanh(x))
UNARY(exp, exp(x))
UNARY(exp2, exp2(x))
UNARY(expm1, expm1(x))
UNARY(log, log(x))
UNARY(log2, log2(x))
UNARY(log10, log10(x))
UNARY(log1p, log1p(x))
UNARY(logb, logb(x))
UNARY(sqrt, sqrt(x))
UNARY(cbrt, cbrt(x))
UNARY(fabs, fabs(x))
UNARY(floor, floor(x))
UNARY(ceil, ceil(x))
UNARY(trunc, trunc(x))
UNARY(round, round(x))
UNARY(rint, rint(x))
UNARY(nearbyint, nearbyint(x))
UNARY(erf, erf(x))
UNARY(erfc, erfc(x))
UNARY(gamma, tgamma(x))
UNARY(lgamma, lgamma(x))
BINARY(pow, pow(x, y))
BINARY(atan2, atan2(x, y))
BINARY(fmod, fmod(x, y))
BINARY(hypot, hypot(x, y))
BINARY(fmin, fmin(x, y))
BINARY(fmax, fmax(x, y))
BINARY(fdim, fdim(x, y))
BINARY(remainder, remainder(x, y))
BINARY(copysign, copysign(x, y))
BINARY(nextafterd, nextafter(x, y))
/* compound(rate, periods) = (1 + rate)^periods; annuity = (1 - (1 + rate)^-periods) / rate */
BINARY(compound, pow(1 + x, y))
BINARY(annuity, x == 0 ? y : (1 - pow(1 + x, -y)) / x)

/* The double after a double argument: its GPR slots are 0 and 1, so an int
 * or pointer after it is in r5. */
static void m_ldexp(ppc_cpu *c) { RETURN_F(ldexp(F(0), (int32_t)R(2))); }
static void m_scalb(ppc_cpu *c) { RETURN_F(scalbn(F(0), (int32_t)R(2))); }
static void m_frexp(ppc_cpu *c)
{
    int e;
    double m = frexp(F(0), &e);
    if (R(2))
        wr32(R(2), (uint32_t)e);
    RETURN_F(m);
}
static void m_modf(ppc_cpu *c)
{
    double ip;
    double f = modf(F(0), &ip);
    if (R(2))
        wr_double(R(2), ip);
    RETURN_F(f);
}
static void m_remquo(ppc_cpu *c)
{
    int q;
    double r = remquo(F(0), F(1), &q);
    if (R(4))
        wr32(R(4), (uint32_t)q);
    RETURN_F(r);
}
static void m_rinttol(ppc_cpu *c) { RETURN((int32_t)lrint(F(0))); }
static void m_roundtol(ppc_cpu *c) { RETURN((int32_t)lround(F(0))); }
static void m_inf(ppc_cpu *c) { RETURN_F(INFINITY); }
static void m_isnand(ppc_cpu *c) { RETURN(isnan(F(0)) ? 1 : 0); }
static void m_isfinited(ppc_cpu *c) { RETURN(isfinite(F(0)) ? 1 : 0); }
static void m_isnormald(ppc_cpu *c) { RETURN(isnormal(F(0)) ? 1 : 0); }
static void m_signbitd(ppc_cpu *c) { RETURN(signbit(F(0)) ? 1 : 0); }
/* MathLib's classes: FP_SNAN 0, FP_QNAN 1, FP_INFINITE 2, FP_ZERO 3,
 * FP_NORMAL 4, FP_SUBNORMAL 5. */
static void m_fpclassifyd(ppc_cpu *c)
{
    double x = F(0);
    int k = isnan(x) ? 1 : isinf(x) ? 2 : x == 0 ? 3 : isnormal(x) ? 4 : 5;
    RETURN(k);
}
/* randomx(double *x): the next of the Lehmer sequence, as SANE has it. */
static void m_randomx(ppc_cpu *c)
{
    double x = rd_double(R(0));
    x = fmod(16807.0 * x, 2147483647.0);
    wr_double(R(0), x);
    RETURN_F(x);
}
static void m_x80tod(ppc_cpu *c) { RETURN_F(sane_ext_get(R(0))); }
static void m_dtox80(ppc_cpu *c)
{
    sane_ext_put(R(1), rd_double(R(0)));
    c->pc = c->lr;
}
/* ldtox80(const long double *, extended80 *): MathLib's long double is a
 * pair of doubles; their sum is close enough. */
static void m_ldtox80(ppc_cpu *c)
{
    sane_ext_put(R(1), rd_double(R(0)) + rd_double(R(0) + 8));
    c->pc = c->lr;
}
static void m_x80told(ppc_cpu *c)
{
    wr_double(R(1), sane_ext_get(R(0)));
    wr_double(R(1) + 8, 0);
    c->pc = c->lr;
}

/* num2dec(const decform *f, double x, decimal *d): x takes r4 and r5. */
static void m_num2dec(ppc_cpu *c)
{
    sane_dec_put(R(3), R(0), F(0));
    c->pc = c->lr;
}
static void m_dec2num(ppc_cpu *c) { RETURN_F(sane_dec_get(R(0))); }
/* dec2str(const decform *f, const decimal *d, char *s): a C string. */
static void m_dec2str(ppc_cpu *c)
{
    gaddr tmp = tb_NewPtr(256, false, true);
    if (tmp) {
        tb_ROMlib_Fdec2str(R(0), R(1), tmp);
        uint8_t n = rd8(tmp);
        ppc_copy(R(2), tmp + 1, n);
        wr8(R(2) + n, 0);
        tb_DisposePtr(tmp);
    }
    c->pc = c->lr;
}
/* str2dec(const char *s, short *ix, decimal *d, short *vp) */
static void m_str2dec(ppc_cpu *c)
{
    gaddr valid = tb_NewPtr(2, true, true);
    tb_ROMlib_Fcstr2dec(R(0), R(1), R(2), valid);
    if (R(3))
        wr16(R(3), rd8(valid));
    tb_DisposePtr(valid);
    c->pc = c->lr;
}

/* ---------------------------------------------------------------------- */
/* InterfaceLib glue */

/* Pascal and C strings */
static void g_c2pstr(ppc_cpu *c)
{
    gaddr s = R(0);
    size_t n = gstrlen(s);
    if (n > 255)
        n = 255;
    gmemcpy(s + 1, s, (uint32_t)n);
    wr8(s, (uint8_t)n);
    RETURN(s);
}
static void g_p2cstr(ppc_cpu *c)
{
    gaddr s = R(0);
    uint8_t n = rd8(s);
    gmemcpy(s, s + 1, n);
    wr8(s + n, 0);
    RETURN(s);
}
/* c2pstrcpy(dst, src) / CopyCStringToPascal(src, dst) */
static void c_to_p(gaddr src, gaddr dst)
{
    size_t n = gstrlen(src);
    if (n > 255)
        n = 255;
    gmemcpy(dst + 1, src, (uint32_t)n);
    wr8(dst, (uint8_t)n);
}
static void p_to_c(gaddr src, gaddr dst)
{
    uint8_t n = rd8(src);
    gmemcpy(dst, src + 1, n);
    wr8(dst + n, 0);
}
static void g_c2pstrcpy(ppc_cpu *c) { c_to_p(R(1), R(0)); c->pc = c->lr; }
static void g_p2cstrcpy(ppc_cpu *c) { p_to_c(R(1), R(0)); c->pc = c->lr; }
static void g_CopyCStringToPascal(ppc_cpu *c) { c_to_p(R(0), R(1)); c->pc = c->lr; }
static void g_CopyPascalStringToC(ppc_cpu *c) { p_to_c(R(0), R(1)); c->pc = c->lr; }

/* GetIndString(theString, strListID, index) */
static void g_GetIndString(ppc_cpu *c)
{
    gaddr out = R(0);
    int16_t index = (int16_t)R(2);
    wr8(out, 0);
    gaddr h = tb_GetResource((int32_t)fourcc("STR#"), (int16_t)R(1));
    if (h && rd32(h) && index >= 1) {
        gaddr p = rd32(h);
        uint16_t count = rd16(p);
        if (index <= count) {
            p += 2;
            for (int i = 1; i < index; i++)
                p += 1u + rd8(p);
            ppc_copy(out, p, 1u + rd8(p));
        }
    }
    c->pc = c->lr;
}

/* The string comparisons, through _CmpString, _RelString and _UprString:
 * bit 9 of the trap ignores diacriticals, bit 10 respects case. */
static uint32_t compare_trap(uint16_t trap, gaddr a, gaddr b, bool case_sens, bool diac_sens)
{
    ppc_trap_begin();
    ppc_trap_set(8, a + 1);
    ppc_trap_set(9, b + 1);
    ppc_trap_set(0, (uint32_t)rd8(a) << 16 | rd8(b));
    ppc_trap((uint16_t)(trap | (case_sens ? 0x0400 : 0) | (diac_sens ? 0 : 0x0200)));
    uint32_t r = ppc_trap_get(0);
    ppc_trap_end();
    return r;
}
static void g_EqualString(ppc_cpu *c) { RETURN(compare_trap(0xA03C, R(0), R(1), R(2) & 0xFF, R(3) & 0xFF) == 0); }
static void g_RelString(ppc_cpu *c)
{
    RETURN((int32_t)(int16_t)compare_trap(0xA050, R(0), R(1), R(2) & 0xFF, R(3) & 0xFF));
}
static void g_IUEqualString(ppc_cpu *c) { RETURN(compare_trap(0xA03C, R(0), R(1), false, true) == 0 ? 0 : 1); }
static void g_IUCompString(ppc_cpu *c)
{
    RETURN((int32_t)(int16_t)compare_trap(0xA050, R(0), R(1), false, true));
}
static void g_UpperString(ppc_cpu *c)
{
    gaddr s = R(0);
    ppc_trap_begin();
    ppc_trap_set(8, s + 1);
    ppc_trap_set(0, rd8(s));
    ppc_trap((uint16_t)(0xA054 | ((R(1) & 0xFF) ? 0 : 0x0200)));
    ppc_trap_end();
    c->pc = c->lr;
}

/* UpperText(text, len) / UppercaseText(text, len, script) and the lower ones:
 * by the script's 'itl2' tables (intl.c) */
static void g_UpperText(ppc_cpu *c)
{
    intl_convert_text(R(0), (int16_t)R(1), true, false);
    c->pc = c->lr;
}
static void g_LowerText(ppc_cpu *c)
{
    intl_convert_text(R(0), (int16_t)R(1), false, false);
    c->pc = c->lr;
}

/* Low-memory queue headers and globals that have accessor functions. */
static void g_GetEvQHdr(ppc_cpu *c) { RETURN(LM_EventQueue); }
static void g_GetVCBQHdr(ppc_cpu *c) { RETURN(LM_VCBQHdr); }
static void g_GetDrvQHdr(ppc_cpu *c) { RETURN(LM_DrvQHdr); }
static void g_GetFSQHdr(ppc_cpu *c) { RETURN(LM_FSQHdr); }
static void g_GetVBLQHdr(ppc_cpu *c) { RETURN(LM_VBLQueue); }
static void g_GetApplLimit(ppc_cpu *c) { RETURN(rd32(LM_ApplLimit)); }
static void g_GetGrayRgn(ppc_cpu *c) { RETURN(rd32(LM_GrayRgn)); }
static void g_GetAppFont(ppc_cpu *c) { RETURN((int32_t)(int16_t)rd16(LM_ApFontID)); }
static void g_GetSysFont(ppc_cpu *c) { RETURN((int32_t)(int16_t)rd16(LM_SysFontFam)); }
static void g_GetDefFontSize(ppc_cpu *c) { RETURN(12); }
/* GetTime(DateTimeRec *) */
static void g_GetTime(ppc_cpu *c)
{
    tb_SecondsToDate(rd32(LM_Time), R(0));
    c->pc = c->lr;
}
/* Microseconds(UnsignedWide *): _Microseconds leaves the high half in A0. */
static void g_Microseconds(ppc_cpu *c)
{
    ppc_trap_begin();
    ppc_trap(0xA193);
    uint32_t hi = ppc_trap_get(8), lo = ppc_trap_get(0);
    ppc_trap_end();
    if (R(0)) {
        wr32(R(0), hi);
        wr32(R(0) + 4, lo);
    }
    c->pc = c->lr;
}
/* ReadLocation(MachineLocation *): WriteLocation's, or Greenwich with no
 * daylight saving. */
static uint8_t location[12];
static void g_ReadLocation(ppc_cpu *c)
{
    if (R(0))
        gput(R(0), location, sizeof location);
    c->pc = c->lr;
}
/* Control/Status(refNum, csCode, csParamPtr) and KillIO(refNum): the
 * device calls with a parameter block made here. */
static int16_t device_call(int16_t (*fn)(gaddr, bool), int16_t ref, int16_t code, gaddr params)
{
    gaddr pb = tb_NewPtr(80, true, true);
    wr16(pb + 24, (uint16_t)ref);  /* ioCRefNum */
    wr16(pb + 26, (uint16_t)code); /* csCode */
    if (params)
        ppc_copy(pb + 28, params, 22); /* csParam */
    int16_t e = fn(pb, false);
    if (params)
        ppc_copy(params, pb + 28, 22);
    tb_DisposePtr(pb);
    return e;
}
static void g_Control(ppc_cpu *c) { RETURN((int32_t)device_call(tb_PBControl, (int16_t)R(0), (int16_t)R(1), R(2))); }
static void g_Status(ppc_cpu *c) { RETURN((int32_t)device_call(tb_PBStatus, (int16_t)R(0), (int16_t)R(1), R(2))); }
static void g_KillIO(ppc_cpu *c) { RETURN((int32_t)device_call(tb_PBKillIO, (int16_t)R(0), 0, 0)); }
/* CountDITL(dialog): the item list's count is one less than the number. */
static void g_CountDITL(ppc_cpu *c)
{
    gaddr items = R(0) ? rd32(R(0) + DialogRecord_items_off) : 0;
    RETURN(items && rd32(items) ? (int32_t)(int16_t)rd16(rd32(items)) + 1 : 0);
}
/* StdFilterProc(dialog, event, itemHit): ModalDialog does the standard
 * filtering itself, so here nothing is handled. */
static void g_StdFilterProc(ppc_cpu *c) { RETURN(0); }

/* A one-register OS trap: A0 = a; returns D0. */
static uint32_t a0_trap(uint16_t trap, gaddr a)
{
    ppc_trap_begin();
    ppc_trap_set(8, a);
    ppc_trap(trap);
    uint32_t r = ppc_trap_get(0);
    ppc_trap_end();
    return r;
}

static void g_MemError(ppc_cpu *c) { RETURN((int32_t)(int16_t)rd16(LM_MemErr)); }
static void g_GetMBarHeight(ppc_cpu *c) { RETURN((int32_t)(int16_t)rd16(LM_MBarHeight)); }
static void g_ApplicationZone(ppc_cpu *c) { RETURN(rd32(LM_ApplZone)); }
static void g_SystemZone(ppc_cpu *c) { RETURN(rd32(LM_SysZone)); }
static void g_HLockHi(ppc_cpu *c)
{
    a0_trap(0xA064, R(0)); /* MoveHHi */
    a0_trap(0xA029, R(0)); /* HLock */
    c->pc = c->lr;
}
/* The 68k world's A5: PowerPC code keeps its globals elsewhere. */
static void g_SetCurrentA5(ppc_cpu *c)
{
    g_cpu->a[5] = rd32(LM_CurrentA5);
    RETURN(g_cpu->a[5]);
}
static void g_SetA5(ppc_cpu *c)
{
    uint32_t old = g_cpu->a[5];
    g_cpu->a[5] = R(0);
    RETURN(old);
}
static void g_nothing(ppc_cpu *c) { c->pc = c->lr; }
static void g_noErr(ppc_cpu *c) { RETURN(0); }

/* MakeDataExecutable(address, length): code was written there; the JIT
 * drops what it translated from that range. */
static void g_make_data_executable(ppc_cpu *c)
{
    if (g_code_map && R(1))
        mem_code_range(R(0), R(1));
    c->pc = c->lr;
}

/* Trap addresses: _GetTrapAddress with the new-style bit, and the
 * Toolbox bit for Toolbox traps. */
static uint32_t trap_address(uint16_t trap, bool toolbox)
{
    ppc_trap_begin();
    ppc_trap_set(0, trap);
    ppc_trap((uint16_t)(0xA346 | (toolbox ? 0x0400 : 0)));
    uint32_t r = ppc_trap_get(8);
    ppc_trap_end();
    return r;
}
static void set_trap_address(uint32_t addr, uint16_t trap, bool toolbox)
{
    ppc_trap_begin();
    ppc_trap_set(8, addr);
    ppc_trap_set(0, trap);
    ppc_trap((uint16_t)(0xA247 | (toolbox ? 0x0400 : 0)));
    ppc_trap_end();
}
/* TrapType: OSTrap 0, ToolTrap 1. */
static void g_NGetTrapAddress(ppc_cpu *c) { RETURN(trap_address((uint16_t)R(0), (R(1) & 0xFF) == 1)); }
static void g_NSetTrapAddress(ppc_cpu *c)
{
    set_trap_address(R(0), (uint16_t)R(1), (R(2) & 0xFF) == 1);
    c->pc = c->lr;
}
static void g_GetToolboxTrapAddress(ppc_cpu *c) { RETURN(trap_address((uint16_t)R(0), true)); }
static void g_GetOSTrapAddress(ppc_cpu *c) { RETURN(trap_address((uint16_t)R(0), false)); }
static void g_SetToolboxTrapAddress(ppc_cpu *c)
{
    set_trap_address(R(0), (uint16_t)R(1), true);
    c->pc = c->lr;
}
static void g_SetOSTrapAddress(ppc_cpu *c)
{
    set_trap_address(R(0), (uint16_t)R(1), false);
    c->pc = c->lr;
}
/* GetTrapAddress(trap): the old call, Toolbox numbers above $A800 by the trap word. */
static void g_GetTrapAddress(ppc_cpu *c)
{
    uint16_t t = (uint16_t)R(0);
    RETURN(trap_address(t, (t & 0x0800) != 0));
}

static void g_SetSoundVol(ppc_cpu *c)
{
    wr8(LM_SdVolume, (uint8_t)(R(0) & 7));
    c->pc = c->lr;
}
static void g_GetSoundVol(ppc_cpu *c)
{
    if (R(0))
        wr16(R(0), rd8(LM_SdVolume) & 7);
    c->pc = c->lr;
}

/* Fixed-point to and from double. */
static void g_Fix2X(ppc_cpu *c) { RETURN_F((int32_t)R(0) / 65536.0); }
static void g_Frac2X(ppc_cpu *c) { RETURN_F((int32_t)R(0) / 1073741824.0); }
static int32_t to_fixed(double v, double scale)
{
    v *= scale;
    if (isnan(v))
        return 0;
    if (v >= 2147483647.0)
        return 0x7FFFFFFF;
    if (v <= -2147483648.0)
        return (int32_t)0x80000000;
    return (int32_t)lrint(v);
}
static void g_X2Fix(ppc_cpu *c) { RETURN(to_fixed(F(0), 65536.0)); }
static void g_X2Frac(ppc_cpu *c) { RETURN(to_fixed(F(0), 1073741824.0)); }

/* The dialog editing calls: TextEdit on the dialog's edit field. */
static void dialog_te(ppc_cpu *c, void (*fn)(gaddr))
{
    gaddr d = R(0);
    if (d && (int16_t)rd16(d + DialogRecord_editField_off) >= 0 && rd32(d + DialogRecord_textH_off))
        fn(rd32(d + DialogRecord_textH_off));
    c->pc = c->lr;
}
static void g_DialogCut(ppc_cpu *c) { dialog_te(c, tb_TECut); }
static void g_DialogCopy(ppc_cpu *c) { dialog_te(c, tb_TECopy); }
static void g_DialogPaste(ppc_cpu *c) { dialog_te(c, tb_TEPaste); }
static void g_DialogDelete(ppc_cpu *c) { dialog_te(c, tb_TEDelete); }

/* GetWDInfo(wdRefNum, *vRefNum, *dirID, *procID) */
static void g_GetWDInfo(ppc_cpu *c)
{
    gaddr pb = tb_NewPtr(128, true, true);
    wr16(pb + 24, (uint16_t)R(0)); /* ioVRefNum */
    int16_t e = tb_PBGetWDInfo(pb, false);
    if (R(1))
        wr16(R(1), rd16(pb + 32)); /* ioWDVRefNum */
    if (R(2))
        wr32(R(2), rd32(pb + 48)); /* ioWDDirID */
    if (R(3))
        wr32(R(3), rd32(pb + 28)); /* ioWDProcID */
    tb_DisposePtr(pb);
    RETURN((int32_t)e);
}
/* OpenDriver(name, *refNum) / CloseDriver(refNum) */
static void g_OpenDriver(ppc_cpu *c)
{
    gaddr pb = tb_NewPtr(128, true, true);
    wr32(pb + 18, R(0)); /* ioNamePtr */
    int16_t e = tb_PBOpen(pb, false);
    if (R(1))
        wr16(R(1), rd16(pb + 24)); /* ioRefNum */
    tb_DisposePtr(pb);
    RETURN((int32_t)e);
}
static void g_CloseDriver(ppc_cpu *c)
{
    gaddr pb = tb_NewPtr(128, true, true);
    wr16(pb + 24, (uint16_t)R(0));
    int16_t e = tb_PBClose(pb, false);
    tb_DisposePtr(pb);
    RETURN((int32_t)e);
}
static void g_GetDCtlEntry(ppc_cpu *c)
{
    int16_t ref = (int16_t)R(0);
    gaddr table = rd32(LM_UTableBase);
    RETURN(table && ref < 0 ? rd32(table + 4 * (gaddr)(-ref - 1)) : 0);
}

/* The Component Manager: _ComponentDispatch with the selector in D0,
 * Pascal arguments (sizes 4 or 2, as a string) and result (0, 2, 4). */
typedef struct {
    uint8_t selector;
    const char *args;
    int result;
} component_call;

static void component_manager(ppc_cpu *c, const component_call *k)
{
    ppc_trap_begin();
    if (k->result)
        ppc_trap_push(k->result, 0);
    for (int i = 0; k->args[i]; i++)
        ppc_trap_push(k->args[i] - '0', R(i));
    ppc_trap_set(0, k->selector);
    ppc_trap(0xA82A);
    uint32_t r = k->result ? ppc_trap_result(k->result) : 0;
    ppc_trap_end();
    if (k->result == 2)
        r = (uint32_t)(int32_t)(int16_t)r;
    RETURN(r);
}

#define COMPONENT(name, sel, args, result) \
    static void cm_##name(ppc_cpu *c) { static const component_call k = { sel, args, result }; component_manager(c, &k); }
COMPONENT(RegisterComponent, 0x01, "444244", 4)
COMPONENT(UnregisterComponent, 0x02, "4", 2)
COMPONENT(CountComponents, 0x03, "4", 4)
COMPONENT(FindNextComponent, 0x04, "44", 4)
COMPONENT(GetComponentInfo, 0x05, "44444", 2)
COMPONENT(GetComponentListModSeed, 0x06, "", 4)
COMPONENT(OpenComponent, 0x07, "4", 4)
COMPONENT(CloseComponent, 0x08, "4", 2)
COMPONENT(GetComponentInstanceError, 0x0A, "4", 2)
COMPONENT(SetComponentInstanceError, 0x0B, "42", 0)
COMPONENT(GetComponentInstanceStorage, 0x0C, "4", 4)
COMPONENT(SetComponentInstanceStorage, 0x0D, "44", 0)
COMPONENT(GetComponentInstanceA5, 0x0E, "4", 4)
COMPONENT(SetComponentInstanceA5, 0x0F, "44", 0)
COMPONENT(GetComponentRefcon, 0x10, "4", 4)
COMPONENT(SetComponentRefcon, 0x11, "44", 0)
COMPONENT(RegisterComponentResource, 0x12, "42", 4)
COMPONENT(CountComponentInstances, 0x13, "4", 4)
COMPONENT(RegisterComponentResourceFile, 0x14, "22", 4)
COMPONENT(OpenComponentResFile, 0x15, "4", 2)
COMPONENT(CloseComponentResFile, 0x18, "2", 2)
COMPONENT(CaptureComponent, 0x1C, "44", 4)
COMPONENT(UncaptureComponent, 0x1D, "4", 2)
COMPONENT(SetDefaultComponent, 0x1E, "42", 2)
COMPONENT(ResolveComponentAlias, 0x20, "4", 4)
COMPONENT(OpenDefaultComponent, 0x21, "44", 4)
COMPONENT(DelegateComponentCall, 0x24, "44", 4)
COMPONENT(GetComponentIconSuite, 0x29, "44", 2)
COMPONENT(GetComponentTypeModSeed, 0x2C, "4", 4)
COMPONENT(OpenAComponent, 0x2D, "44", 2)
COMPONENT(OpenADefaultComponent, 0x2E, "444", 2)
COMPONENT(OpenAComponentResFile, 0x2F, "44", 2)
COMPONENT(GetComponentResource, 0x35, "4424", 2)
COMPONENT(GetComponentIndString, 0x36, "4422", 2)
#undef COMPONENT

/* Component calls (ComponentCallNow): the instance and the parameters
 * (sizes 4, 2, or 1 for a Boolean or char pushed as a word), then the
 * parameter size and selector; _ComponentDispatch with D0 = 0. */
static void instance_call(ppc_cpu *c, int16_t what, const char *args)
{
    ppc_trap_begin();
    ppc_trap_push(4, 0);
    ppc_trap_push(4, R(0));
    uint32_t size = 0;
    for (int i = 0; args[i]; i++) {
        int n = args[i] - '0';
        ppc_trap_push(n, R(1 + i));
        size += n == 4 ? 4 : 2;
    }
    ppc_trap_push(4, size << 16 | (uint16_t)what);
    ppc_trap_set(0, 0);
    ppc_trap(0xA82A);
    uint32_t r = ppc_trap_result(4);
    ppc_trap_end();
    RETURN(r);
}

#define CCALL(name, what, args) static void cc_##name(ppc_cpu *c) { instance_call(c, what, args); }
CCALL(MCSetMovie, 2, "444")
CCALL(MCRemoveAMovie, 3, "4")
CCALL(MCGetIndMovie, 5, "2")
CCALL(MCRemoveAllMovies, 6, "")
CCALL(MCIsPlayerEvent, 7, "4")
CCALL(MCSetActionFilter, 8, "4")
CCALL(MCDoAction, 9, "24")
CCALL(MCSetControllerAttached, 10, "1")
CCALL(MCIsControllerAttached, 11, "")
CCALL(MCSetControllerPort, 12, "4")
CCALL(MCGetControllerPort, 13, "")
CCALL(MCSetVisible, 14, "1")
CCALL(MCGetVisible, 15, "")
CCALL(MCGetControllerBoundsRect, 16, "4")
CCALL(MCSetControllerBoundsRect, 17, "4")
CCALL(MCGetControllerBoundsRgn, 18, "")
CCALL(MCGetWindowRgn, 19, "4")
CCALL(MCMovieChanged, 20, "4")
CCALL(MCSetDuration, 21, "4")
CCALL(MCGetCurrentTime, 22, "4")
CCALL(MCNewAttachedController, 23, "444")
CCALL(MCDraw, 24, "4")
CCALL(MCActivate, 25, "41")
CCALL(MCIdle, 26, "")
CCALL(MCKey, 27, "14")
CCALL(MCClick, 28, "4444")
CCALL(MCEnableEditing, 29, "1")
CCALL(MCIsEditingEnabled, 30, "")
CCALL(MCCopy, 31, "")
CCALL(MCCut, 32, "")
CCALL(MCPaste, 33, "4")
CCALL(MCClear, 34, "")
CCALL(MCUndo, 35, "")
CCALL(MCPositionController, 36, "444")
CCALL(MCGetControllerInfo, 37, "4")
CCALL(MCSetClip, 40, "44")
CCALL(MCGetClip, 41, "44")
CCALL(MCDrawBadge, 42, "44")
CCALL(MCSetUpEditMenu, 43, "44")
CCALL(MCGetMenuString, 44, "424")
CCALL(MCSetActionFilterWithRefCon, 45, "44")
CCALL(MCPtInController, 46, "44")
CCALL(MCInvalidate, 47, "44")
CCALL(MCAdjustCursor, 48, "444")
CCALL(MCGetInterfaceElement, 49, "44")
/* The QuickTime Music Architecture's tune player and note allocator (qt_tune.c). */
CCALL(TuneSetHeader, 4, "4")
CCALL(TuneGetTimeBase, 5, "4")
CCALL(TuneSetTimeScale, 6, "4")
CCALL(TuneGetTimeScale, 7, "4")
CCALL(TuneGetIndexedNoteChannel, 8, "44")
CCALL(TuneQueue, 10, "4444444")
CCALL(TuneInstant, 11, "44")
CCALL(TuneGetStatus, 12, "4")
CCALL(TuneStop, 13, "4")
CCALL(TuneSetVolume, 16, "4")
CCALL(TuneGetVolume, 17, "")
CCALL(TunePreroll, 18, "")
CCALL(TuneUnroll, 19, "")
CCALL(TuneSetNoteChannels, 20, "4444")
CCALL(TuneSetPartTranspose, 21, "444")
CCALL(TuneGetNoteAllocator, 23, "")
CCALL(TuneSetSofter, 24, "4")
CCALL(TuneTask, 25, "")
CCALL(TuneSetBalance, 26, "4")
CCALL(TuneSetSoundLocalization, 27, "4")
CCALL(TuneSetHeaderWithSize, 28, "44")
CCALL(TuneSetPartMix, 29, "4444")
CCALL(TuneGetPartMix, 30, "4444")
CCALL(NARegisterMusicDevice, 0, "444")
CCALL(NAUnregisterMusicDevice, 1, "4")
CCALL(NAGetRegisteredMusicDevice, 2, "44444")
CCALL(NASaveMusicConfiguration, 3, "")
CCALL(NANewNoteChannel, 4, "44")
CCALL(NADisposeNoteChannel, 5, "4")
CCALL(NAGetNoteChannelInfo, 6, "444")
CCALL(NAPrerollNoteChannel, 7, "4")
CCALL(NAUnrollNoteChannel, 8, "4")
CCALL(NASetNoteChannelVolume, 11, "44")
CCALL(NAResetNoteChannel, 12, "4")
CCALL(NAPlayNote, 13, "444")
CCALL(NASetController, 14, "444")
CCALL(NASetKnob, 15, "444")
CCALL(NAFindNoteChannelTone, 16, "444")
CCALL(NASetInstrumentNumber, 17, "44")
CCALL(NAPickInstrument, 18, "4444444")
CCALL(NAPickArrangement, 19, "444444")
CCALL(NASetDefaultMIDIInput, 21, "4")
CCALL(NAGetDefaultMIDIInput, 22, "4")
CCALL(NAUseDefaultMIDIInput, 25, "444")
CCALL(NALoseDefaultMIDIInput, 26, "")
CCALL(NAStuffToneDescription, 27, "44")
CCALL(NACopyrightDialog, 28, "4444444")
CCALL(NAGetIndNoteChannel, 31, "444")
CCALL(NAGetMIDIPorts, 33, "44")
CCALL(NAGetNoteRequest, 34, "44")
CCALL(NASendMIDI, 35, "44")
CCALL(NAPickEditInstrument, 36, "444444")
CCALL(NANewNoteChannelFromAtomicInstrument, 37, "444")
CCALL(NASetAtomicInstrument, 38, "444")
CCALL(NAGetKnob, 40, "444")
CCALL(NATask, 41, "")
CCALL(NASetNoteChannelBalance, 42, "44")
CCALL(NASetInstrumentNumberInterruptSafe, 43, "44")
CCALL(NASetNoteChannelSoundLocalization, 44, "44")
CCALL(NAGetController, 45, "444")
#undef CCALL

/* FindWordBreaks(text, len, offset, leadingEdge, breaks, offsets, script):
 * FindWord's, the script aside. */
static void g_FindWordBreaks(ppc_cpu *c)
{
    tb_FindWord(R(0), (int16_t)R(1), (int16_t)R(2), (int8_t)R(3), R(4), R(5));
    c->pc = c->lr;
}

/* UppercaseStripDiacritics(text, len, script): in place. */
static void g_UppercaseStripDiacritics(ppc_cpu *c)
{
    intl_convert_text(R(0), (int16_t)R(1), true, true);
    c->pc = c->lr;
}

/* PBControlImmed(pb): a Control call, done at once like the others. */
static void g_PBControlImmed(ppc_cpu *c) { RETURN((uint32_t)(int32_t)tb_PBControl(R(0), false)); }

/* TruncText(width, textPtr, short *length, truncWhere): the text cut to
 * fit width with an ellipsis, as TruncString cuts a Pascal string. */

/* The Script Manager's older names: GetScript and SetScript are
 * Get/SetScriptVariable, GetEnvirons and SetEnvirons the Script Manager
 * variables (the same traps and selectors). */
static void g_GetScript(ppc_cpu *c) { RETURN((uint32_t)tb_GetScriptVariable((int16_t)R(0), (int16_t)R(1))); }
static void g_SetScript(ppc_cpu *c)
{
    RETURN((uint32_t)(int32_t)tb_SetScriptVariable((int16_t)R(0), (int16_t)R(1), (int32_t)R(2)));
}
static void g_GetEnvirons(ppc_cpu *c) { RETURN((uint32_t)tb_GetScriptManagerVariable((int16_t)R(0))); }
static void g_SetEnvirons(ppc_cpu *c)
{
    RETURN((uint32_t)(int32_t)tb_SetScriptManagerVariable((int16_t)R(0), (int32_t)R(1)));
}


static void g_TruncText(ppc_cpu *c)
{
    RETURN((uint32_t)(int32_t)tb_TruncText((int16_t)R(0), R(1), R(2), (int16_t)R(3)));
}

/* GetDefaultUser(&userRef, name): Sharing Setup's owner, the host
 * account's full name (Netscape Navigator names its profile after it). */
static void g_GetDefaultUser(ppc_cpu *c)
{
    const struct passwd *pw = getpwuid(getuid());
    const char *name = pw && pw->pw_gecos && *pw->pw_gecos ? pw->pw_gecos : pw ? pw->pw_name : "User";
    uint8_t mac[256];
    size_t n = utf8_to_macroman(name, mac, strlen(name));
    if (n > 31)
        n = 31;
    if (R(0))
        wr32(R(0), 1); /* the default user's identity (ppctoolbox.c) */
    if (R(1)) {
        wr8(R(1), (uint8_t)n);
        gput(R(1) + 1, mac, (uint32_t)n);
    }
    RETURN(0);
}


/* Internet Config: an instance (the preferences are components.c's). */
static void ic_ICStart(ppc_cpu *c)
{
    if (R(0))
        wr32(R(0), 0x49430001); /* any non-zero instance */
    RETURN(0);
}
static void ic_prefNotFound(ppc_cpu *c) { RETURN((uint32_t)(int32_t)-666); }
static void ic_FindConfigFile(ppc_cpu *c)
{
    ic_make_file();
    RETURN(0);
}
/* The preferences, after the instance in r3: */
static void ic_GetPref(ppc_cpu *c) { RETURN((uint32_t)(int32_t)ic_get_pref(R(1), R(2), R(3), R(4))); }
static void ic_SetPref(ppc_cpu *c) { RETURN((uint32_t)(int32_t)ic_set_pref(R(1), R(3), (int32_t)R(4))); }
static void ic_GetPrefHandle(ppc_cpu *c) { RETURN((uint32_t)(int32_t)ic_get_pref_handle(R(1), R(2), R(3), 0)); }
static void ic_FindPrefHandle(ppc_cpu *c) { RETURN((uint32_t)(int32_t)ic_get_pref_handle(R(1), R(2), 0, R(3))); }
static void ic_SetPrefHandle(ppc_cpu *c)
{
    gaddr h = R(3);
    RETURN((uint32_t)(int32_t)(h && rd32(h) ? ic_set_pref(R(1), rd32(h), tb_GetHandleSize(h)) : paramErr));
}
static void ic_DeletePref(ppc_cpu *c) { RETURN((uint32_t)(int32_t)ic_delete_pref(R(1))); }
static void ic_GetIndPref(ppc_cpu *c) { RETURN((uint32_t)(int32_t)ic_get_ind_pref((int32_t)R(1), R(2))); }
static void ic_CountPref(ppc_cpu *c)
{
    if (R(1))
        wr32(R(1), (uint32_t)ic_count_prefs());
    RETURN(0);
}
static void ic_GetSeed(ppc_cpu *c)
{
    if (R(1))
        wr32(R(1), ic_get_seed());
    RETURN(0);
}

/* ICLaunchURL(inst, hint, data, len, &selStart, &selEnd) */
static void ic_LaunchURL(ppc_cpu *c)
{
    gaddr ps = R(4), pe = R(5);
    int32_t len = (int32_t)R(3);
    RETURN((uint32_t)(int32_t)ic_launch_url(R(2), len, ps ? (int32_t)rd32(ps) : 0, pe ? (int32_t)rd32(pe) : len));
}

/* C-string forms of the comparisons. */
static gaddr temp_pstring(gaddr cstr)
{
    gaddr p = tb_NewPtr(256, false, true);
    if (p)
        c_to_p(cstr, p);
    return p;
}
static void c_compare(ppc_cpu *c, uint16_t trap, bool equal)
{
    gaddr a = temp_pstring(R(0)), b = temp_pstring(R(1));
    uint32_t r = compare_trap(trap, a, b, R(2) & 0xFF, R(3) & 0xFF);
    tb_DisposePtr(a);
    tb_DisposePtr(b);
    RETURN(equal ? (uint32_t)(r == 0) : (uint32_t)(int32_t)(int16_t)r);
}
static void g_equalstring(ppc_cpu *c) { c_compare(c, 0xA03C, true); }
static void g_relstring(ppc_cpu *c) { c_compare(c, 0xA050, false); }

/* The MIDI Manager's version, the same way. */
static void g_MIDIVersion(ppc_cpu *c)
{
    if (R(0))
        wr32(R(0), (uint32_t)tb_DirectorUnknown3());
    c->pc = c->lr;
}
/* No AppleTalk: the port isn't configured. */
static void g_portNotCf(ppc_cpu *c) { RETURN((uint32_t)(int32_t)-98); }
static void g_GetVideoDefault(ppc_cpu *c)
{
    if (R(0))
        startmgr_get_video(R(0));
    c->pc = c->lr;
}

/* ---------------------------------------------------------------------- */
/* Wide (64-bit) arithmetic: hi long then lo long in memory. */

static int64_t wide_get(gaddr w) { return (int64_t)((uint64_t)rd32(w) << 32 | rd32(w + 4)); }
static void wide_put(gaddr w, int64_t v)
{
    wr32(w, (uint32_t)((uint64_t)v >> 32));
    wr32(w + 4, (uint32_t)v);
}
static void w_WideAdd(ppc_cpu *c) { wide_put(R(0), (int64_t)((uint64_t)wide_get(R(0)) + (uint64_t)wide_get(R(1)))); RETURN(R(0)); }
static void w_WideSubtract(ppc_cpu *c) { wide_put(R(0), (int64_t)((uint64_t)wide_get(R(0)) - (uint64_t)wide_get(R(1)))); RETURN(R(0)); }
static void w_WideNegate(ppc_cpu *c) { wide_put(R(0), (int64_t)(0 - (uint64_t)wide_get(R(0)))); RETURN(R(0)); }
static void w_WideCompare(ppc_cpu *c)
{
    int64_t a = wide_get(R(0)), b = wide_get(R(1));
    RETURN((int32_t)(a < b ? -1 : a > b));
}
static void w_WideMultiply(ppc_cpu *c) { wide_put(R(2), (int64_t)(int32_t)R(0) * (int32_t)R(1)); RETURN(R(2)); }
/* WideDivide(dividend, divisor, *remainder): the quotient (a long). */
static void w_WideDivide(ppc_cpu *c)
{
    int64_t a = wide_get(R(0));
    int32_t d = (int32_t)R(1);
    if (!d) {
        if (R(2))
            wr32(R(2), 0x80000000u);
        RETURN(0x80000000u);
        return;
    }
    if (R(2))
        wr32(R(2), (uint32_t)(int32_t)(a % d));
    RETURN((uint32_t)(int32_t)(a / d));
}
/* WideWideDivide(dividend, divisor, *remainder): the quotient in place. */
static void w_WideWideDivide(ppc_cpu *c)
{
    int64_t a = wide_get(R(0));
    int32_t d = (int32_t)R(1);
    if (d) {
        if (R(2))
            wr32(R(2), (uint32_t)(int32_t)(a % d));
        wide_put(R(0), a / d);
    }
    RETURN(R(0));
}
/* WideShift(src, shift): arithmetic, right for a positive shift, rounded. */
static void w_WideShift(ppc_cpu *c)
{
    int64_t a = wide_get(R(0));
    int32_t n = (int32_t)R(1);
    if (n > 0 && n < 64)
        a = (a + ((int64_t)1 << (n - 1))) >> n;
    else if (n < 0 && n > -64)
        a = (int64_t)((uint64_t)a << -n);
    wide_put(R(0), a);
    RETURN(R(0));
}
static void w_WideBitShift(ppc_cpu *c)
{
    int64_t a = wide_get(R(0));
    int32_t n = (int32_t)R(1);
    if (n > 0 && n < 64)
        a >>= n;
    else if (n < 0 && n > -64)
        a = (int64_t)((uint64_t)a << -n);
    wide_put(R(0), a);
    RETURN(R(0));
}
static void w_WideSquareRoot(ppc_cpu *c)
{
    int64_t a = wide_get(R(0));
    RETURN(a > 0 ? (uint32_t)sqrt((double)a) : 0);
}

/* ---------------------------------------------------------------------- */
/* The C-string forms of Pascal-string calls: the strings go over to
 * Pascal, the Pascal routine runs, and strings it fills come back. */

/* Call the export `name` with argument `in` (a C string) made Pascal, and
 * argument `out` (a buffer) made C after; -1 for none. */
static void with_pascal(ppc_cpu *c, const char *name, int in, int out)
{
    void (*fn)(ppc_cpu *) = cfm_export_fn(name);
    if (!fn) {
        ppc_halt(c, "unimplemented: %s (for the C-string form)", name);
        return;
    }
    gaddr tmp = 0, cbuf = out >= 0 ? R(out) : 0;
    if (in >= 0 && R(in)) {
        tmp = temp_pstring(R(in));
        c->gpr[3 + in] = tmp;
    }
    if (cbuf) {
        tmp = tmp ? tmp : tb_NewPtr(256, true, true);
        c->gpr[3 + out] = tmp;
    }
    uint32_t lr = c->lr;
    fn(c);
    if (cbuf)
        p_to_c(tmp, cbuf);
    if (tmp)
        tb_DisposePtr(tmp);
    c->pc = lr;
}
/* The same with several string arguments: bit n of `ins` for argument n. */
static void with_pascals(ppc_cpu *c, const char *name, unsigned ins)
{
    void (*fn)(ppc_cpu *) = cfm_export_fn(name);
    if (!fn) {
        ppc_halt(c, "unimplemented: %s (for the C-string form)", name);
        return;
    }
    gaddr t[8] = { 0 };
    for (int i = 0; i < 8; i++)
        if ((ins >> i) & 1 && R(i)) {
            t[i] = temp_pstring(R(i));
            c->gpr[3 + i] = t[i];
        }
    uint32_t lr = c->lr;
    fn(c);
    for (int i = 0; i < 8; i++)
        if (t[i])
            tb_DisposePtr(t[i]);
    c->pc = lr;
}
static void cs_getfinfo(ppc_cpu *c) { with_pascals(c, "GetFInfo", 1); }
static void cs_setfinfo(ppc_cpu *c) { with_pascals(c, "SetFInfo", 1); }
static void cs_fsopen(ppc_cpu *c) { with_pascals(c, "FSOpen", 1); }
static void cs_openrf(ppc_cpu *c) { with_pascals(c, "OpenRF", 1); }
static void cs_create(ppc_cpu *c) { with_pascals(c, "Create", 1); }
static void cs_fsdelete(ppc_cpu *c) { with_pascals(c, "FSDelete", 1); }
static void cs_hopen(ppc_cpu *c) { with_pascals(c, "HOpen", 4); }
static void cs_fsmakefsspec(ppc_cpu *c) { with_pascals(c, "FSMakeFSSpec", 4); }
static void cs_opendeskacc(ppc_cpu *c) { with_pascals(c, "OpenDeskAcc", 1); }
static void cs_opendriver(ppc_cpu *c) { with_pascals(c, "OpenDriver", 1); }
static void cs_newwindow(ppc_cpu *c) { with_pascals(c, "NewWindow", 4); }
static void cs_newcwindow(ppc_cpu *c) { with_pascals(c, "NewCWindow", 4); }
static void cs_setwtitle(ppc_cpu *c) { with_pascals(c, "SetWTitle", 2); }
static void cs_getwtitle(ppc_cpu *c) { with_pascal(c, "GetWTitle", -1, 1); }
static void cs_newcontrol(ppc_cpu *c) { with_pascals(c, "NewControl", 8); }
/* The alert sound's 'snd ' ID (Sound control panel): kept, not used. */
static int16_t alert_beep_id = 1;
static void g_GetAlertBeepResID(ppc_cpu *c) { RETURN((int32_t)alert_beep_id); }
static void g_SetAlertBeepResID(ppc_cpu *c)
{
    alert_beep_id = (int16_t)R(0);
    c->pc = c->lr;
}

/* YieldToAnyThread(): YieldToThread(kNoThreadID). */
static void g_YieldToAnyThread(ppc_cpu *c)
{
    void (*fn)(ppc_cpu *) = cfm_export_fn("YieldToThread");
    c->gpr[3] = 0;
    if (fn)
        fn(c);
    else
        RETURN(0);
}
static void cs_openresfile(ppc_cpu *c) { with_pascals(c, "OpenResFile", 1); }
static void cs_createresfile(ppc_cpu *c) { with_pascals(c, "CreateResFile", 1); }
static void cs_getnamedresource(ppc_cpu *c) { with_pascals(c, "GetNamedResource", 2); }
/* InitZone(growProc, masterCount, limitPtr, startPtr): _InitZone with its
 * parameter block. */
static void g_InitZone(ppc_cpu *c)
{
    gaddr pb = tb_NewPtr(14, true, true);
    wr32(pb, R(3));
    wr32(pb + 4, R(2));
    wr16(pb + 8, (uint16_t)R(1));
    wr32(pb + 10, R(0));
    ppc_trap_begin();
    ppc_trap_set(8, pb);
    ppc_trap(0xA019);
    ppc_trap_end();
    tb_DisposePtr(pb);
    c->pc = c->lr;
}
static void cs_setdialogitemtext(ppc_cpu *c) { with_pascal(c, "SetDialogItemText", 1, -1); }
static void cs_getdialogitemtext(ppc_cpu *c) { with_pascal(c, "GetDialogItemText", -1, 1); }
static void cs_numtostring(ppc_cpu *c) { with_pascal(c, "NumToString", -1, 1); }
static void cs_stringtonum(ppc_cpu *c) { with_pascal(c, "StringToNum", 0, -1); }
static void cs_setcontroltitle(ppc_cpu *c) { with_pascal(c, "SetControlTitle", 1, -1); }
static void cs_getcontroltitle(ppc_cpu *c) { with_pascal(c, "GetControlTitle", -1, 1); }
static void cs_insertmenuitem(ppc_cpu *c) { with_pascal(c, "InsertMenuItem", 1, -1); }
static void cs_appendmenu(ppc_cpu *c) { with_pascal(c, "AppendMenu", 1, -1); }
static void cs_setmenuitemtext(ppc_cpu *c) { with_pascal(c, "SetMenuItemText", 2, -1); }
static void cs_getmenuitemtext(ppc_cpu *c) { with_pascal(c, "GetMenuItemText", -1, 2); }
static void cs_drawstring(ppc_cpu *c) { with_pascal(c, "DrawString", 0, -1); }
static void cs_stringwidth(ppc_cpu *c) { with_pascal(c, "StringWidth", 0, -1); }
static void cs_upperstring(ppc_cpu *c) { with_pascal(c, "UpperString", 0, 0); }
static void cs_debugstr(ppc_cpu *c) { c->pc = c->lr; }
static void cs_newmenu(ppc_cpu *c) { with_pascal(c, "NewMenu", 1, -1); }
static void cs_getindstring(ppc_cpu *c) { with_pascal(c, "GetIndString", -1, 0); }
/* paramtext(a, b, c, d): four strings in. */
static void cs_paramtext(ppc_cpu *c)
{
    gaddr t[4];
    for (int i = 0; i < 4; i++) {
        t[i] = R(i) ? temp_pstring(R(i)) : 0;
        c->gpr[3 + i] = t[i];
    }
    uint32_t lr = c->lr;
    void (*fn)(ppc_cpu *) = cfm_export_fn("ParamText");
    if (fn)
        fn(c);
    for (int i = 0; i < 4; i++)
        if (t[i])
            tb_DisposePtr(t[i]);
    c->pc = lr;
}

/* ---------------------------------------------------------------------- */
/* Assorted */

/* TextEdit's click loop is a field. */
static void g_TESetClickLoop(ppc_cpu *c)
{
    if (R(1) && rd32(R(1)))
        wr32(rd32(R(1)) + TERec_wordBreak_off + 4, R(0));
    c->pc = c->lr;
}
static void g_InlineGetHandleSize(ppc_cpu *c) { RETURN(R(0) ? (uint32_t)tb_GetHandleSize(R(0)) : 0); }
/* GetHandleSize and GetPtrSize: 0 when the handle or pointer is bad (nil
 * or purged), MemError saying why.  Only the 68k trap leaves the result
 * code in D0; Internet Explorer 5 sizes a copy of a nil handle by it. */
static void g_GetHandleSize(ppc_cpu *c)
{
    int32_t n = tb_GetHandleSize(R(0));
    RETURN((uint32_t)(n < 0 ? 0 : n));
}
static void g_GetPtrSize(ppc_cpu *c)
{
    int32_t n = tb_GetPtrSize(R(0));
    RETURN((uint32_t)(n < 0 ? 0 : n));
}
static void g_GZSaveHnd(ppc_cpu *c) { RETURN(rd32(LM_GZRootHnd)); }
static void g_LMGetSysFontSize(ppc_cpu *c) { RETURN((int32_t)(int16_t)rd16(0x0BA8)); }
static void g_LMSetSysFontSize(ppc_cpu *c) { wr16(0x0BA8, (uint16_t)R(0)); c->pc = c->lr; }
/* HWCfgFlags: the low-memory word the definitions call SCSIFlags. */
static void g_LMGetHWCfgFlags(ppc_cpu *c) { RETURN((int32_t)(int16_t)rd16(LM_SCSIFlags)); }
static void g_LMSetHWCfgFlags(ppc_cpu *c) { wr16(LM_SCSIFlags, (uint16_t)R(0)); c->pc = c->lr; }
static void g_LMGetGNEFilter(ppc_cpu *c) { RETURN(rd32(0x029A)); }
static void g_LMSetGNEFilter(ppc_cpu *c) { wr32(0x029A, R(0)); c->pc = c->lr; }
static void g_IUEqualPString(ppc_cpu *c) { g_IUEqualString(c); }
static void g_IUCompPString(ppc_cpu *c) { g_IUCompString(c); }
static void g_GetComponentVersion(ppc_cpu *c) { instance_call(c, -4, ""); }
static void g_false(ppc_cpu *c) { RETURN(0); }
/* The serial driver's calls, on a port that is always idle. */
/* The Serial Manager: Control and Status calls on the driver, csParam
 * filled before and read after. */
static gaddr ser_pb(ppc_cpu *c, int16_t code)
{
    gaddr pb = tb_NewPtr(CntrlParam_SIZEOF, false, true);
    wr16(pb + CntrlParam_ioCRefNum_off, (uint16_t)R(0));
    wr16(pb + CntrlParam_csCode_off, (uint16_t)code);
    return pb;
}
static void ser_call(ppc_cpu *c, gaddr pb, bool status, gaddr out, uint32_t n)
{
    int16_t r = status ? tb_PBStatus(pb, false) : tb_PBControl(pb, false);
    if (out && !r)
        gmemcpy(out, pb + CntrlParam_csParam_off, n);
    tb_DisposePtr(pb);
    RETURN((int32_t)r);
}
static void g_SerReset(ppc_cpu *c)
{
    gaddr pb = ser_pb(c, 8);
    wr16(pb + CntrlParam_csParam_off, (uint16_t)R(1));
    ser_call(c, pb, false, 0, 0);
}
static void g_SerSetBuf(ppc_cpu *c)
{
    gaddr pb = ser_pb(c, 9);
    wr32(pb + CntrlParam_csParam_off, R(1));
    wr16(pb + CntrlParam_csParam_off + 4, (uint16_t)R(2));
    ser_call(c, pb, false, 0, 0);
}
static void g_SerHShake(ppc_cpu *c)
{
    gaddr pb = ser_pb(c, 10);
    if (R(1))
        gmemcpy(pb + CntrlParam_csParam_off, R(1), 8);
    ser_call(c, pb, false, 0, 0);
}
static void g_SerSetBrk(ppc_cpu *c) { ser_call(c, ser_pb(c, 12), false, 0, 0); }
static void g_SerClrBrk(ppc_cpu *c) { ser_call(c, ser_pb(c, 11), false, 0, 0); }
static void g_SerGetBuf(ppc_cpu *c) { ser_call(c, ser_pb(c, 2), true, R(1), 4); }
static void g_SerStatus(ppc_cpu *c) { ser_call(c, ser_pb(c, 8), true, R(1), 6); }
/* The AppleTalk calls: AppleTalk isn't there. */
static void g_noAppleTalk(ppc_cpu *c) { RETURN((uint32_t)(int32_t)-98); }

/* AppleTalk's preferred calls: (pb, async) as a Control call on .MPP or
 * .ATP.  Without the trap layer, an answer given at once is written to
 * ioResult and an asynchronous call's completion routine queued here. */
static void at_call(ppc_cpu *c, int16_t refnum, int16_t code)
{
    gaddr pb = R(0);
    bool async = R(1) != 0;
    wr16(pb + IOParam_ioRefNum_off, (uint16_t)refnum);
    wr16(pb + CntrlParam_csCode_off, (uint16_t)code);
    int16_t r = tb_PBControl(pb, async);
    if (r == ioInProgress) {
        RETURN(0);
        return;
    }
    wr16(pb + IOParam_ioResult_off, (uint16_t)r);
    if (async && rd32(pb + IOParam_ioCompletion_off))
        io_complete_later(pb, r);
    RETURN((int32_t)r);
}
#define MPP_CALL(name, code) static void g_##name(ppc_cpu *c) { at_call(c, -10, code); }
#define ATP_CALL(name, code) static void g_##name(ppc_cpu *c) { at_call(c, -11, code); }
MPP_CALL(POpenSkt, 248) MPP_CALL(PCloseSkt, 247) MPP_CALL(PWriteDDP, 246) MPP_CALL(PRegisterName, 253)
MPP_CALL(PLookupName, 251) MPP_CALL(PConfirmName, 250) MPP_CALL(PRemoveName, 252) MPP_CALL(PKillNBP, 254)
MPP_CALL(PSetSelfSend, 256) MPP_CALL(PGetAppleTalkInfo, 258) MPP_CALL(PATalkClosePrep, 259)
ATP_CALL(POpenATPSkt, 254) ATP_CALL(PCloseATPSkt, 250) ATP_CALL(PSendRequest, 255) ATP_CALL(PNSendRequest, 248)
ATP_CALL(PGetRequest, 253) ATP_CALL(PSendResponse, 252) ATP_CALL(PAddResponse, 251) ATP_CALL(PRelTCB, 256)
ATP_CALL(PRelRspCB, 249) ATP_CALL(PKillSendReq, 258) ATP_CALL(PKillGetReq, 257)

/* A Str32 packed after another (NBP's entity names). */
static uint32_t put_str32(gaddr dst, gaddr s)
{
    uint32_t n = s ? rd8(s) : 0;
    if (n > 32)
        n = 32;
    wr8(dst, (uint8_t)n);
    if (n)
        gmemcpy(dst + 1, s + 1, n);
    return n + 1;
}

/* NBPSetEntity(buffer, object, type, zone) */
static void g_NBPSetEntity(ppc_cpu *c)
{
    gaddr p = R(0);
    p += put_str32(p, R(1));
    p += put_str32(p, R(2));
    put_str32(p, R(3));
    RETURN(0);
}

/* NBPSetNTE(ntePtr, object, type, zone, socket): a names table entry,
 * the entity after its link, address and enumerator. */
static void g_NBPSetNTE(ppc_cpu *c)
{
    gaddr nte = R(0);
    gmemset(nte, 0, 9);
    wr8(nte + 7, (uint8_t)R(4));
    gaddr p = nte + 9;
    p += put_str32(p, R(1));
    p += put_str32(p, R(2));
    put_str32(p, R(3));
    RETURN(0);
}

/* NBPExtract(buffer, numInBuf, whichOne, EntityName *, AddrBlock *): the
 * whichOne'th tuple of a lookup's answers. */
static void g_NBPExtract(ppc_cpu *c)
{
    gaddr p = R(0);
    int count = (int16_t)R(1), which = (int16_t)R(2);
    if (which < 1 || which > count) {
        RETURN((int32_t)-3104); /* extractErr */
        return;
    }
    for (int i = 1; i < which; i++) {
        p += 5;
        for (int k = 0; k < 3; k++)
            p += 1 + (uint32_t)rd8(p);
    }
    if (R(4))
        wr32(R(4), rd32(p));
    p += 5;
    for (int k = 0; k < 3; k++) { /* EntityName: three Str32 fields of 34 bytes */
        if (R(3))
            put_str32(R(3) + (uint32_t)k * 34, p);
        p += 1 + (uint32_t)rd8(p);
    }
    RETURN(0);
}

/* BuildDDPwds(wds, header, data, AddrBlock dest, type, length): the
 * header's pointer (an odd address: the buffer's second byte) with the
 * destination and type in it, then the data, then the end. */
static void g_BuildDDPwds(ppc_cpu *c)
{
    gaddr wds = R(0), hdr = R(1) + 1, dest = R(3);
    gmemset(hdr, 0, 16);
    wr16(hdr + 7, (uint16_t)(dest >> 16));
    wr8(hdr + 11, (uint8_t)(dest >> 8));
    wr8(hdr + 13, (uint8_t)dest);
    wr8(hdr + 15, (uint8_t)R(4));
    wr16(wds, 0);
    wr32(wds + 2, hdr);
    wr16(wds + 6, (uint16_t)R(5));
    wr32(wds + 8, R(2));
    wr16(wds + 12, 0);
    RETURN(0);
}

/* BuildBDSBuffer(buffer, bds, size, count): the buffer cut into count
 * elements of up to 578 bytes; returns how many. */
static void g_BuildBDSBuffer(ppc_cpu *c)
{
    gaddr buf = R(0), bds = R(1);
    int left = (int16_t)R(2), n = (int16_t)R(3), i = 0;
    for (; i < n && i < 8 && left > 0; i++) {
        int k = left > 578 ? 578 : left;
        gaddr e = bds + (uint32_t)i * 12;
        wr16(e, (uint16_t)k);
        wr32(e + 2, buf);
        wr16(e + 6, 0);
        wr32(e + 8, 0);
        buf += (uint32_t)k;
        left -= k;
    }
    RETURN(i);
}

/* GetNodeAddress(short *node, short *net) */
static void g_GetNodeAddress(ppc_cpu *c)
{
    if (!at_opened()) {
        RETURN((int32_t)-28); /* notOpenErr */
        return;
    }
    if (R(0))
        wr16(R(0), at_node());
    if (R(1))
        wr16(R(1), 0);
    RETURN(0);
}
static void g_IsMPPOpen(ppc_cpu *c) { RETURN(at_opened() ? 1 : 0); }
static void g_MPPOpen(ppc_cpu *c) { RETURN((int32_t)at_open()); }

/* ---------------------------------------------------------------------- */
/* More InterfaceLib */

/* The Slot Manager's calls on an SpBlock (slots.c). */
#define SLOT_CALL(name, sel) static void g_##name(ppc_cpu *c) { RETURN((int32_t)slot_manager(sel, R(0))); }
SLOT_CALL(SReadByte, 0x00) SLOT_CALL(SReadWord, 0x01) SLOT_CALL(SReadLong, 0x02) SLOT_CALL(SGetCString, 0x03)
SLOT_CALL(SGetBlock, 0x05) SLOT_CALL(SFindStruct, 0x06) SLOT_CALL(SReadStruct, 0x07) SLOT_CALL(SGetSRsrc, 0x0B)
SLOT_CALL(SGetTypeSRsrc, 0x0C) SLOT_CALL(SNextSRsrc, 0x14) SLOT_CALL(SNextTypeSRsrc, 0x15) SLOT_CALL(SRsrcInfo, 0x16)
SLOT_CALL(SDisposePtr, 0x17) SLOT_CALL(SReadDrvrName, 0x19) SLOT_CALL(SVersion, 0x08)
#undef SLOT_CALL

/* The Cursor Device Manager (osutil.c): the arguments as words, a word
 * argument (2) sign-extended. */
static void crsr_dev(ppc_cpu *c, uint16_t sel, const char *args)
{
    uint32_t v[4] = { 0 };
    for (int i = 0; args[i] && i < 4; i++)
        v[i] = args[i] == '2' ? (uint32_t)(int32_t)(int16_t)R(i) : R(i);
    RETURN((int32_t)cursor_device(sel, v));
}
#define CRSR_CALL(name, sel, args) static void g_##name(ppc_cpu *c) { crsr_dev(c, sel, args); }
CRSR_CALL(CrsrDevMove, 0x0, "444") CRSR_CALL(CrsrDevMoveTo, 0x1, "444") CRSR_CALL(CrsrDevFlush, 0x2, "4")
CRSR_CALL(CrsrDevButtons, 0x3, "42") CRSR_CALL(CrsrDevButtonDown, 0x4, "4") CRSR_CALL(CrsrDevButtonUp, 0x5, "4")
CRSR_CALL(CrsrDevButtonOp, 0x6, "4224") CRSR_CALL(CrsrDevSetButtons, 0x7, "42")
CRSR_CALL(CrsrDevSetAcceleration, 0x8, "44") CRSR_CALL(CrsrDevDoubleTime, 0x9, "44")
CRSR_CALL(CrsrDevUnitsPerInch, 0xA, "44") CRSR_CALL(CrsrDevNextDevice, 0xB, "4")
CRSR_CALL(CrsrDevNewDevice, 0xC, "4") CRSR_CALL(CrsrDevDisposeDevice, 0xD, "4")
#undef CRSR_CALL

/* A universal procedure pointer from PowerPC code: a routine descriptor,
 * or (from code that skipped NewRoutineDescriptor) a transition vector. */
static uint32_t call_upp(gaddr upp, uint32_t procinfo, const uint32_t *args, int n)
{
    if (!upp)
        return 0;
    return mm_is_descriptor(upp) ? mm_call(upp, procinfo, args, n) : ppc_call(upp, args, n);
}

/* GetFormatOrder: see formatorder.c. */
void ppc_GetFormatOrder(ppc_cpu *c);

/* ValidDate(const LongDateRec *, flags, LongDateTime *newSecs): the first
 * field (of those flags names, by mask bit) out of range, or -1
 * (validDateFields) and the date's seconds.  smallDateBit (31) keeps the
 * date within what the 32-bit Time global holds, 1904 to 2040. */
static void g_ValidDate(ppc_cpu *c)
{
    gaddr d = R(0);
    uint32_t flags = R(1);
    int16_t f[11];
    for (int i = 0; i < 11; i++)
        f[i] = (int16_t)rd16(d + 2 * (uint32_t)i);
    int y = f[1], m = f[2];
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int dim = m >= 1 && m <= 12 ? mdays[m - 1] + (m == 2 && leap) : 31;
    const int lo[11] = { 0, (flags >> 31) ? 1904 : -32767, 1, 1, 0, 0, 0, 1, 1, 1, 0 };
    const int hi[11] = { 0, (flags >> 31) ? 2040 : 32767, 12, dim, 23, 59, 59, 7, leap ? 366 : 365, 53, 1 };
    for (int i = 0; i < 11; i++)
        if ((flags >> i) & 1 && (f[i] < lo[i] || f[i] > hi[i])) {
            RETURN(i);
            return;
        }
    if (R(2))
        tb_LongDateToSeconds(d, R(2));
    RETURN((uint32_t)-1);
}

/* IdleUpdate(): the Power Manager's idle timer starts again; Ticks. */
static void g_IdleUpdate(ppc_cpu *c) { RETURN(rd32(LM_Ticks)); }

/* FollowFinderAlias(fromFile, alias, logon, *target, *wasChanged) */
static void g_FollowFinderAlias(ppc_cpu *c) { RETURN((int32_t)tb_ResolveAlias(R(0), R(1), R(3), R(4))); }

/* DrvrInstall(drvrPtr, refNum) (drivers.c) */
static void g_DrvrInstall(ppc_cpu *c) { RETURN((int32_t)driver_install(R(0), (int16_t)R(1))); }

/* BuildBDS(buffPtr, bdsPtr, buffSize): BuildBDSBuffer's eight elements. */
static void g_BuildBDS(ppc_cpu *c)
{
    c->gpr[6] = 8;
    g_BuildBDSBuffer(c);
}

/* WriteLocation(MachineLocation *): kept for ReadLocation. */
static void g_WriteLocation(ppc_cpu *c)
{
    if (R(0))
        gget(location, R(0), sizeof location);
    c->pc = c->lr;
}

/* IPCListPortsAsync(pb): IPCListPortsSync's answer, in ioResult and to
 * the completion routine. */
static void g_IPCListPortsAsync(ppc_cpu *c)
{
    gaddr pb = R(0);
    int16_t r = -98;
    if (pb) {
        wr16(pb + IOParam_ioResult_off, (uint16_t)r);
        if (rd32(pb + IOParam_ioCompletion_off))
            io_complete_later(pb, r);
    }
    RETURN((int32_t)r);
}

/* SleepQInstall / SleepQRemove(SleepQRecPtr): the machine never sleeps,
 * so the sleep queue's procedures are never called. */

/* Anti-aliased text (Appearance's font smoothing): the setting is kept;
 * QuickDraw here draws the fonts' bitmaps either way. */
static bool aa_enabled;
static int16_t aa_min_size = 12;
static void g_SetAntiAliasedTextEnabled(ppc_cpu *c)
{
    aa_enabled = (R(0) & 0xFF) != 0;
    aa_min_size = (int16_t)R(1);
    RETURN(0);
}
static void g_IsAntiAliasedTextEnabled(ppc_cpu *c)
{
    if (R(0))
        wr16(R(0), (uint16_t)aa_min_size);
    RETURN(aa_enabled);
}

/* IsCmdChar(event, test): a key-down or auto-key with the Command key,
 * whose key makes test with Shift up or down. */
static void g_IsCmdChar(ppc_cpu *c)
{
    gaddr ev = R(0);
    uint8_t want = (uint8_t)R(1);
    int16_t what = ev ? (int16_t)rd16(ev) : 0;
    uint16_t mods = ev ? rd16(ev + 14) : 0;
    if ((what != 3 && what != 5) || !(mods & 0x0100)) {
        RETURN(0);
        return;
    }
    uint32_t msg = rd32(ev + 2);
    if ((uint8_t)msg == want) {
        RETURN(1);
        return;
    }
    uint16_t key = (uint16_t)((msg >> 8) & 0x7F), hi = (uint16_t)((mods >> 8) & ~0x03); /* without Command and Shift */
    for (int shift = 0; shift < 2; shift++)
        if ((uint8_t)tb_KeyTranslate(0, (uint16_t)(key | (hi | (shift ? 2 : 0)) << 8), 0) == want) {
            RETURN(1);
            return;
        }
    RETURN(0);
}

/* getfnum(name, *num) and getfontname(num, name): C strings. */
static void cs_getfnum(ppc_cpu *c) { with_pascals(c, "GetFNum", 1); }
static void cs_getfontname(ppc_cpu *c) { with_pascal(c, "GetFontName", -1, 1); }

/* AppleScriptLib: the Open Scripting Architecture's calls go to the
 * scripting component instance they're given (our AppleScript, osa.c). */
static void os_OSALoad(ppc_cpu *c) { instance_call(c, 0x0001, "444"); }
static void os_OSAStore(ppc_cpu *c) { instance_call(c, 0x0002, "4444"); }
static void os_OSAExecute(ppc_cpu *c) { instance_call(c, 0x0003, "4444"); }
static void os_OSADisplay(ppc_cpu *c) { instance_call(c, 0x0004, "4444"); }
static void os_OSAScriptError(ppc_cpu *c) { instance_call(c, 0x0005, "444"); }
static void os_OSADispose(ppc_cpu *c) { instance_call(c, 0x0006, "4"); }
static void os_OSASetScriptInfo(ppc_cpu *c) { instance_call(c, 0x0007, "444"); }
static void os_OSAGetScriptInfo(ppc_cpu *c) { instance_call(c, 0x0008, "444"); }
static void os_OSASetActiveProc(ppc_cpu *c) { instance_call(c, 0x0009, "44"); }
static void os_OSAGetActiveProc(ppc_cpu *c) { instance_call(c, 0x000A, "44"); }
static void os_OSAScriptingComponentName(ppc_cpu *c) { instance_call(c, 0x0102, "4"); }
static void os_OSAGetSource(ppc_cpu *c) { instance_call(c, 0x0201, "444"); }
static void os_OSACompile(ppc_cpu *c) { instance_call(c, 0x0203, "444"); }
static void os_OSACopyID(ppc_cpu *c) { instance_call(c, 0x0204, "44"); }
static void os_OSACoerceFromDesc(ppc_cpu *c) { instance_call(c, 0x0301, "444"); }
static void os_OSACoerceToDesc(ppc_cpu *c) { instance_call(c, 0x0302, "4444"); }
static void os_OSASetSendProc(ppc_cpu *c) { instance_call(c, 0x0305, "44"); }
static void os_OSAGetSendProc(ppc_cpu *c) { instance_call(c, 0x0306, "44"); }
static void os_OSASetCreateProc(ppc_cpu *c) { instance_call(c, 0x0307, "44"); }
static void os_OSAGetCreateProc(ppc_cpu *c) { instance_call(c, 0x0308, "44"); }
static void os_OSASetDefaultTarget(ppc_cpu *c) { instance_call(c, 0x0309, "4"); }
static void os_OSALoadExecute(ppc_cpu *c) { instance_call(c, 0x0501, "4444"); }
static void os_OSACompileExecute(ppc_cpu *c) { instance_call(c, 0x0502, "4444"); }
static void os_OSADoScript(ppc_cpu *c) { instance_call(c, 0x0503, "44444"); }
static void os_OSASetResumeDispatchProc(ppc_cpu *c) { instance_call(c, 0x0507, "44"); }
static void os_OSAGetResumeDispatchProc(ppc_cpu *c) { instance_call(c, 0x0508, "44"); }
static void os_OSAExecuteEvent(ppc_cpu *c) { instance_call(c, 0x0509, "4444"); }
static void os_OSADoEvent(ppc_cpu *c) { instance_call(c, 0x050A, "4444"); }
static void os_OSAMakeContext(ppc_cpu *c) { instance_call(c, 0x050B, "444"); }

/* A new run: the guest blocks went with the old heap. */
void ppc_libs_reset(void)
{
    memset(location, 0, sizeof location);
    stdclib_reset();
    aa_enabled = false;
    aa_min_size = 12;
}

const ppc_export lib_exports[] = {
    /* Slot Manager */
    { "SReadByte", g_SReadByte }, { "SReadWord", g_SReadWord }, { "SReadLong", g_SReadLong },
    { "SGetCString", g_SGetCString }, { "SGetBlock", g_SGetBlock }, { "SFindStruct", g_SFindStruct },
    { "SReadStruct", g_SReadStruct }, { "SGetSRsrc", g_SGetSRsrc }, { "SGetTypeSRsrc", g_SGetTypeSRsrc },
    { "SNextSRsrc", g_SNextSRsrc }, { "SNextTypeSRsrc", g_SNextTypeSRsrc }, { "SRsrcInfo", g_SRsrcInfo },
    { "SDisposePtr", g_SDisposePtr }, { "SReadDrvrName", g_SReadDrvrName },
    /* Cursor Device Manager, by both names */
    { "CrsrDevMove", g_CrsrDevMove }, { "CrsrDevMoveTo", g_CrsrDevMoveTo }, { "CrsrDevFlush", g_CrsrDevFlush },
    { "CrsrDevButtons", g_CrsrDevButtons }, { "CrsrDevButtonDown", g_CrsrDevButtonDown },
    { "CrsrDevButtonUp", g_CrsrDevButtonUp }, { "CrsrDevButtonOp", g_CrsrDevButtonOp },
    { "CrsrDevSetButtons", g_CrsrDevSetButtons }, { "CrsrDevSetAcceleration", g_CrsrDevSetAcceleration },
    { "CrsrDevDoubleTime", g_CrsrDevDoubleTime }, { "CrsrDevUnitsPerInch", g_CrsrDevUnitsPerInch },
    { "CrsrDevNextDevice", g_CrsrDevNextDevice }, { "CrsrDevNewDevice", g_CrsrDevNewDevice },
    { "CrsrDevDisposeDevice", g_CrsrDevDisposeDevice },
    { "CursorDeviceMove", g_CrsrDevMove }, { "CursorDeviceMoveTo", g_CrsrDevMoveTo },
    { "CursorDeviceFlush", g_CrsrDevFlush }, { "CursorDeviceButtons", g_CrsrDevButtons },
    { "CursorDeviceButtonDown", g_CrsrDevButtonDown }, { "CursorDeviceButtonUp", g_CrsrDevButtonUp },
    { "CursorDeviceButtonOp", g_CrsrDevButtonOp }, { "CursorDeviceSetButtons", g_CrsrDevSetButtons },
    { "CursorDeviceSetAcceleration", g_CrsrDevSetAcceleration }, { "CursorDeviceDoubleTime", g_CrsrDevDoubleTime },
    { "CursorDeviceUnitsPerInch", g_CrsrDevUnitsPerInch }, { "CursorDeviceNextDevice", g_CrsrDevNextDevice },
    { "CursorDeviceNewDevice", g_CrsrDevNewDevice }, { "CursorDeviceDisposeDevice", g_CrsrDevDisposeDevice },
    /* Assorted InterfaceLib */
    { "GetFormatOrder", ppc_GetFormatOrder },
    { "ValidDate", g_ValidDate }, { "IdleUpdate", g_IdleUpdate }, { "FollowFinderAlias", g_FollowFinderAlias },
    { "DrvrInstall", g_DrvrInstall }, { "BuildBDS", g_BuildBDS }, { "WriteLocation", g_WriteLocation },
    { "LwrText", g_LowerText }, { "IPCListPortsAsync", g_IPCListPortsAsync },
    { "DebuggerLockMemory", g_noErr }, { "DebuggerUnlockMemory", g_noErr },
    { "SleepQInstall", g_nothing }, { "SleepQRemove", g_nothing },
    { "SetAntiAliasedTextEnabled", g_SetAntiAliasedTextEnabled },
    { "IsAntiAliasedTextEnabled", g_IsAntiAliasedTextEnabled }, { "IsCmdChar", g_IsCmdChar },
    { "getfnum", cs_getfnum }, { "getfontname", cs_getfontname },
    { "LMGetHWCfgFlags", g_LMGetHWCfgFlags }, { "LMSetHWCfgFlags", g_LMSetHWCfgFlags },
    /* AppleScriptLib */
    { "OSALoad", os_OSALoad }, { "OSAStore", os_OSAStore }, { "OSAExecute", os_OSAExecute },
    { "OSADisplay", os_OSADisplay }, { "OSAScriptError", os_OSAScriptError }, { "OSADispose", os_OSADispose },
    { "OSASetScriptInfo", os_OSASetScriptInfo }, { "OSAGetScriptInfo", os_OSAGetScriptInfo },
    { "OSASetActiveProc", os_OSASetActiveProc }, { "OSAGetActiveProc", os_OSAGetActiveProc },
    { "OSAScriptingComponentName", os_OSAScriptingComponentName }, { "OSAGetSource", os_OSAGetSource },
    { "OSACompile", os_OSACompile }, { "OSACopyID", os_OSACopyID }, { "OSACoerceFromDesc", os_OSACoerceFromDesc },
    { "OSACoerceToDesc", os_OSACoerceToDesc }, { "OSASetSendProc", os_OSASetSendProc },
    { "OSAGetSendProc", os_OSAGetSendProc }, { "OSASetCreateProc", os_OSASetCreateProc },
    { "OSAGetCreateProc", os_OSAGetCreateProc }, { "OSASetDefaultTarget", os_OSASetDefaultTarget },
    { "OSALoadExecute", os_OSALoadExecute }, { "OSACompileExecute", os_OSACompileExecute },
    { "OSADoScript", os_OSADoScript }, { "OSASetResumeDispatchProc", os_OSASetResumeDispatchProc },
    { "OSAGetResumeDispatchProc", os_OSAGetResumeDispatchProc }, { "OSAExecuteEvent", os_OSAExecuteEvent },
    { "OSADoEvent", os_OSADoEvent }, { "OSAMakeContext", os_OSAMakeContext },
    /* Wide arithmetic */
    { "WideAdd", w_WideAdd }, { "WideBitShift", w_WideBitShift }, { "WideCompare", w_WideCompare },
    { "WideDivide", w_WideDivide }, { "WideMultiply", w_WideMultiply }, { "WideNegate", w_WideNegate },
    { "WideShift", w_WideShift }, { "WideSquareRoot", w_WideSquareRoot }, { "WideSubtract", w_WideSubtract },
    { "WideWideDivide", w_WideWideDivide },
    /* C-string forms */
    { "getfinfo", cs_getfinfo }, { "setfinfo", cs_setfinfo }, { "fsopen", cs_fsopen }, { "openrf", cs_openrf },
    { "create", cs_create }, { "fsdelete", cs_fsdelete }, { "hopen", cs_hopen },
    { "fsmakefsspec", cs_fsmakefsspec }, { "opendeskacc", cs_opendeskacc }, { "opendriver", cs_opendriver },
    { "newwindow", cs_newwindow }, { "newcwindow", cs_newcwindow }, { "setwtitle", cs_setwtitle },
    { "getwtitle", cs_getwtitle }, { "newcontrol", cs_newcontrol },
    { "GetAlertBeepResID", g_GetAlertBeepResID }, { "SetAlertBeepResID", g_SetAlertBeepResID },
    { "YieldToAnyThread", g_YieldToAnyThread },
   
    { "openresfile", cs_openresfile }, { "createresfile", cs_createresfile },
    { "getnamedresource", cs_getnamedresource }, { "InitZone", g_InitZone },
    { "appendmenu", cs_appendmenu }, { "debugstr", cs_debugstr }, { "drawstring", cs_drawstring },
    { "getcontroltitle", cs_getcontroltitle }, { "getdialogitemtext", cs_getdialogitemtext },
    { "getindstring", cs_getindstring }, { "getmenuitemtext", cs_getmenuitemtext },
    { "insertmenuitem", cs_insertmenuitem }, { "newmenu", cs_newmenu }, { "numtostring", cs_numtostring },
    { "paramtext", cs_paramtext }, { "setcontroltitle", cs_setcontroltitle },
    { "setdialogitemtext", cs_setdialogitemtext }, { "setmenuitemtext", cs_setmenuitemtext },
    { "stringtonum", cs_stringtonum }, { "stringwidth", cs_stringwidth }, { "upperstring", cs_upperstring },
    /* Assorted */
    { "TESetClickLoop", g_TESetClickLoop }, { "InlineGetHandleSize", g_InlineGetHandleSize },
    { "GetHandleSize", g_GetHandleSize }, { "GetPtrSize", g_GetPtrSize },
    { "GZSaveHnd", g_GZSaveHnd }, { "LMGetSysFontSize", g_LMGetSysFontSize }, { "LMSetSysFontSize", g_LMSetSysFontSize },
    { "LMGetGNEFilter", g_LMGetGNEFilter }, { "LMSetGNEFilter", g_LMSetGNEFilter },
    { "IUEqualPString", g_IUEqualPString }, { "IUCompPString", g_IUCompPString },
    { "GetComponentVersion", g_GetComponentVersion },
    { "HoldMemory", g_noErr }, { "UnholdMemory", g_noErr }, { "LockMemory", g_noErr }, { "UnlockMemory", g_noErr },
    { "LockMemoryContiguous", g_noErr },
    /* Serial ports */
    { "SerReset", g_SerReset }, { "SerHShake", g_SerHShake }, { "SerSetBuf", g_SerSetBuf }, { "SerSetBrk", g_SerSetBrk },
    { "SerClrBrk", g_SerClrBrk }, { "SerGetBuf", g_SerGetBuf }, { "SerStatus", g_SerStatus },
    /* AppleTalk and the PPC Toolbox's network side */
    { "GetMyZone", g_noAppleTalk },
    { "NBPSetNTE", g_NBPSetNTE }, { "NBPSetEntity", g_NBPSetEntity }, { "NBPExtract", g_NBPExtract },
    { "BuildDDPwds", g_BuildDDPwds }, { "BuildBDSBuffer", g_BuildBDSBuffer }, { "GetNodeAddress", g_GetNodeAddress },
    { "IsMPPOpen", g_IsMPPOpen }, { "IsATPOpen", g_IsMPPOpen }, { "MPPOpen", g_MPPOpen },
    { "POpenSkt", g_POpenSkt }, { "PCloseSkt", g_PCloseSkt }, { "PWriteDDP", g_PWriteDDP },
    { "PRegisterName", g_PRegisterName }, { "PLookupName", g_PLookupName }, { "PConfirmName", g_PConfirmName },
    { "PRemoveName", g_PRemoveName }, { "PKillNBP", g_PKillNBP }, { "PSetSelfSend", g_PSetSelfSend },
    { "PGetAppleTalkInfo", g_PGetAppleTalkInfo }, { "PATalkClosePrep", g_PATalkClosePrep },
    { "POpenATPSkt", g_POpenATPSkt }, { "PCloseATPSkt", g_PCloseATPSkt }, { "PSendRequest", g_PSendRequest },
    { "PNSendRequest", g_PNSendRequest }, { "PGetRequest", g_PGetRequest }, { "PSendResponse", g_PSendResponse },
    { "PAddResponse", g_PAddResponse }, { "PRelTCB", g_PRelTCB }, { "PRelRspCB", g_PRelRspCB },
    { "PKillSendReq", g_PKillSendReq }, { "PKillGetReq", g_PKillGetReq },
    /* Odds and ends */
    { "equalstring", g_equalstring }, { "relstring", g_relstring }, { "SVersion", g_SVersion },
    { "MIDIVersion", g_MIDIVersion }, { "InstallExceptionHandler", g_noErr },
    { "GetVideoDefault", g_GetVideoDefault }, { "MPPOpen", g_portNotCf }, { "PPCInit", g_noErr },
    { "OpenXPP", g_portNotCf }, { "ATPLoad", g_portNotCf }, { "MPPClose", g_noErr },
    { "GetDefaultUser", g_GetDefaultUser }, { "TruncText", g_TruncText },
    { "GetScript", g_GetScript }, { "SetScript", g_SetScript }, { "GetEnvirons", g_GetEnvirons },
    { "SetEnvirons", g_SetEnvirons },
    { "FindWordBreaks", g_FindWordBreaks }, { "UppercaseStripDiacritics", g_UppercaseStripDiacritics },
    { "PBControlImmed", g_PBControlImmed },
    /* Internet Config */
    { "ICStart", ic_ICStart }, { "ICStop", g_noErr }, { "ICFindConfigFile", ic_FindConfigFile },
    { "ICFindUserConfigFile", ic_FindConfigFile }, { "ICGeneralFindConfigFile", ic_FindConfigFile }, { "ICChooseConfig", g_noErr },
    { "ICBegin", g_noErr }, { "ICEnd", g_noErr }, { "ICGetPref", ic_GetPref },
    { "ICFindPrefHandle", ic_FindPrefHandle }, { "ICGetPrefHandle", ic_GetPrefHandle },
    { "ICSetPref", ic_SetPref }, { "ICSetPrefHandle", ic_SetPrefHandle }, { "ICCountPref", ic_CountPref },
    { "ICGetIndPref", ic_GetIndPref }, { "ICDeletePref", ic_DeletePref },
    { "ICLaunchURL", ic_LaunchURL }, { "ICMapFilename", ic_prefNotFound }, { "ICMapTypeCreator", ic_prefNotFound },
    { "ICGetSeed", ic_GetSeed }, { "ICGetVersion", g_noErr }, { "ICGetConfigName", g_noErr },
    /* Movie controllers */
    { "MCActivate", cc_MCActivate }, { "MCAdjustCursor", cc_MCAdjustCursor }, { "MCClear", cc_MCClear },
    { "MCClick", cc_MCClick }, { "MCCopy", cc_MCCopy }, { "MCCut", cc_MCCut }, { "MCDoAction", cc_MCDoAction },
    { "MCDraw", cc_MCDraw }, { "MCDrawBadge", cc_MCDrawBadge }, { "MCEnableEditing", cc_MCEnableEditing },
    { "MCGetClip", cc_MCGetClip }, { "MCGetControllerBoundsRect", cc_MCGetControllerBoundsRect },
    { "MCGetControllerBoundsRgn", cc_MCGetControllerBoundsRgn }, { "MCGetControllerInfo", cc_MCGetControllerInfo },
    { "MCGetControllerPort", cc_MCGetControllerPort }, { "MCGetCurrentTime", cc_MCGetCurrentTime },
    { "MCGetIndMovie", cc_MCGetIndMovie }, { "MCGetInterfaceElement", cc_MCGetInterfaceElement },
    { "MCGetMenuString", cc_MCGetMenuString }, { "MCGetVisible", cc_MCGetVisible },
    { "MCGetWindowRgn", cc_MCGetWindowRgn }, { "MCIdle", cc_MCIdle }, { "MCInvalidate", cc_MCInvalidate },
    { "MCIsControllerAttached", cc_MCIsControllerAttached }, { "MCIsEditingEnabled", cc_MCIsEditingEnabled },
    { "MCIsPlayerEvent", cc_MCIsPlayerEvent }, { "MCKey", cc_MCKey }, { "MCMovieChanged", cc_MCMovieChanged },
    { "MCNewAttachedController", cc_MCNewAttachedController }, { "MCPaste", cc_MCPaste },
    { "MCPositionController", cc_MCPositionController }, { "MCPtInController", cc_MCPtInController },
    { "MCRemoveAMovie", cc_MCRemoveAMovie }, { "MCRemoveAllMovies", cc_MCRemoveAllMovies },
    { "MCRemoveMovie", cc_MCRemoveAllMovies }, { "MCSetActionFilter", cc_MCSetActionFilter },
    { "MCSetActionFilterWithRefCon", cc_MCSetActionFilterWithRefCon }, { "MCSetClip", cc_MCSetClip },
    { "MCSetControllerAttached", cc_MCSetControllerAttached },
    { "MCSetControllerBoundsRect", cc_MCSetControllerBoundsRect }, { "MCSetControllerPort", cc_MCSetControllerPort },
    { "MCSetDuration", cc_MCSetDuration }, { "MCSetMovie", cc_MCSetMovie }, { "MCSetUpEditMenu", cc_MCSetUpEditMenu },
    { "MCSetVisible", cc_MCSetVisible }, { "MCUndo", cc_MCUndo },
    /* Tune player and note allocator */
    { "NACopyrightDialog", cc_NACopyrightDialog }, { "NADisposeNoteChannel", cc_NADisposeNoteChannel },
    { "NAFindNoteChannelTone", cc_NAFindNoteChannelTone }, { "NAGetController", cc_NAGetController },
    { "NAGetDefaultMIDIInput", cc_NAGetDefaultMIDIInput }, { "NAGetIndNoteChannel", cc_NAGetIndNoteChannel },
    { "NAGetKnob", cc_NAGetKnob }, { "NAGetMIDIPorts", cc_NAGetMIDIPorts },
    { "NAGetNoteChannelInfo", cc_NAGetNoteChannelInfo }, { "NAGetNoteRequest", cc_NAGetNoteRequest },
    { "NAGetRegisteredMusicDevice", cc_NAGetRegisteredMusicDevice },
    { "NALoseDefaultMIDIInput", cc_NALoseDefaultMIDIInput }, { "NANewNoteChannel", cc_NANewNoteChannel },
    { "NANewNoteChannelFromAtomicInstrument", cc_NANewNoteChannelFromAtomicInstrument },
    { "NAPickArrangement", cc_NAPickArrangement }, { "NAPickEditInstrument", cc_NAPickEditInstrument },
    { "NAPickInstrument", cc_NAPickInstrument }, { "NAPlayNote", cc_NAPlayNote },
    { "NAPrerollNoteChannel", cc_NAPrerollNoteChannel }, { "NARegisterMusicDevice", cc_NARegisterMusicDevice },
    { "NAResetNoteChannel", cc_NAResetNoteChannel }, { "NASaveMusicConfiguration", cc_NASaveMusicConfiguration },
    { "NASendMIDI", cc_NASendMIDI }, { "NASetAtomicInstrument", cc_NASetAtomicInstrument },
    { "NASetController", cc_NASetController }, { "NASetDefaultMIDIInput", cc_NASetDefaultMIDIInput },
    { "NASetInstrument", cc_NASetInstrumentNumber }, { "NASetInstrumentNumber", cc_NASetInstrumentNumber },
    { "NASetInstrumentNumberInterruptSafe", cc_NASetInstrumentNumberInterruptSafe }, { "NASetKnob", cc_NASetKnob },
    { "NASetNoteChannelBalance", cc_NASetNoteChannelBalance },
    { "NASetNoteChannelInstrument", cc_NASetInstrumentNumber },
    { "NASetNoteChannelSoundLocalization", cc_NASetNoteChannelSoundLocalization },
    { "NASetNoteChannelVolume", cc_NASetNoteChannelVolume }, { "NAStuffToneDescription", cc_NAStuffToneDescription },
    { "NATask", cc_NATask }, { "NAUnregisterMusicDevice", cc_NAUnregisterMusicDevice },
    { "NAUnrollNoteChannel", cc_NAUnrollNoteChannel }, { "NAUseDefaultMIDIInput", cc_NAUseDefaultMIDIInput },
    { "TuneGetIndexedNoteChannel", cc_TuneGetIndexedNoteChannel },
    { "TuneGetNoteAllocator", cc_TuneGetNoteAllocator }, { "TuneGetPartMix", cc_TuneGetPartMix },
    { "TuneGetStatus", cc_TuneGetStatus }, { "TuneGetTimeBase", cc_TuneGetTimeBase },
    { "TuneGetTimeScale", cc_TuneGetTimeScale }, { "TuneGetVolume", cc_TuneGetVolume },
    { "TuneInstant", cc_TuneInstant }, { "TunePreroll", cc_TunePreroll }, { "TuneQueue", cc_TuneQueue },
    { "TuneSetBalance", cc_TuneSetBalance }, { "TuneSetHeader", cc_TuneSetHeader },
    { "TuneSetHeaderWithSize", cc_TuneSetHeaderWithSize }, { "TuneSetNoteChannels", cc_TuneSetNoteChannels },
    { "TuneSetPartMix", cc_TuneSetPartMix }, { "TuneSetPartTranspose", cc_TuneSetPartTranspose },
    { "TuneSetSofter", cc_TuneSetSofter }, { "TuneSetSoundLocalization", cc_TuneSetSoundLocalization },
    { "TuneSetTimeScale", cc_TuneSetTimeScale }, { "TuneSetVolume", cc_TuneSetVolume }, { "TuneStop", cc_TuneStop },
    { "TuneTask", cc_TuneTask }, { "TuneUnroll", cc_TuneUnroll },
    /* Component Manager */
    { "CaptureComponent", cm_CaptureComponent }, { "CloseComponent", cm_CloseComponent },
    { "CloseComponentResFile", cm_CloseComponentResFile }, { "CountComponentInstances", cm_CountComponentInstances },
    { "CountComponents", cm_CountComponents }, { "DelegateComponentCall", cm_DelegateComponentCall },
    { "FindNextComponent", cm_FindNextComponent }, { "GetComponentIconSuite", cm_GetComponentIconSuite },
    { "GetComponentIndString", cm_GetComponentIndString }, { "GetComponentInfo", cm_GetComponentInfo },
    { "GetComponentInstanceA5", cm_GetComponentInstanceA5 },
    { "GetComponentInstanceError", cm_GetComponentInstanceError },
    { "GetComponentInstanceStorage", cm_GetComponentInstanceStorage },
    { "GetComponentListModSeed", cm_GetComponentListModSeed }, { "GetComponentRefcon", cm_GetComponentRefcon },
    { "GetComponentResource", cm_GetComponentResource }, { "GetComponentTypeModSeed", cm_GetComponentTypeModSeed },
    { "OpenAComponent", cm_OpenAComponent }, { "OpenAComponentResFile", cm_OpenAComponentResFile },
    { "OpenADefaultComponent", cm_OpenADefaultComponent }, { "OpenComponent", cm_OpenComponent },
    { "OpenComponentResFile", cm_OpenComponentResFile }, { "OpenDefaultComponent", cm_OpenDefaultComponent },
    { "RegisterComponent", cm_RegisterComponent }, { "RegisterComponentResource", cm_RegisterComponentResource },
    { "RegisterComponentResourceFile", cm_RegisterComponentResourceFile },
    { "ResolveComponentAlias", cm_ResolveComponentAlias }, { "SetComponentInstanceA5", cm_SetComponentInstanceA5 },
    { "SetComponentInstanceError", cm_SetComponentInstanceError },
    { "SetComponentInstanceStorage", cm_SetComponentInstanceStorage },
    { "SetComponentRefcon", cm_SetComponentRefcon }, { "SetDefaultComponent", cm_SetDefaultComponent },
    { "UncaptureComponent", cm_UncaptureComponent }, { "UnregisterComponent", cm_UnregisterComponent },
    /* MathLib */
    { "acos", m_acos }, { "acosh", m_acosh }, { "annuity", m_annuity }, { "asin", m_asin },
    { "asinh", m_asinh }, { "atan", m_atan }, { "atan2", m_atan2 }, { "atanh", m_atanh },
    { "cbrt", m_cbrt }, { "ceil", m_ceil }, { "compound", m_compound }, { "copysign", m_copysign },
    { "cos", m_cos }, { "cosh", m_cosh }, { "dec2num", m_dec2num }, { "dec2str", m_dec2str },
    { "dtox80", m_dtox80 }, { "erf", m_erf }, { "erfc", m_erfc }, { "exp", m_exp }, { "exp2", m_exp2 },
    { "expm1", m_expm1 }, { "fabs", m_fabs }, { "fdim", m_fdim },
    { "floor", m_floor }, { "fmax", m_fmax }, { "fmin", m_fmin }, { "fmod", m_fmod }, { "frexp", m_frexp },
    { "gamma", m_gamma }, { "hypot", m_hypot }, { "ldexp", m_ldexp }, { "ldtox80", m_ldtox80 },
    { "lgamma", m_lgamma }, { "log", m_log }, { "log10", m_log10 }, { "log1p", m_log1p },
    { "log2", m_log2 }, { "logb", m_logb }, { "modf", m_modf },
    { "nearbyint", m_nearbyint }, { "nextafterd", m_nextafterd }, { "num2dec", m_num2dec }, { "pow", m_pow },
    { "randomx", m_randomx }, { "remainder", m_remainder }, { "remquo", m_remquo }, { "rint", m_rint },
    { "rinttol", m_rinttol }, { "round", m_round }, { "roundtol", m_roundtol }, { "scalb", m_scalb },
    { "sin", m_sin }, { "sinh", m_sinh }, { "sqrt", m_sqrt }, { "str2dec", m_str2dec }, { "tan", m_tan },
    { "tanh", m_tanh }, { "trunc", m_trunc }, { "x80tod", m_x80tod }, { "x80told", m_x80told },
    { "__fpclassifyd", m_fpclassifyd }, { "__inf", m_inf }, { "__isfinited", m_isfinited },
    { "__isnan", m_isnand }, { "__isnand", m_isnand }, { "__isnanf", m_isnand }, { "__isnormald", m_isnormald },
    { "__signbitd", m_signbitd },
    /* The float forms: a float argument comes in an FPR as a double. */
    { "__fpclassifyf", m_fpclassifyd }, { "__isfinitef", m_isfinited }, { "__isnormalf", m_isnormald },
    { "__signbitf", m_signbitd },
    /* Idle */
    { "EnableIdle", g_nothing }, { "DisableIdle", g_nothing },
    /* InterfaceLib glue */
    { "ApplicationZone", g_ApplicationZone }, { "C2PStr", g_c2pstr }, { "CloseDriver", g_CloseDriver },
    { "CopyCStringToPascal", g_CopyCStringToPascal }, { "CopyPascalStringToC", g_CopyPascalStringToC },
    { "DialogCopy", g_DialogCopy }, { "DialogCut", g_DialogCut }, { "DialogDelete", g_DialogDelete },
    { "DialogPaste", g_DialogPaste }, { "EqualString", g_EqualString }, { "Fix2X", g_Fix2X },
    { "Frac2X", g_Frac2X }, { "GetDCtlEntry", g_GetDCtlEntry }, { "GetIndString", g_GetIndString },
    { "GetMBarHeight", g_GetMBarHeight }, { "GetOSTrapAddress", g_GetOSTrapAddress },
    { "GetSoundVol", g_GetSoundVol }, { "GetToolboxTrapAddress", g_GetToolboxTrapAddress },
    { "GetTrapAddress", g_GetTrapAddress }, { "GetWDInfo", g_GetWDInfo }, { "HLockHi", g_HLockHi },
    { "IUCompString", g_IUCompString }, { "IUEqualString", g_IUEqualString }, { "MakeDataExecutable", g_make_data_executable },
    { "MemError", g_MemError }, { "NGetTrapAddress", g_NGetTrapAddress }, { "NSetTrapAddress", g_NSetTrapAddress },
    { "OpenDriver", g_OpenDriver }, { "P2CStr", g_p2cstr }, { "RelString", g_RelString },
    { "SetA5", g_SetA5 }, { "SetCurrentA5", g_SetCurrentA5 }, { "SetOSTrapAddress", g_SetOSTrapAddress },
    { "SetSoundVol", g_SetSoundVol }, { "SetToolboxTrapAddress", g_SetToolboxTrapAddress },
    { "SysBreak", g_nothing }, { "SysBreakFunc", g_nothing }, { "SysBreakStr", g_nothing },
    { "SystemZone", g_SystemZone }, { "UpperString", g_UpperString }, { "X2Fix", g_X2Fix },
    { "UpperText", g_UpperText }, { "UppercaseText", g_UpperText }, { "UprText", g_UpperText }, { "LowerText", g_LowerText },
    { "LowercaseText", g_LowerText }, { "GetEvQHdr", g_GetEvQHdr }, { "GetVCBQHdr", g_GetVCBQHdr },
    { "GetDrvQHdr", g_GetDrvQHdr }, { "GetFSQHdr", g_GetFSQHdr }, { "GetVBLQHdr", g_GetVBLQHdr },
    { "GetApplLimit", g_GetApplLimit }, { "GetGrayRgn", g_GetGrayRgn }, { "GetAppFont", g_GetAppFont },
    { "GetSysFont", g_GetSysFont }, { "GetDefFontSize", g_GetDefFontSize }, { "GetTime", g_GetTime },
    { "Microseconds", g_Microseconds }, { "ReadLocation", g_ReadLocation }, { "Control", g_Control },
    { "Status", g_Status }, { "KillIO", g_KillIO },
    { "StdFilterProc", g_StdFilterProc },
    { "X2Frac", g_X2Frac }, { "_BreakPoint", g_nothing }, { "c2pstr", g_c2pstr },
    { "c2pstrcpy", g_c2pstrcpy }, { "p2cstr", g_p2cstr }, { "p2cstrcpy", g_p2cstrcpy },
    { "FlushCodeCache", g_nothing }, { "FlushDataCache", g_nothing }, { "FlushCodeCacheRange", g_noErr },
};
const unsigned lib_export_count = sizeof lib_exports / sizeof lib_exports[0];

/* ---------------------------------------------------------------------- */
/* Data */

/* The data exports ppc_data_export makes, by name only. */
bool ppc_data_export_known(const char *name)
{
    if (ml_data_export_known(name))
        return true;
    if (stdclib_data_known(name))
        return true;
    if (carbon_cf_data_known(name))
        return true;
    static const char *const names[] = { "CallComponentUPP", "pi", "__huge_val" };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(name, names[i]))
            return true;
    return false;
}

gaddr ppc_data_export(const char *name)
{
    if (ml_data_export_known(name))
        return ml_data_export(name);
    if (stdclib_data_known(name))
        return stdclib_data_export(name);
    if (carbon_cf_data_known(name))
        return carbon_cf_data_export(name);
    if (!strcmp(name, "CallComponentUPP"))
        return mm_call_component_upp();
    if (!strcmp(name, "pi")) {
        gaddr p = tb_NewPtr(8, true, true);
        wr_double(p, M_PI);
        return p;
    }
    if (!strcmp(name, "__huge_val")) { /* HUGE_VAL: +infinity */
        gaddr p = tb_NewPtr(8, true, true);
        wr_double(p, INFINITY);
        return p;
    }
    return 0;
}
