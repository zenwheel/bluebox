/*
 * PrivateInterfaceLib's names Mac OS 9 added that Blue Box can back: Mac OS
 * 9's own were calls into the ROM's private dispatchers, whose arguments
 * and results show in the glue around each call.  What is here is what
 * those and the names say plainly and Blue Box keeps the state for; the
 * rest of Mac OS 9's private names (the Code Fragment Manager's registry,
 * the virtual memory manager's, the TrueType scaler's, the file system's
 * queues) have no state here to act on.
 */
#include <string.h>

#include "m68k.h"
#include "ppc.h"
#include "ppc_bridge.h"
#include "tb_internal.h"
#include "filemgr_internal.h"

#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

enum { nsvErr = -35, wPrErr = -44, rfNumErr = -51, wrPermErr = -61 };

/* BlockClear(ptr, count): the bytes zeroed. */
static void ppc_BlockClear(ppc_cpu *c)
{
    gaddr p = c->gpr[3];
    uint32_t n = c->gpr[4];
    if (n)
        gmemset(p, 0, n);
    c->pc = c->lr;
}

/* Font2RealScript(fontID): the script a font belongs to, as Font2Script
 * says, whatever the script system's font override. */
extern int16_t tb_FontToScript(int16_t fontnum);
static void ppc_Font2RealScript(ppc_cpu *c) { RETURN((int32_t)tb_FontToScript((int16_t)c->gpr[3])); }

/* The Window Manager's layers: the root, and the front window of one. */
static void ppc_GetRootLayer(ppc_cpu *c)
{
    gaddr l = tb_GetCurLayer();
    for (gaddr p; l && (p = tb_GetParent(l)); l = p)
        ;
    RETURN(l);
}

static void ppc_FrontWindowIn(ppc_cpu *c) { RETURN(tb_GetSubWindows(c->gpr[3])); }

/* ActiveWindow(): the front window that is highlighted, if any. */
static void ppc_ActiveWindow(ppc_cpu *c)
{
    gaddr w = rd32(LM_WindowList);
    RETURN(w && rd8(w + WindowRecord_hilited_off) ? w : 0);
}

/* The Process Manager's switching: DisableSwitching nests, EnableSwitching
 * undoes one, and switching is on while none is outstanding. */
static int switch_disabled;

static void ppc_DisableSwitching(ppc_cpu *c)
{
    switch_disabled++;
    c->pc = c->lr;
}

static void ppc_EnableSwitching(ppc_cpu *c)
{
    if (switch_disabled)
        switch_disabled--;
    c->pc = c->lr;
}

static void ppc_GetSwitchingEnabled(ppc_cpu *c) { RETURN(switch_disabled == 0); }

/* The File System Manager's checks of a number: a volume reference, then
 * a file's.  A mounted volume is locked, as a CD is. */
static bool volume_of(int16_t vref, int16_t *drive)
{
    char root[1024];
    uint8_t name[32];
    int16_t got, driver;
    return fm_volume(0, vref, &got, root, sizeof root, name, drive, &driver) && (vref == 0 || got == vref);
}

static int16_t ut_check_vol(int16_t vref)
{
    int16_t got;
    return volume_of(vref, &got) ? noErr : nsvErr;
}

static void ppc_UTCheckVolRefNum(ppc_cpu *c) { RETURN((int32_t)ut_check_vol((int16_t)c->gpr[3])); }
/* Every volume is online while it is mounted. */
static void ppc_UTCheckVolOffline(ppc_cpu *c) { RETURN((int32_t)ut_check_vol((int16_t)c->gpr[3])); }

static void ppc_UTCheckVolModifiable(ppc_cpu *c)
{
    int16_t drive;
    /* Drive 1 is the main volume. */
    RETURN((int32_t)(!volume_of((int16_t)c->gpr[3], &drive) ? nsvErr : drive == 1 ? noErr : wPrErr));
}

static bool fork_of(int16_t rn, bool *write)
{
    bool rsrc;
    uint32_t pos, len;
    return fm_fork_state(rn, NULL, 0, &rsrc, write, &pos, &len);
}

static void ppc_UTCheckFileRefNum(ppc_cpu *c)
{
    bool w;
    RETURN((int32_t)(fork_of((int16_t)c->gpr[3], &w) ? noErr : rfNumErr));
}

static void ppc_UTCheckFileModifiable(ppc_cpu *c)
{
    bool w;
    RETURN((int32_t)(!fork_of((int16_t)c->gpr[3], &w) ? rfNumErr : w ? noErr : wrPermErr));
}

const ppc_export priv_os9_exports[] = {
    { "BlockClear", ppc_BlockClear },
    { "Font2RealScript", ppc_Font2RealScript },
    { "GetRootLayer", ppc_GetRootLayer },
    { "FrontWindowIn", ppc_FrontWindowIn },
    { "ActiveWindow", ppc_ActiveWindow },
    { "DisableSwitching", ppc_DisableSwitching },
    { "EnableSwitching", ppc_EnableSwitching },
    { "GetSwitchingEnabled", ppc_GetSwitchingEnabled },
    { "UTCheckVolRefNum", ppc_UTCheckVolRefNum },
    { "UTCheckVolOffline", ppc_UTCheckVolOffline },
    { "UTCheckVolModifiable", ppc_UTCheckVolModifiable },
    { "UTCheckFileRefNum", ppc_UTCheckFileRefNum },
    { "UTCheckFileModifiable", ppc_UTCheckFileModifiable },
};
const unsigned priv_os9_export_count = sizeof priv_os9_exports / sizeof priv_os9_exports[0];

/* A new run: switching on. */
void priv_os9_reset(void) { switch_disabled = 0; }
