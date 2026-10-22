/* OS Utilities, trap table access and other small routines. */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "m68k.h"
#include "tb_internal.h"
#include "trap.h"

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
    if (n > 0 && events_host_attached()) {
        /* Really wait, as a Mac did, with interrupt-time work (sound
         * refills, Time Manager and VBL tasks) going on in 5 ms slices.
         * Skipping the clock ahead instead would leave it ahead of the
         * host's: tasks paced by it then catch up all at once, and the
         * sound they queue plays seconds late (Apeiron's). */
        machine_advance_ticks();
        uint32_t until = rd32(LM_Ticks) + (uint32_t)n;
        while ((int32_t)(rd32(LM_Ticks) - until) < 0 && !g_cpu->halted) {
            struct timespec ts = { 0, 5000000 };
            nanosleep(&ts, NULL);
            machine_advance_ticks();
            sound_pump();
            time_pump();
        }
    } else if (n > 0) {
        /* Headless, the clock follows instructions: it skips ahead rather
         * than spin. */
        machine_skip_time((uint64_t)n * 1000000000u / 60);
    }
    machine_advance_ticks();
    *ftp = rd32(LM_Ticks);
}

void tb_SysBeep(int16_t i) { sound_beep(); }

/* ---- Environment ---- */

/* Parameter RAM: the low-memory copy (SysParam) is the only one there is,
 * always valid, so InitUtil has nothing to set up and WriteParam nothing
 * to write through to. */
int16_t tb_InitUtil(void) { return noErr; }
int16_t tb_WriteParam(void) { return noErr; }

int16_t tb_SysEnvirons(int16_t vers, gaddr p)
{
    if (vers < 1)
        return -5501; /* envBadVers */
    SysEnvRec_set_environsVersion(p, vers > 2 ? 2 : vers);
    /* As System 7 does, from Gestalt: the machine (its Gestalt number less
     * two), the processor and the FPU. */
    uint32_t mach = gestalt_value(fourcc("mach"), 0), proc = gestalt_value(fourcc("proc"), 3);
    SysEnvRec_set_machineType(p, (int16_t)(mach > 2 ? mach - 2 : 0));
    SysEnvRec_set_systemVersion(p, (int16_t)rd16(LM_SysVersion));
    SysEnvRec_set_processor(p, (int16_t)proc); /* numbered alike: env68040 = 5 */
    SysEnvRec_set_hasFPU(p, gestalt_value(fourcc("fpu "), 0) != 0);
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
    if (!newTraps && (n & 0xF000) == 0xA000) {
        tool = (n & 0x0800) != 0; /* a whole trap word says which table */
    } else if (!newTraps) {
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
/* A desk accessory by name: one in the Apple Menu Items folder opens as
 * the Finder would open it (applications launch). */
int16_t tb_OpenDeskAcc(gaddr acc)
{
    uint8_t name[256];
    int n = rd8(acc);
    gget(name, acc + 1, (uint32_t)n);
    if (n && name[0] == 0) { /* desk accessories' names start with a NUL */
        memmove(name, name + 1, (size_t)--n);
    }
    fm_open_apple_menu_item(name, n);
    return 0;
}
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

/* StripDiacritics(text, len, script): the marks taken off, in place. */
void tb_StripDiacritics(gaddr textp, int16_t len, int16_t script) { intl_convert_text(textp, len, false, true); }

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

/* LowerText (A056) and its variants: A0 the text, D0.W its length; trap bit
 * $0200 strips diacritical marks and $0400 makes upper case (StripText
 * only strips).  The script's 'itl2' tables (intl.c). */
void raw_LowerText(cpu68k *c, uint16_t tw)
{
    intl_convert_text(c->a[0], (int32_t)(c->d[0] & 0xFFFF), (tw & 0x0400) != 0, (tw & 0x0200) != 0);
    c->d[0] = 0;
}

/* LowercaseText, UppercaseText, StripDiacritics and UppercaseStripDiacritics:
 * one ScriptUtil entry, the selector $400 for upper case and $800 for
 * stripping marks (Roman script rules for every script). */
void tb_TextUtilFunctions(gaddr textp, int16_t len, int16_t script, int16_t selector)
{
    intl_convert_text(textp, len, (selector & 0x0400) != 0, (selector & 0x0800) != 0);
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
    wr16(LM_DSErrCode, (uint16_t)g_cpu->d[0]);
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
    static const char from[] = "1234567890-=[]\\;',./`", to[] = "!@#$%^&*()_+{}|:\"<>?~";
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

/* Key1Trans and Key2Trans (low-memory vectors, called by JSR): the key
 * code in D2.W and the modifier flags' high byte in D1, the character
 * returned in D0. */
void key_trans_native(cpu68k *c, uint16_t tw)
{
    uint16_t keycode = (uint16_t)(((c->d[1] & 0xFF) << 8) | (c->d[2] & 0x7F));
    c->d[0] = tb_KeyTranslate(0, keycode, 0) & 0xFF;
}

/* DrvrInstall (A03D): A0 the driver, D0 its refnum. */
void raw_DrvrInstall(cpu68k *c, uint16_t tw) { c->d[0] = (uint32_t)(int32_t)driver_install(c->a[0], (int16_t)c->d[0]); }

/* The Cursor Device Manager: there are no cursor devices but the ones a
 * program makes (CursorDeviceNewDevice), which keep what is set in them
 * and move nothing.  args are the call's arguments, left to right. */
enum {
    cd_next = 0, cd_devID = 16, cd_resolution = 20, cd_devClass = 24, cd_cntButtons = 25, cd_buttons = 27,
    cd_buttonOp = 28, cd_buttonData = 68, cd_doubleClickTime = 100, cd_acceleration = 104, cd_size = 138,
};
static gaddr cursor_devices;

void cursor_devices_reset(void) { cursor_devices = 0; }

int16_t cursor_device(uint16_t sel, const uint32_t *args)
{
    gaddr dev = args[0];
    switch (sel) {
    case 0x0B: { /* CursorDeviceNextDevice(*ourDevice): the one after, the first after nil */
        gaddr at = dev ? rd32(dev) : 0;
        wr32(dev, at ? rd32(at + cd_next) : cursor_devices);
        return noErr;
    }
    case 0x0C: { /* CursorDeviceNewDevice(*ourDevice): a mouse of one button */
        gaddr d = tb_NewPtr(cd_size, true, true);
        if (!d)
            return memFullErr;
        wr32(d + cd_devID, fourcc("mous"));
        wr32(d + cd_resolution, 100u << 16);
        wr8(d + cd_devClass, 1); /* mouse */
        wr8(d + cd_cntButtons, 1);
        wr32(d + cd_doubleClickTime, rd32(LM_DoubleTime));
        wr32(d + cd_acceleration, 1u << 16);
        wr32(d + cd_next, cursor_devices);
        cursor_devices = d;
        wr32(dev, d);
        return noErr;
    }
    default:
        break;
    }
    if (!dev)
        return paramErr;
    switch (sel) {
    case 0x03: /* CursorDeviceButtons(dev, buttons) */
        wr8(dev + cd_buttons, (uint8_t)args[1]);
        break;
    case 0x04: /* CursorDeviceButtonDown */
        wr8(dev + cd_buttons, (uint8_t)(rd8(dev + cd_buttons) | 1));
        break;
    case 0x05: /* CursorDeviceButtonUp */
        wr8(dev + cd_buttons, (uint8_t)(rd8(dev + cd_buttons) & ~1));
        break;
    case 0x06: /* CursorDeviceButtonOp(dev, button, opcode, data) */
        if (args[1] >= 8)
            return paramErr;
        wr8(dev + cd_buttonOp + args[1], (uint8_t)args[2]);
        wr32(dev + cd_buttonData + 4 * args[1], args[3]);
        break;
    case 0x07: /* CursorDeviceSetButtons(dev, count) */
        wr8(dev + cd_cntButtons, (uint8_t)args[1]);
        break;
    case 0x08: /* CursorDeviceSetAcceleration(dev, acceleration) */
        wr32(dev + cd_acceleration, args[1]);
        break;
    case 0x09: /* CursorDeviceDoubleTime(dev, ticks) */
        wr32(dev + cd_doubleClickTime, args[1]);
        break;
    case 0x0A: /* CursorDeviceUnitsPerInch(dev, resolution) */
        wr32(dev + cd_resolution, args[1]);
        break;
    case 0x0D: /* CursorDeviceDisposeDevice */
        if (cursor_devices == dev)
            cursor_devices = rd32(dev + cd_next);
        for (gaddr at = cursor_devices; at; at = rd32(at + cd_next))
            if (rd32(at + cd_next) == dev)
                wr32(at + cd_next, rd32(dev + cd_next));
        tb_DisposePtr(dev);
        break;
    default: /* Move, MoveTo, Flush: the cursor is the host's */
        break;
    }
    return noErr;
}

/* CursorDeviceDispatch (AADB): the selector in D0, Pascal arguments. */
void raw_CursorDeviceDispatch(cpu68k *c, uint16_t tw)
{
    /* Each call's arguments, left to right: 4 a long or pointer, 2 a word. */
    static const char *const args[14] = { "444", "444", "4", "42", "4", "4", "4224", "42", "44", "44", "44", "4", "4", "4" };
    uint16_t sel = (uint16_t)c->d[0];
    const char *a = sel < 14 ? args[sel] : "4";
    uint32_t n = 0, v[4] = { 0 };
    for (const char *p = a; *p; p++)
        n += (uint32_t)(*p - '0');
    uint32_t ret = cpu_pop32(c);
    gaddr at = c->a[7] + n;
    for (int i = 0; a[i]; i++) {
        at -= (gaddr)(a[i] - '0');
        v[i] = a[i] == '4' ? rd32(at) : (uint32_t)(int32_t)(int16_t)rd16(at);
    }
    int16_t err = cursor_device(sel, v);
    c->a[7] += n;
    wr16(c->a[7], (uint16_t)err);
    cpu_push32(c, ret);
}

/* MemoryDispatch (A05C), virtual memory's calls, selector in D0: with all
 * memory real and resident, holding and locking (HoldMemory, UnholdMemory,
 * LockMemory, UnlockMemory, LockMemoryContiguous) have nothing to do, as
 * on a Mac with virtual memory off. */
void raw_MemoryDispatch(cpu68k *c, uint16_t tw)
{
    c->d[0] = (uint16_t)c->d[0] <= 4 ? 0 : (uint32_t)-50; /* noErr; paramErr */
}

/* Operating System queues: a header of qFlags (word), qHead and qTail; each
 * element begins with its link to the next. */
enum { qErr = -1 };

void tb_Enqueue(gaddr e, gaddr h)
{
    if (!e || !h)
        return;
    wr32(e, 0);
    gaddr tail = rd32(h + 6);
    if (!rd32(h + 2) || !tail)
        wr32(h + 2, e);
    else
        wr32(tail, e);
    wr32(h + 6, e);
}

int16_t tb_Dequeue(gaddr e, gaddr h)
{
    if (!e || !h)
        return qErr;
    gaddr prev = 0;
    for (gaddr q = rd32(h + 2); q; prev = q, q = rd32(q)) {
        if (q != e)
            continue;
        gaddr next = rd32(q);
        if (prev)
            wr32(prev, next);
        else
            wr32(h + 2, next);
        if (rd32(h + 6) == q)
            wr32(h + 6, prev);
        return noErr;
    }
    return qErr;
}

/* ---- ADB Manager ----
 * Two devices, as on a desktop Mac: an extended keyboard at address 2 and
 * a mouse at address 3.  Input comes through the Event Manager; each device
 * also has a service routine of its own, which does nothing, so a program
 * that installs one in front of it (SetADBInfo) can pass the data on as it
 * should.  The mouse's movement goes to the installed routine as ADB's
 * auto-polling delivered it, register 0 at interrupt time (Apeiron steers
 * by it), and ADBOp answers Talk commands with the devices' registers. */

static const struct { int8_t type, addr, handler; } adb_devices[] = { { 2, 2, 2 }, { 1, 3, 1 } };
enum { adb_count = 2 };
static gaddr adb_service[adb_count], adb_data[adb_count];
static gaddr adb_own_service; /* the devices' own routine: it does nothing */
static gaddr adb_buffer;      /* a register's bytes for a service routine */
static bool adb_last_button;

static int adb_index(int addr)
{
    for (int k = 0; k < adb_count; k++)
        if (adb_devices[k].addr == (addr & 0xF))
            return k;
    return -1;
}

static void adb_fill(gaddr adbp, int k)
{
    if (!adbp)
        return;
    wr8(adbp, (uint8_t)adb_devices[k].type);
    wr8(adbp + 1, (uint8_t)adb_devices[k].addr);
    wr32(adbp + 2, adb_service[k]);
    wr32(adbp + 6, adb_data[k]);
}

int16_t tb_CountADBs(void) { return adb_count; }

/* The index'th device (from 1): its ADB address, or an error. */
int16_t tb_GetIndADB(gaddr adbp, int16_t index)
{
    if (index < 1 || index > adb_count)
        return -1;
    adb_fill(adbp, index - 1);
    return adb_devices[index - 1].addr;
}

int16_t tb_GetADBInfo(gaddr adbp, int16_t address)
{
    int k = adb_index(address);
    if (k < 0)
        return -1;
    adb_fill(adbp, k);
    return noErr;
}

/* ADBSetInfoBlock: siServiceRtPtr, siDataAreaAddr */
int16_t tb_SetADBInfo(gaddr adbp, int16_t address)
{
    int k = adb_index(address);
    if (k < 0)
        return -1;
    adb_service[k] = adbp ? rd32(adbp) : 0;
    adb_data[k] = adbp ? rd32(adbp + 4) : 0;
    if (adb_devices[k].type == 1) { /* movement from now on */
        int dh, dv;
        ev_take_motion(&dh, &dv, 0);
        adb_last_button = tb_Button();
    }
    return noErr;
}

void tb_ADBReInit(void) {}

/* A device register's bytes, as a Pascal string, into buf. */
static void adb_talk(int k, int reg, gaddr buf)
{
    uint8_t b[2] = { 0xFF, 0xFF };
    int n = 2;
    if (reg == 3) {
        b[0] = (uint8_t)(0x60 | adb_devices[k].addr);
        b[1] = (uint8_t)adb_devices[k].handler;
    } else if (reg == 0 && adb_devices[k].type == 1) {
        /* The mouse's motion since the last packet (not the cursor's: a
         * program moving the cursor is no motion), as much as a packet
         * holds; the rest goes in the next.  Halved, as an ADB mouse
         * counted far fewer steps an inch than the host's pointer moves
         * (Apeiron steered at the pointer's rate was touchy); the odd
         * step is kept for the next packet. */
        static int odd_h, odd_v;
        int dh, dv;
        ev_take_motion(&dh, &dv, 128);
        dh += odd_h, dv += odd_v;
        odd_h = dh % 2, odd_v = dv % 2;
        dh /= 2, dv /= 2;
        bool down = tb_Button();
        adb_last_button = down;
        b[0] = (uint8_t)((down ? 0 : 0x80) | (dv & 0x7F));
        b[1] = (uint8_t)(0x80 | (dh & 0x7F));
    }
    if (!buf)
        return;
    wr8(buf, (uint8_t)n);
    gput(buf + 1, b, (uint32_t)n);
}

/* A service or completion routine, as the ADB interrupt called it: A0 the
 * buffer, A1 the routine, A2 its data area, D0 the command; registers
 * preserved. */
static void adb_call(gaddr fn, gaddr buf, gaddr data, uint8_t cmd)
{
    cpu68k *c = g_cpu;
    uint32_t d[8], a[8];
    uint16_t sr = c->sr;
    memcpy(d, c->d, sizeof d);
    memcpy(a, c->a, sizeof a);
    c->a[0] = buf;
    c->a[1] = fn;
    c->a[2] = data;
    c->d[0] = cmd;
    guest_call_regs(fn);
    memcpy(c->d, d, sizeof d);
    memcpy(c->a, a, sizeof a);
    c->sr = sr;
}

/* _ADBOp: A0 = ADBOpBlock (dataBuffPtr, opServiceRtPtr, opDataAreaPtr),
 * D0.B = command (address << 4, then Talk 3 / Listen 2 / Flush, register). */
static void adbop_native(cpu68k *c, uint16_t tw)
{
    (void)tw;
    gaddr pb = c->a[0];
    uint8_t cmd = (uint8_t)c->d[0];
    int k = adb_index(cmd >> 4), kind = (cmd >> 2) & 3, reg = cmd & 3;
    gaddr buf = pb ? rd32(pb) : 0, fn = pb ? rd32(pb + 4) : 0, data = pb ? rd32(pb + 8) : 0;
    if (k >= 0 && kind == 3)
        adb_talk(k, reg, buf);
    c->d[0] = noErr;
    if (fn)
        adb_call(fn, buf, data, cmd);
}

static void adb_own_native(cpu68k *c, uint16_t tw) { (void)c; (void)tw; }

/* Interrupt time: the mouse's movement to the routine a program installed. */
void adb_pump(void)
{
    if (!adb_service[1] || adb_service[1] == adb_own_service)
        return;
    bool down = tb_Button();
    if (!ev_motion_pending() && down == adb_last_button)
        return;
    if (!adb_buffer)
        adb_buffer = tb_NewPtr(8, true, true); /* system heap */
    /* Talk register 0 from address 3, packet by packet until the movement
     * is all delivered (a few a time, as the mouse reported every tick). */
    for (int i = 0; i < 8 && (ev_motion_pending() || tb_Button() != adb_last_button); i++) {
        adb_talk(1, 0, adb_buffer);
        adb_call(adb_service[1], adb_buffer, adb_data[1], 0x3C);
    }
}

void adb_install(void)
{
    adb_own_service = native_register(adb_own_native, "ADBService", 0);
    for (int k = 0; k < adb_count; k++) {
        adb_service[k] = adb_own_service;
        adb_data[k] = 0;
    }
    adb_buffer = 0;
    { int dh, dv; ev_take_motion(&dh, &dv, 0); } /* motion from now on */
    adb_last_button = false;
    wr32(OS_TRAP_TABLE + 0x7Cu * 4, native_register(adbop_native, "ADBOp", 0xA07C));
}
