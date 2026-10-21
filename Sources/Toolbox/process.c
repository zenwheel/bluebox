/*
 * Process Manager: one process per host process, so the app is the only
 * (and front) process.  Temporary memory comes from the growable
 * application heap.
 */
#include <stdlib.h>
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
    ProcessInfoRec_set_processMode(info, g_app_size_flags); /* the SIZE flags, as the Process Manager gave */
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

/* Our high-level event port, named as the Process Manager names them: the
 * application's name, its creator and the type 'ep01'. */
int16_t tb_GetPortNameFromProcessSerialNumber(gaddr port, gaddr psn)
{
    if (!is_us(psn))
        return procNotFound;
    gmemset(port, 0, PPCPortRec_SIZEOF);
    uint32_t len = rd8(LM_CurApName);
    gmemcpy(port + PPCPortRec_name_off, LM_CurApName, (len > 31 ? 31 : len) + 1);
    if (len > 31)
        wr8(port + PPCPortRec_name_off, 31);
    PPCPortRec_set_portKindsSelector(port, 1); /* ppcByCreatorAndType */
    wr32(port + PPCPortRec_u_port_creator_off, g_app_creator);
    wr32(port + PPCPortRec_u_port_type_off, fourcc("ep01"));
    return noErr;
}

/* MultiFinder's calls, kept by the Process Manager for Microsoft's programs.
 * GetSwitchInfo gives the system process's signature ('TWIT') and version;
 * GetActiveTaskInfo lists the applications (ours) as TaskInfoRecs
 * (ProcessID word, signature, the 'VERS' 0 version long; 10 bytes). */
void tb_GetSwitchInfo(gaddr info)
{
    wr32(info, fourcc("TWIT"));
    wr32(info + 4, rd16(LM_SysVersion));
}

int16_t tb_GetActiveTaskInfo(gaddr info, int16_t slots)
{
    if (slots > 0 && info) {
        gaddr vers = tb_GetResource((int32_t)fourcc("VERS"), 0);
        wr16(info, 1);
        wr32(info + 2, g_app_creator);
        wr32(info + 6, vers && rd32(vers) ? rd32(rd32(vers)) : 0);
    }
    return 1;
}

void tb_CancelSwitch(void) {}
int16_t tb_BeginSystemMode(void) { return 0; }
int16_t tb_EndSystemMode(void) { return 0; }

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

int aem_launch_documents(gaddr params, char ***paths_out);
static machine_launch_fn launch_fn;
static void *launch_ctx;

void machine_set_launch_handler(machine_launch_fn fn, void *ctx)
{
    launch_fn = fn;
    launch_ctx = ctx;
}

bool process_launch(const char *app, const char *const *docs, int ndocs)
{
    return launch_fn && launch_fn(app, docs, ndocs, launch_ctx);
}

int16_t tb_LaunchApplication(gaddr pb)
{
    char path[1024];
    if (rd16(pb + LaunchParamBlockRec_launchBlockID_off) != 0x4C43) { /* not extendedBlock 'LC' */
        /* The old _Launch: a file name (in the default directory) and a
         * configuration word; the caller is replaced by any file with code,
         * whatever its type (Dark Castle's application launches "Data A"). */
        gaddr name = rd32(pb);
        if (!name || !fm_host_path(0, 0, name, true, path, sizeof path))
            return fnfErr;
        if (!process_launch(path, NULL, 0))
            return -600;
        rm_update_all();
        cpu_halt(g_cpu, "ExitToShell (launched %s)", path);
        return noErr;
    }
    gaddr spec = rd32(pb + LaunchParamBlockRec_launchAppSpec_off);
    uint16_t flags = rd16(pb + LaunchParamBlockRec_launchControlFlags_off);
    if (!spec || !fm_host_path((int16_t)rd16(spec), (int32_t)rd32(spec + 2), spec + 6, true, path, sizeof path))
        return fnfErr;
    /* Documents to open come as an Open Documents event in the parameters. */
    char **docs = NULL;
    int ndocs = aem_launch_documents(LaunchParamBlockRec_launchAppParameters(pb), &docs);
    bool ok = process_launch(path, (const char *const *)docs, ndocs);
    for (int i = 0; i < ndocs; i++)
        free(docs[i]);
    free(docs);
    if (!ok)
        return -600; /* procNotFound: nothing could be started */
    gmemset(pb + LaunchParamBlockRec_launchProcessSN_off, 0, 8);
    if (!(flags & 0x4000)) { /* launchContinue */
        rm_update_all();
        cpu_halt(g_cpu, "ExitToShell (launched %s)", path);
    }
    return noErr;
}

/* The PPC Toolbox ($A0DD, selector in D0): it starts, but no port opens, as
 * on a Mac with nothing to talk to (Word 5.0 opens one at startup and goes
 * on without it).  The result is in D0 and the parameter block's ioResult. */
enum { noPortErr = -903 };

void tb_PPCDispatch(int32_t d0, int32_t a0)
{
    int16_t err = d0 == 0 /* PPCInit */ ? noErr : noPortErr;
    if (d0 != 0 && a0)
        wr16((gaddr)a0 + 16, (uint16_t)err);
    g_cpu->d[0] = (uint32_t)(int32_t)err;
}
