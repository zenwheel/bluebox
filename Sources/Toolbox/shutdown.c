/*
 * The Shutdown Manager (_ShutDown, $A895): procedures programs install to
 * run at shut down or restart, and ShutDwnPower and ShutDwnStart, which
 * here end the run (M21: the Finder's Shut Down and Restart, and any
 * program's), after the procedures have run, stage by stage, and the
 * files are written out.
 */
#include "tb_internal.h"
#include "trap.h"

enum { sdOnPowerOff = 1, sdOnRestart = 2, sdOnUnmount = 4, sdOnDrivers = 8, sdOnBootVolUnmount = 16 };

#define MAX_SD 32
static struct {
    gaddr proc;
    int16_t flags;
} procs[MAX_SD];
static int nprocs;

void shutdown_reset(void) { nprocs = 0; }

void tb_ShutDwnInstall(gaddr proc, int16_t flags)
{
    if (!proc || nprocs >= MAX_SD)
        return;
    procs[nprocs].proc = proc;
    procs[nprocs].flags = flags;
    nprocs++;
}

void tb_ShutDwnRemove(gaddr proc)
{
    for (int i = 0; i < nprocs; i++)
        if (procs[i].proc == proc) {
            procs[i] = procs[--nprocs];
            return;
        }
}

/* Each procedure for a stage, with the stage in D0 as the Shutdown Manager
 * called them. */
static void run_stage(int16_t stage)
{
    for (int i = 0; i < nprocs; i++)
        if (procs[i].flags & stage)
            hook_call(procs[i].proc, (uint16_t)stage, 0, g_cpu->a[7]);
}

static void shut_down(bool restart)
{
    run_stage(sdOnDrivers);
    run_stage(sdOnUnmount);
    run_stage(sdOnBootVolUnmount);
    run_stage(restart ? sdOnRestart : sdOnPowerOff);
    rm_update_all();
    fm_flush_dirty();
    cpu_halt(g_cpu, restart ? "ShutDwnStart (restart)" : "ShutDwnPower (shut down)");
}

void tb_ShutDwnPower(void) { shut_down(false); }
void tb_ShutDwnStart(void) { shut_down(true); }
