/*
 * The debugger.  Blue Box is a Mac with MacsBug installed: low memory says
 * so as MacsBug left it, and Debugger, DebugStr, SysBreak and the PowerPC
 * debugger calls enter it.  Entering it logs the message, where it came
 * from and the routine's MacsBug name, and goes on, as typing "g" in
 * MacsBug would; --debugger halt (BLUEBOX_DEBUGGER=halt) stops the run
 * there instead, as before.  A program that enters it over and over (an
 * assertion in its event loop) is logged at the first entry and then at
 * the 10th, 100th, 1000th ... from the same place, not every time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hw68k.h"
#include "log.h"
#include "machine.h"
#include "ppc.h"
#include "tb_internal.h"
#include "trap.h"

BB_HOST static int mode = -1; /* DEBUGGER_LOG, _HALT or _QUIET; -1: BLUEBOX_DEBUGGER or log */

enum { MAX_PLACES = 128 };

typedef struct {
    char kind[16];
    uint32_t pc;
    uint32_t hash; /* of the message */
    uint64_t count;
} place;

static place places[MAX_PLACES];
static unsigned nplaces;
static uint64_t entries, unplaced;
static char last_message[256];

void machine_set_debugger(int m) { mode = m; }

static int debugger_mode(void)
{
    if (mode < 0) {
        const char *e = getenv("BLUEBOX_DEBUGGER");
        mode = e && !strcmp(e, "halt") ? DEBUGGER_HALT : e && !strcmp(e, "quiet") ? DEBUGGER_QUIET : DEBUGGER_LOG;
    }
    return mode;
}

void debugger_reset(void)
{
    nplaces = 0;
    entries = unplaced = 0;
    last_message[0] = 0;
}

uint64_t machine_debugger_entries(void) { return entries; }
unsigned machine_debugger_places(void) { return nplaces + (unplaced ? 1 : 0); }
const char *machine_debugger_last(void) { return last_message; }

static uint32_t fnv(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

/* A routine's MacsBug name: the symbol a compiler put after the routine's
 * RTS, RTD or JMP (A0), found by looking on from the PC (the forms of
 * MacsBug's manual, "Procedure names": $80+n then n characters, $80 then
 * a length byte, or the older 8 characters with the first's top bit set). */
static bool symbol_char(uint8_t ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' ||
           ch == '%' || ch == '.' || ch == ' ';
}

static bool symbol_at(gaddr a, char *out, size_t n)
{
    uint8_t b = rd8(a);
    unsigned len, start;
    if (b > 0x80 && b < 0xA0) {
        len = b - 0x80u;
        start = 1;
    } else if (b == 0x80) {
        len = rd8(a + 1);
        start = 2;
    } else if (b & 0x80 && symbol_char(b & 0x7F)) {
        len = 8;
        start = 0;
    } else {
        return false;
    }
    if (len < 2 || len + 1 > n)
        return false;
    for (unsigned i = 0; i < len; i++) {
        uint8_t ch = rd8(a + start + i);
        if (start == 0)
            ch &= 0x7F;
        if (!symbol_char(ch))
            return false;
        out[i] = (char)ch;
    }
    while (len && out[len - 1] == ' ')
        len--;
    out[len] = 0;
    return len > 0;
}

static bool routine_name(gaddr pc, char *out, size_t n)
{
    pc = mem_strip(pc) & ~1u;
    if (pc < 0x1000 || pc > 0xFFF00000u)
        return false;
    for (gaddr a = pc; a < pc + 0x8000; a += 2) {
        uint16_t w = rd16(a);
        if (w == 0x4E75 || w == 0x4ED0)
            return symbol_at(a + 2, out, n);
        if (w == 0x4E74)
            return symbol_at(a + 4, out, n);
    }
    return false;
}

/* One entry: logged (throttled), counted, or the run stopped.  Returns
 * true if the run is to stop. */
static bool enter(const char *kind, const char *msg, uint32_t pc, uint32_t caller, bool ppc)
{
    char clean[256];
    if (msg) {
        /* A message's returns and other control characters as spaces. */
        size_t i = 0;
        for (; msg[i] && i < sizeof clean - 1; i++)
            clean[i] = (uint8_t)msg[i] < 0x20 || msg[i] == 0x7F ? ' ' : msg[i];
        clean[i] = 0;
        msg = clean;
    }
    entries++;
    snprintf(last_message, sizeof last_message, "%s%s%s", kind, msg ? ": " : "", msg ? msg : "");
    int m = debugger_mode();
    if (m == DEBUGGER_HALT)
        return true;
    uint32_t h = fnv(msg ? msg : "");
    place *p = NULL;
    for (unsigned i = 0; i < nplaces && !p; i++)
        if (places[i].pc == pc && places[i].hash == h && !strcmp(places[i].kind, kind))
            p = &places[i];
    if (!p && nplaces < MAX_PLACES) {
        p = &places[nplaces++];
        snprintf(p->kind, sizeof p->kind, "%s", kind);
        p->pc = pc;
        p->hash = h;
        p->count = 0;
    }
    uint64_t count;
    if (p) {
        count = ++p->count;
    } else {
        count = ++unplaced; /* past the table: these are counted together */
        pc = 0;
    }
    if (m == DEBUGGER_QUIET)
        return false;
    bool power = count == 1;
    for (uint64_t t = 10; t <= count && !power; t *= 10)
        power = count == t;
    if (!power)
        return false;
    char name[64] = "";
    if (!ppc && pc && routine_name(pc, name + 4, sizeof name - 4))
        memcpy(name, " in ", 4);
    char at[16] = "", from[32] = "";
    if (pc)
        snprintf(at, sizeof at, " at %08X", pc);
    if (caller)
        snprintf(from, sizeof from, "%s %08X", pc ? ", from" : " from", caller);
    char times[40] = "";
    if (count > 1)
        snprintf(times, sizeof times, " (%llu times)", (unsigned long long)count);
    LOGF(LOG_DEBUGGER, "debugger: %s%s%s%s%s%s%s%s, went on\n", kind, msg ? " \"" : "", msg ? msg : "",
         msg ? "\"" : "", at, name, from, times);
    return false;
}

/* The 68k caller: the return address in the frame A6 points at, if it
 * looks like one (the routine that entered the debugger linked A6). */
static uint32_t caller_68k(cpu68k *c)
{
    gaddr a6 = mem_strip(c->a[6]);
    gaddr sp = mem_strip(c->a[7]);
    if (a6 & 1 || a6 < sp || a6 - sp > 0x100000u)
        return 0;
    uint32_t ret = mem_strip(rd32(a6 + 4));
    return ret & 1 || ret < 0x1000 ? 0 : ret;
}

static bool enter_68k(const char *kind, const char *msg)
{
    cpu68k *c = g_cpu;
    if (!enter(kind, msg, c->op_pc, caller_68k(c), false))
        return false;
    machine_dump_68k_trace();
    if (msg)
        cpu_halt(c, "%s: %s", kind, msg);
    else
        cpu_halt(c, "%s() called", kind);
    return true;
}

/* ---- The 68k traps ---- */

void tb_Debugger(void) { enter_68k("Debugger", NULL); }

void tb_DebugStr(gaddr p)
{
    char s[256];
    pstr_to_c(p, s, sizeof s);
    enter_68k("DebugStr", s);
}

/* SysBreak, SysBreakStr and SysBreakFunc are SysError -490, -491 and -492
 * (Traps.h: MOVE.W #code,D0; _SysError, the string or the DebuggerUPP
 * pushed for the last two as a Pascal routine's argument).  MacsBug 6.5.3
 * takes the first for Debugger and the other two for DebugStr ($3582: it
 * sets up an $A9FF for -490 and an $ABFF for -491 and -492), so the
 * argument is popped; SysBreakFunc's routine is logged by its address. */
bool debugger_sys_break(cpu68k *c, int16_t code)
{
    if (code == -490)
        return !enter_68k("SysBreak", NULL);
    if (code != -491 && code != -492)
        return false;
    /* Above the return address the trap's dispatch pushed. */
    char s[256];
    if (code == -491)
        pstr_to_c(rd32(c->a[7] + 4), s, sizeof s);
    else
        snprintf(s, sizeof s, "routine %08X", rd32(c->a[7] + 4));
    if (enter_68k(code == -491 ? "SysBreakStr" : "SysBreakFunc", s))
        return false;
    uint32_t ret = cpu_pop32(c);
    c->a[7] += 4;
    cpu_push32(c, ret);
    return true;
}

/* MacJmp's entry, for a program that calls the debugger through it: as
 * MacsBug resumed, the RTS after this native returns to the caller. */
static void macjmp_entry(cpu68k *c, uint16_t tw)
{
    (void)tw;
    if (!enter("MacJmp", NULL, 0, mem_strip(rd32(c->a[7])), false))
        return;
    machine_dump_68k_trace();
    cpu_halt(c, "debugger entered through MacJmp");
}

/* ---- PowerPC: InterfaceLib's Debugger, DebugStr and debugstr (a C
 * string), the SysBreak calls, DriverServicesLib's SysDebug and
 * SysDebugStr and Debugger68k and DebugStr68k ---- */

static void enter_ppc(ppc_cpu *c, const char *kind, const char *msg)
{
    /* The call's site, and the caller's caller from the back chain
     * (where the caller saved its LR at 8 in the frame below it). */
    uint32_t back = rd32(c->gpr[1]);
    uint32_t caller = back && !(back & 3) ? rd32(back + 8) : 0;
    if (enter(kind, msg, c->lr - 4, caller, true)) {
        if (msg)
            ppc_halt(c, "%s: %s", kind, msg);
        else
            ppc_halt(c, "%s() called", kind);
        return;
    }
    c->pc = c->lr;
}

void debugger_ppc(ppc_cpu *c, const char *kind, int arg)
{
    char s[256];
    if (arg == DEBUGGER_ARG_PSTR)
        pstr_to_c(c->gpr[3], s, sizeof s);
    else if (arg == DEBUGGER_ARG_ADDR)
        snprintf(s, sizeof s, "routine %08X", c->gpr[3]);
    else if (arg == DEBUGGER_ARG_CSTR) {
        size_t i = 0;
        for (gaddr p = c->gpr[3]; p && i < sizeof s - 1 && rd8(p); p++)
            s[i++] = (char)rd8(p);
        s[i] = 0;
    }
    enter_ppc(c, kind, arg == DEBUGGER_ARG_NONE ? NULL : s);
}

/* ---- Low memory ---- */

/* MacJmp ($120) and MacJmpFlag ($BFF) as MacsBug left them installed.
 * MacsBug 6.5.3's install ($59E4 in its data fork; ProtoMacsBug's the
 * same): MacJmp its entry, then if MacJmpFlag reads $FF (a ROM before
 * MacJmpFlag: the flags are in MacJmp's top byte, as the Mac Plus ROM's
 * own debugger nub kept them) the flags in $120, else in $BFF: bits 6
 * (installed), 5 (initialised), 4 and 2 set, $74; bit 7 set while it
 * runs.  System 7.1's debugger code tests $BFF for $FF the same way.
 * The Quadra 800/900/950 and Power Mac ROMs clear $BFF at boot and enter
 * the debugger only when MacJmpFlag doubled is positive and nonzero
 * (MOVE.B $BFF,D3; ADD.B D3,D3; BLE: installed and not busy).  So a
 * Quadra or Power Mac, in either addressing mode, has $74 in $BFF and the
 * entry alone in MacJmp; a 68000 Mac or a Mac II (ROMs $75, $76, $78) has
 * $FF in $BFF and $74 in MacJmp's top byte.  The entry is a JMP in the
 * system heap to our own native: AMP 68K 2.9a2, Allostris 2.0 and
 * Megalomania read their nil objects' fields from around $120, and need
 * the nonzero bytes of a system heap address there, as MacsBug's was. */
void debugger_install(void)
{
    debugger_reset();
    wr32(0x0120, 0);
    wr8(0x0BFF, 0);
    gaddr dbg = mm_new_ptr_in(rd32(LM_SysZone), 6, false);
    if (!dbg)
        return;
    wr16(dbg, 0x4EF9); /* JMP */
    wr32(dbg + 2, native_register(macjmp_entry, "MacsBug", 0));
    bool old_rom = machine_cpu_model() == 68000 || (hw_machine()->rom_version & 0xFF) < 0x7C;
    if (old_rom) {
        wr8(0x0BFF, 0xFF);
        wr32(0x0120, (uint32_t)DEBUGGER_FLAGS << 24 | dbg);
    } else {
        wr32(0x0120, dbg);
        wr8(0x0BFF, DEBUGGER_FLAGS);
    }
}

/* The run's summary line (empty when the debugger was never entered). */
void machine_debugger_summary(char *out, size_t n)
{
    if (!entries) {
        if (n)
            out[0] = 0;
        return;
    }
    if (debugger_mode() == DEBUGGER_HALT) {
        snprintf(out, n, "debugger entered, halted (%s)", last_message);
        return;
    }
    snprintf(out, n, "debugger entered %llu time%s at %u place%s, went on (last: %s)", (unsigned long long)entries,
             entries == 1 ? "" : "s", machine_debugger_places(), machine_debugger_places() == 1 ? "" : "s",
             last_message);
}
