/*
 * Process Manager: one process per host process, so the app is the front
 * process.  The Finder follows it in the process list (programs find it by
 * its type and creator to send it events, which aem.c carries out), then
 * the other runs that take Apple Events, as serial numbers {1, their
 * process ID}, so programs can find each other by signature and send
 * events.  When the run is the Finder itself (bluebox run on a 'FNDR'
 * file), it is that Finder process, first in the list.  Temporary memory
 * comes from the growable application heap.
 */
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include "machine.h"
#include "tb_internal.h"

enum { procNotFound = -600 };
#define OUR_PSN 0x1000u
#define FINDER_PSN 0x1001u
#define kCurrentProcess 2u

static uint32_t our_psn(void) { return g_is_finder ? FINDER_PSN : OUR_PSN; }

/* The Finder's serial number; when the run is the Finder, that is us. */
bool process_is_finder(uint32_t hi, uint32_t lo) { return hi == 0 && lo == FINDER_PSN; }
static bool is_finder(gaddr psn) { return !g_is_finder && process_is_finder(rd32(psn), rd32(psn + 4)); }

bool process_is_us(uint32_t hi, uint32_t lo) { return hi == 0 && (lo == our_psn() || lo == kCurrentProcess); }
static bool is_us(gaddr psn) { return process_is_us(rd32(psn), rd32(psn + 4)); }

static void put_us(gaddr psn)
{
    wr32(psn, 0);
    wr32(psn + 4, our_psn());
}

int16_t tb_GetCurrentProcess(gaddr psn)
{
    put_us(psn);
    return noErr;
}

/* Another run, by its serial number: its listing entry. */
static bool other_run(gaddr psn, machine_ae_process *found)
{
    if (rd32(psn) != 1)
        return false;
    machine_ae_process procs[64];
    int n = machine_ae_processes(procs, 64);
    for (int i = 0; i < n; i++)
        if ((uint32_t)procs[i].pid == rd32(psn + 4)) {
            *found = procs[i];
            return true;
        }
    return false;
}

int16_t tb_GetNextProcess(gaddr psn)
{
    if (rd32(psn) == 0 && rd32(psn + 4) == 0) { /* kNoProcess: the first one */
        put_us(psn);
        return noErr;
    }
    if (is_us(psn) && !g_is_finder) { /* then the Finder */
        wr32(psn, 0);
        wr32(psn + 4, FINDER_PSN);
        return noErr;
    }
    /* Then the other runs, in process ID order. */
    uint32_t after = is_finder(psn) || is_us(psn) ? 0 : rd32(psn) == 1 ? rd32(psn + 4) : UINT32_MAX;
    machine_ae_process procs[64];
    int n = machine_ae_processes(procs, 64);
    uint32_t next = UINT32_MAX;
    for (int i = 0; i < n; i++)
        if (procs[i].pid != getpid() && (uint32_t)procs[i].pid > after && (uint32_t)procs[i].pid < next)
            next = (uint32_t)procs[i].pid;
    if (next != UINT32_MAX) {
        wr32(psn, 1);
        wr32(psn + 4, next);
        return noErr;
    }
    wr32(psn, 0);
    wr32(psn + 4, 0);
    return procNotFound;
}

uint16_t g_app_size_flags;

extern char g_app_path[];
extern uint32_t g_app_creator;

/* The partition reported for the Finder and other runs, whose memory is
 * not ours to see: about a System 7.5 Finder's. */
#define OTHER_PARTITION 0x100000u

int16_t tb_GetProcessInformation(gaddr psn, gaddr info)
{
    machine_ae_process other;
    if (other_run(psn, &other)) {
        /* What the listing says: its signature (its name stays empty). */
        gaddr name = ProcessInfoRec_processName(info);
        if (name)
            wr8(name, 0);
        gmemcpy(info + ProcessInfoRec_processNumber_off, psn, 8);
        ProcessInfoRec_set_processType(info, fourcc("APPL"));
        ProcessInfoRec_set_processSignature(info, (int32_t)other.signature);
        ProcessInfoRec_set_processMode(info, 0x0040); /* takes high-level events */
        ProcessInfoRec_set_processLocation(info, 0);
        /* A partition of its own, as a Finder's or an application's:
         * memory monitors divide by it (Monitor showed each one's use). */
        ProcessInfoRec_set_processSize(info, OTHER_PARTITION);
        ProcessInfoRec_set_processFreeMem(info, OTHER_PARTITION / 4);
        wr32(info + ProcessInfoRec_processLauncher_off, 0);
        wr32(info + ProcessInfoRec_processLauncher_off + 4, 0);
        ProcessInfoRec_set_processLaunchDate(info, 0);
        ProcessInfoRec_set_processActiveTime(info, 0);
        gaddr spec = ProcessInfoRec_processAppSpec(info);
        if (spec)
            gmemset(spec, 0, FSSpec_SIZEOF);
        return noErr;
    }
    if (is_finder(psn)) {
        gaddr name = ProcessInfoRec_processName(info);
        if (name)
            gput(name, "\006Finder", 7);
        gmemcpy(info + ProcessInfoRec_processNumber_off, psn, 8);
        ProcessInfoRec_set_processType(info, fourcc("FNDR"));
        ProcessInfoRec_set_processSignature(info, fourcc("MACS"));
        ProcessInfoRec_set_processMode(info, 0x0040); /* takes high-level events */
        ProcessInfoRec_set_processLocation(info, 0);
        ProcessInfoRec_set_processSize(info, OTHER_PARTITION);
        ProcessInfoRec_set_processFreeMem(info, OTHER_PARTITION / 4);
        wr32(info + ProcessInfoRec_processLauncher_off, 0);
        wr32(info + ProcessInfoRec_processLauncher_off + 4, 0);
        ProcessInfoRec_set_processLaunchDate(info, 0);
        ProcessInfoRec_set_processActiveTime(info, 0);
        gaddr spec = ProcessInfoRec_processAppSpec(info);
        if (spec)
            gmemset(spec, 0, FSSpec_SIZEOF);
        return noErr;
    }
    if (!is_us(psn))
        return procNotFound;
    gaddr name = ProcessInfoRec_processName(info);
    if (name)
        gmemcpy(name, LM_CurApName, (uint32_t)rd8(LM_CurApName) + 1);
    put_us(info + ProcessInfoRec_processNumber_off);
    ProcessInfoRec_set_processType(info, fourcc(g_is_finder ? "FNDR" : "APPL"));
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

void hfs_make_fsref(gaddr ref, const char *path); /* hfsplus.c */

/* GetProcessBundleLocation (CarbonLib): the FSRef of the process's
 * application file.  The Finder and the other runs have none here. */
int16_t process_bundle_location(gaddr psn, gaddr ref)
{
    machine_ae_process other;
    if (!psn || !ref)
        return paramErr;
    if (is_us(psn)) {
        char full[PATH_MAX];
        if (!g_app_path[0] || !realpath(g_app_path, full)) /* by the path the File Manager knows it by */
            return fnfErr;
        hfs_make_fsref(ref, full);
        return noErr;
    }
    return is_finder(psn) || other_run(psn, &other) ? fnfErr : procNotFound;
}

/* A process's name as GetProcessInformation gives it (a Pascal string,
 * Mac Roman, into name[256]), for CopyProcessName (CarbonLib). */
int16_t process_name(gaddr psn, uint8_t *name)
{
    machine_ae_process other;
    name[0] = 0;
    if (!psn)
        return paramErr;
    if (is_us(psn)) {
        gget(name, LM_CurApName, (uint32_t)rd8(LM_CurApName) + 1);
        return noErr;
    }
    if (is_finder(psn)) {
        memcpy(name, "\006Finder", 7);
        return noErr;
    }
    return other_run(psn, &other) ? noErr : procNotFound;
}

int16_t tb_SameProcess(gaddr a, gaddr b, gaddr same_out)
{
    wr8(same_out, is_us(a) && is_us(b) ? 1 : (rd32(a) == rd32(b) && rd32(a + 4) == rd32(b + 4)));
    return noErr;
}

int16_t tb_SetFrontProcess(gaddr psn) { return is_us(psn) || is_finder(psn) ? noErr : procNotFound; }
void ev_wake_process(void);

/* Woken, this process's sleep in WaitNextEvent ends; the Finder of
 * another run is only a name here. */
int16_t tb_WakeUpProcess(gaddr psn)
{
    if (is_us(psn)) {
        ev_wake_process();
        return noErr;
    }
    return is_finder(psn) ? noErr : procNotFound;
}
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
    LOGF(LOG_MEM, "TempNewHandle(%d) -> %08X\n", logical_size, h);
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
 * Without launchContinue the caller ends, as the Process Manager did,
 * unless the caller is the Finder, which goes on beside it. */

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

static machine_url_fn url_fn;
static void *url_ctx;

void machine_set_url_handler(machine_url_fn fn, void *ctx)
{
    url_fn = fn;
    url_ctx = ctx;
}

bool process_open_url(const char *url) { return url_fn && url_fn(url, url_ctx); }

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
        if (g_is_finder)
            return noErr;
        rm_update_all();
        fm_flush_disks(); /* a program's disks written back through their drivers */
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
    /* The Finder keeps running whatever it asks: the program is its own
     * run beside it. */
    if (!(flags & 0x4000) && !g_is_finder) { /* launchContinue */
        rm_update_all();
        fm_flush_disks(); /* a program's disks written back through their drivers */
        cpu_halt(g_cpu, "ExitToShell (launched %s)", path);
    }
    return noErr;
}

