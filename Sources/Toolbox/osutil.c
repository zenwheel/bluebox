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
    SysEnvRec_set_sysVRefNum(p, -1);
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
