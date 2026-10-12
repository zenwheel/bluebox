/* OS Utilities, trap table access and other small routines. */
#include <stdio.h>
#include <time.h>

#include "tb_internal.h"

/* ---- Time ---- */

/* Seconds between 1904-01-01 and 1970-01-01. */
#define MAC_EPOCH_OFFSET 2082844800u

uint32_t mac_now(void)
{
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    /* Mac time is local time. */
    return (uint32_t)((int64_t)t + lt.tm_gmtoff + MAC_EPOCH_OFFSET);
}

uint32_t tb_TickCount(void)
{
    sound_pump();
    return rd32(LM_Ticks);
}

int16_t tb_ReadDateTime(gaddr secs)
{
    wr32(secs, mac_now());
    return noErr;
}

void tb_GetDateTime(gaddr p) { wr32(p, mac_now()); }

void tb_Delay(int32_t n, uint32_t *ftp)
{
    /* Time is advanced by the run loop; skip ahead rather than spin. */
    if (n > 0)
        wr32(LM_Ticks, rd32(LM_Ticks) + (uint32_t)n);
    *ftp = rd32(LM_Ticks);
}

void tb_SysBeep(int16_t i) { sound_beep(); }

/* ---- Environment ---- */

int16_t tb_SysEnvirons(int16_t vers, gaddr p)
{
    if (vers < 1)
        return -5501; /* envBadVers */
    SysEnvRec_set_environsVersion(p, vers > 2 ? 2 : vers);
    SysEnvRec_set_machineType(p, 0);   /* envMachUnknown: newer than SysEnvirons knows */
    SysEnvRec_set_systemVersion(p, (int16_t)rd16(LM_SysVersion));
    SysEnvRec_set_processor(p, 3);     /* env68020 */
    SysEnvRec_set_hasFPU(p, 0);
    SysEnvRec_set_hasColorQD(p, 1);
    SysEnvRec_set_keyBoardType(p, 4);  /* envAExtendKbd */
    SysEnvRec_set_atDrvrVersNum(p, 0);
    SysEnvRec_set_sysVRefNum(p, fm_system_vref());
    return vers > 2 ? -5502 : noErr;   /* envVersTooBig */
}

gaddr tb_StripAddress(gaddr p) { return g_cpu ? p & g_cpu->addr_mask : p; }
void tb_SwapMMUMode(uint32_t *bp) { *bp = 1; }
void tb_HWPriv(int32_t d0, int32_t a0) { g_cpu->d[0] = 0; }

void tb_Debugger(void) { cpu_halt(g_cpu, "Debugger() called"); }

void tb_DebugStr(gaddr p)
{
    char s[256];
    pstr_to_c(p, s, sizeof s);
    cpu_halt(g_cpu, "DebugStr: %s", s);
}

/* ---- Trap table access ---- */

static uint32_t table_slot(uint16_t n, bool newTraps, bool tool)
{
    if (!newTraps) {
        n &= 0x1FF;
        tool = n > 0x4F && n != 0x54 && n != 0x57;
    }
    return tool ? TB_TRAP_TABLE + (n & 0x3FFu) * 4 : OS_TRAP_TABLE + (n & 0xFFu) * 4;
}

gaddr tb_GetTrapAddress(uint16_t n, bool newTraps, bool tool)
{
    return rd32(table_slot(n, newTraps, tool));
}

void tb_SetTrapAddress(gaddr addr, uint16_t n, bool newTraps, bool tool)
{
    LOGF(LOG_TRAPS, "SetTrapAddress(%04X) = %08X\n", n, addr);
    wr32(table_slot(n, newTraps, tool), addr);
}

/* ---- Desk accessories (none are installed) ---- */

int8_t tb_SystemEdit(int16_t editcmd) { return 0; } /* not handled: the app does it */
int16_t tb_OpenDeskAcc(gaddr acc) { return 0; }
void tb_CloseDeskAcc(int16_t rn) {}
void tb_SystemMenu(int32_t menu) {}

/* ---- String comparison (register-based OS traps) ---- */

/* MacRoman upper case without diacriticals (for comparisons that ignore
 * marks) and with them. */
static uint8_t upper_mac(uint8_t c)
{
    if (c >= 'a' && c <= 'z')
        return (uint8_t)(c - 32);
    switch (c) {
    case 0x8A: return 0x80; case 0x8C: return 0x81; case 0x8D: return 0x82; case 0x8E: return 0x83;
    case 0x96: return 0x84; case 0x9A: return 0x85; case 0x9F: return 0x86; case 0x88: return 0xCB;
    case 0x8B: return 0xCC; case 0x9B: return 0xCD; case 0xBE: return 0xAE; case 0xBF: return 0xAF;
    case 0xCF: return 0xCE; case 0xD8: return 0xD9;
    default: return c;
    }
}

static uint8_t strip_mark(uint8_t c)
{
    static const char plain[] = "AACENOUaaaaaaceeeeiiiinooooouuuu";
    if (c >= 0x80 && c <= 0x9F)
        return (uint8_t)plain[c - 0x80];
    switch (c) {
    case 0xCB: case 0xCC: return 'A';
    case 0xCD: case 0xAF: return 'O';
    case 0xD8: return 'y';
    case 0xD9: return 'Y';
    default: return c;
    }
}

static int cmp_char(uint8_t a, uint8_t b, bool case_sens, bool ignore_marks)
{
    if (ignore_marks) {
        a = strip_mark(a);
        b = strip_mark(b);
    }
    if (!case_sens) {
        a = upper_mac(a);
        b = upper_mac(b);
    }
    return (int)a - (int)b;
}

/* CmpString (EqualString): A0, A1 the strings, D0 = len1 << 16 | len2.
 * Trap bits: $0200 ignore diacriticals, $0400 case sensitive.  D0 = 0 if
 * equal, else 1. */
void raw_CmpString(cpu68k *c, uint16_t tw)
{
    uint32_t a = c->a[0], b = c->a[1];
    int la = (int)(c->d[0] >> 16), lb = (int)(c->d[0] & 0xFFFF);
    bool case_sens = (tw & 0x0400) != 0, marks = (tw & 0x0200) != 0;
    int r = la != lb;
    for (int i = 0; !r && i < la; i++)
        r = cmp_char(rd8(a + (uint32_t)i), rd8(b + (uint32_t)i), case_sens, marks) != 0;
    c->d[0] = (uint32_t)r;
}

/* RelString: like CmpString, returning -1, 0 or 1 in D0. */
void raw_RelString(cpu68k *c, uint16_t tw)
{
    uint32_t a = c->a[0], b = c->a[1];
    int la = (int)(c->d[0] >> 16), lb = (int)(c->d[0] & 0xFFFF);
    bool case_sens = (tw & 0x0400) != 0, marks = (tw & 0x0200) != 0;
    int n = la < lb ? la : lb, r = 0;
    for (int i = 0; !r && i < n; i++)
        r = cmp_char(rd8(a + (uint32_t)i), rd8(b + (uint32_t)i), case_sens, marks);
    if (!r)
        r = la - lb;
    c->d[0] = (uint32_t)(r < 0 ? -1 : r > 0 ? 1 : 0);
}

/* UprString: A0 the text, D0 its length; bit $0200 also strips marks. */
void raw_UprString(cpu68k *c, uint16_t tw)
{
    uint32_t a = c->a[0];
    int n = (int)(c->d[0] & 0xFFFF);
    for (int i = 0; i < n; i++) {
        uint8_t ch = rd8(a + (uint32_t)i);
        if (tw & 0x0200)
            ch = strip_mark(ch);
        wr8(a + (uint32_t)i, upper_mac(ch));
    }
}

/* A character's comparison key: upper case unless case matters, without its
 * diacritical mark when marks don't. */
int mac_char_key(uint8_t c, bool case_sens, bool ignore_marks)
{
    if (ignore_marks)
        c = strip_mark(c);
    if (!case_sens)
        c = upper_mac(c);
    return c;
}

static uint8_t lower_mac(uint8_t c)
{
    if (c >= 'A' && c <= 'Z')
        return (uint8_t)(c + 32);
    for (int i = 0x80; i < 0x100; i++)
        if (i != c && upper_mac((uint8_t)i) == c)
            return (uint8_t)i;
    return c;
}

/* LowerText (A056) and its variants: A0 the text, D0.W its length; trap bit
 * $0200 strips diacritical marks and $0400 makes upper case. */
void raw_LowerText(cpu68k *c, uint16_t tw)
{
    uint32_t a = c->a[0];
    int n = (int)(c->d[0] & 0xFFFF);
    for (int i = 0; i < n; i++) {
        uint8_t ch = rd8(a + (uint32_t)i);
        if (tw & 0x0200)
            ch = strip_mark(ch);
        wr8(a + (uint32_t)i, (tw & 0x0400) ? upper_mac(ch) : lower_mac(ch));
    }
    c->d[0] = 0;
}

/* Extended parameter RAM: 256 bytes, kept for the session.  A0 the buffer,
 * D0 = count << 16 | first address. */
static uint8_t xpram[256];

void raw_ReadXPRam(cpu68k *c, uint16_t tw)
{
    uint32_t n = c->d[0] >> 16, at = c->d[0] & 0xFFFF;
    for (uint32_t i = 0; i < n; i++)
        wr8(c->a[0] + i, xpram[(at + i) & 0xFF]);
    c->d[0] = 0;
}

void raw_WriteXPRam(cpu68k *c, uint16_t tw)
{
    uint32_t n = c->d[0] >> 16, at = c->d[0] & 0xFFFF;
    for (uint32_t i = 0; i < n; i++)
        xpram[(at + i) & 0xFF] = rd8(c->a[0] + i);
    c->d[0] = 0;
}

/* SysError: on a real Mac, the bomb box.  The error code is in D0. */
void tb_SysError(int16_t code)
{
    cpu_halt(g_cpu, "SysError %d", (int16_t)g_cpu->d[0]);
}

void tb_InitAllPacks(void) {}
void tb_InitPack(int16_t id) {}

/* ---- KeyTranslate: a US 'KCHR' ----
 * keycode: the virtual key in bits 0-6, key up in bit 7, modifiers in the
 * high byte.  The result's low byte is the character. */

static const char key_plain[128] = {
    'a', 's', 'd', 'f', 'h', 'g', 'z', 'x', 'c', 'v', 0,   'b', 'q', 'w', 'e', 'r',  /* 0x00 */
    'y', 't', '1', '2', '3', '4', '6', '5', '=', '9', '7', '-', '8', '0', ']', 'o',  /* 0x10 */
    'u', '[', 'i', 'p', 13,  'l', 'j', '\'', 'k', ';', '\\', ',', '/', 'n', 'm', '.', /* 0x20 */
    9,   ' ', '`', 8,   0,   27,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,    /* 0x30 */
    0,   '.', 29,  '*', 0,   '+', 28,  27,  31,  0,   0,   '/', 3,   30,  '-', 0,    /* 0x40 */
    0,   '=', '0', '1', '2', '3', '4', '5', '6', '7', 0,   '8', '9', 0,   0,   0,    /* 0x50 */
    16,  16,  16,  16,  16,  16,  0,   16,  0,   16,  0,   16,  0,   16,  0,   16,   /* 0x60: F keys */
    0,   16,  5,   1,   11,  127, 16,  4,   16,  12,  16,  28,  29,  31,  30,  0,    /* 0x70 */
};

static char shifted(char ch)
{
    static const char from[] = "1234567890-=[]\;',./`", to[] = "!@#$%^&*()_+{}|:\"<>?~";
    if (ch >= 'a' && ch <= 'z')
        return (char)(ch - 32);
    const char *p = strchr(from, ch);
    return p && ch ? to[p - from] : ch;
}

uint32_t tb_KeyTranslate(gaddr kchr, uint16_t keycode, gaddr state)
{
    if (state)
        wr32(state, 0);
    char ch = key_plain[keycode & 0x7F];
    uint16_t mods = keycode & 0xFF00;
    bool letter = ch >= 'a' && ch <= 'z';
    if ((mods & 0x0200) || ((mods & 0x0400) && letter)) /* shift; caps lock for letters */
        ch = shifted(ch);
    if ((mods & 0x1000) && letter) /* control */
        ch = (char)(ch & 0x1F);
    return (uint8_t)ch;
}

/* DrvrInstall (A03D): A0 the driver, D0 its refnum.  Nothing calls drivers
 * here (the Device Manager answers for files and absent drivers), so the
 * installation is accepted and otherwise ignored. */
void raw_DrvrInstall(cpu68k *c, uint16_t tw) { c->d[0] = 0; }
