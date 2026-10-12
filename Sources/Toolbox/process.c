/*
 * Process Manager: one process per host process, so the app is the only
 * (and front) process.  Temporary memory comes from the growable
 * application heap.
 */
#include "machine.h"
#include "tb_internal.h"

enum { procNotFound = -600 };
#define OUR_PSN 0x1000u
#define kCurrentProcess 2u

static bool is_us(gaddr psn)
{
    uint32_t hi = rd32(psn), lo = rd32(psn + 4);
    return hi == 0 && (lo == OUR_PSN || lo == kCurrentProcess);
}

static void put_us(gaddr psn)
{
    wr32(psn, 0);
    wr32(psn + 4, OUR_PSN);
}

int16_t tb_GetCurrentProcess(gaddr psn)
{
    put_us(psn);
    return noErr;
}

int16_t tb_GetNextProcess(gaddr psn)
{
    if (rd32(psn) == 0 && rd32(psn + 4) == 0) { /* kNoProcess: the first one */
        put_us(psn);
        return noErr;
    }
    wr32(psn, 0);
    wr32(psn + 4, 0);
    return procNotFound;
}

uint16_t g_app_size_flags;

extern char g_app_path[];
extern uint32_t g_app_creator;

int16_t tb_GetProcessInformation(gaddr psn, gaddr info)
{
    if (!is_us(psn))
        return procNotFound;
    gaddr name = ProcessInfoRec_processName(info);
    if (name)
        gmemcpy(name, LM_CurApName, (uint32_t)rd8(LM_CurApName) + 1);
    put_us(info + ProcessInfoRec_processNumber_off);
    ProcessInfoRec_set_processType(info, fourcc("APPL"));
    ProcessInfoRec_set_processSignature(info, (int32_t)g_app_creator);
    ProcessInfoRec_set_processMode(info, 0x00000400); /* modeHighLevelEventAware-ish defaults */
    ProcessInfoRec_set_processLocation(info, rd32(LM_ApplZone));
    ProcessInfoRec_set_processSize(info, (uint32_t)tb_FreeMem(false));
    ProcessInfoRec_set_processFreeMem(info, (uint32_t)tb_FreeMem(false));
    wr32(info + ProcessInfoRec_processLauncher_off, 0);
    wr32(info + ProcessInfoRec_processLauncher_off + 4, 0);
    ProcessInfoRec_set_processLaunchDate(info, 0);
    ProcessInfoRec_set_processActiveTime(info, rd32(LM_Ticks));
    gaddr spec = ProcessInfoRec_processAppSpec(info);
    if (spec && g_app_path[0])
        fm_spec_for_path(g_app_path, spec);
    return noErr;
}

int16_t tb_SameProcess(gaddr a, gaddr b, gaddr same_out)
{
    wr8(same_out, is_us(a) && is_us(b) ? 1 : (rd32(a) == rd32(b) && rd32(a + 4) == rd32(b + 4)));
    return noErr;
}

int16_t tb_SetFrontProcess(gaddr psn) { return is_us(psn) ? noErr : procNotFound; }
int16_t tb_WakeUpProcess(gaddr psn) { return is_us(psn) ? noErr : procNotFound; }
int16_t tb_GetProcessSerialNumberFromPortName(gaddr port_name, gaddr psn) { return procNotFound; }

/* ---- Temporary memory ---- */

int32_t tb_TempFreeMem(void) { return tb_FreeMem(false); }

int32_t tb_TempMaxMem(gaddr grow)
{
    if (grow)
        wr32(grow, 0);
    return tb_MaxBlock(false);
}

gaddr tb_TempTopMem(void) { return rd32(LM_ApplLimit); }

gaddr tb_TempNewHandle(int32_t logical_size, gaddr result_code)
{
    gaddr h = mm_new_handle_in(rd32(LM_ApplZone), (uint32_t)(logical_size < 0 ? 0 : logical_size), false);
    if (result_code)
        wr16(result_code, rd16(LM_MemErr));
    return h;
}

void tb_TempHLock(gaddr h, gaddr result_code)
{
    tb_HLock(h);
    if (result_code)
        wr16(result_code, rd16(LM_MemErr));
}

void tb_TempHUnlock(gaddr h, gaddr result_code)
{
    tb_HUnlock(h);
    if (result_code)
        wr16(result_code, rd16(LM_MemErr));
}

void tb_TempDisposeHandle(gaddr h, gaddr result_code)
{
    tb_DisposeHandle(h);
    if (result_code)
        wr16(result_code, rd16(LM_MemErr));
}

int16_t tb_GetFrontProcess(gaddr psn, int32_t dummy)
{
    put_us(psn);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* LaunchApplication: the host starts the other application as its own run.
 * Without launchContinue the caller ends, as the Process Manager did. */

static machine_launch_fn launch_fn;
static void *launch_ctx;

void machine_set_launch_handler(machine_launch_fn fn, void *ctx)
{
    launch_fn = fn;
    launch_ctx = ctx;
}

int16_t tb_LaunchApplication(gaddr pb)
{
    gaddr spec = rd32(pb + LaunchParamBlockRec_launchAppSpec_off);
    uint16_t flags = rd16(pb + LaunchParamBlockRec_launchControlFlags_off);
    char path[1024];
    if (!spec || !fm_host_path((int16_t)rd16(spec), (int32_t)rd32(spec + 2), spec + 6, true, path, sizeof path))
        return fnfErr;
    if (!launch_fn || !launch_fn(path, launch_ctx))
        return -600; /* procNotFound: nothing could be started */
    gmemset(pb + LaunchParamBlockRec_launchProcessSN_off, 0, 8);
    if (!(flags & 0x4000)) { /* launchContinue */
        rm_update_all();
        cpu_halt(g_cpu, "ExitToShell (launched %s)", path);
    }
    return noErr;
}
