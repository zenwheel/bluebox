/*
 * ThreadsLib's remaining exports, on the Thread Manager of threadmgr.c.
 * Mac OS 9's were for its debugger and its fragment manager; their
 * argument lists here are Blue Box's:
 *
 *   SuspendThread(id), ResumeThread(id), TerminateThread(id)
 *   GetThreadInformation(id, *info): 8 longs: size 32, ID, state, style,
 *     stack size, stack base, entry routine, its parameter
 *   GetThreadScheduler(*scheduler): the one SetThreadScheduler gave
 *   GetThreadsInContext(taskRef, max, *count, IDs[]): every thread of the
 *     task (the application's is the only one)
 *   GetThreadTaskRefGivenProcID(psn, *taskRef)
 *   ReadWriteThreadRegisters(id, write, registers): 32 GPRs, LR, CTR, CR,
 *     XER, PC as 37 longs
 *   SetDebugProcsGivenTaskRef(taskRef, new, dispose, schedule)
 *   CreateThreadPoolExtended(style, count, stackSize, options)
 *   CleanupThreadsPowerPC(): the program's threads are all disposed of
 */
#include "ppc_bridge.h"
#include "tb_internal.h"
#include "os9left.h"

enum { threadNotFoundErr = -618, threadProtocolErr = -619, kStoppedThreadState = 1, kNoThreadID = 0 };

gaddr thread_scheduler(void);
gaddr thread_debug_proc(int which);
int16_t thread_info(int32_t id, gaddr out);
unsigned thread_ids(uint32_t *ids, unsigned max);
int16_t thread_registers(int32_t id, bool write, gaddr rec);
void thread_cleanup(void);

static void th_SuspendThread(ppc_cpu *c) { RETURN(tb_SetThreadState((int32_t)R(0), kStoppedThreadState, kNoThreadID)); }

/* ResumeThread(id): a stopped thread is ready; one that isn't stopped is
 * a protocol error. */
static void th_ResumeThread(ppc_cpu *c)
{
    gaddr st = tb_NewPtr(2, false, true);
    if (!st)
        RETURN(memFullErr);
    int16_t e = tb_GetThreadState((int32_t)R(0), st);
    uint16_t state = rd16(st);
    tb_DisposePtr(st);
    if (e)
        RETURN(e);
    if (state != kStoppedThreadState)
        RETURN(threadProtocolErr);
    RETURN(tb_SetThreadReadyGivenTaskRef(1, (int32_t)R(0)));
}

static void th_TerminateThread(ppc_cpu *c) { RETURN(tb_DisposeThread((int32_t)R(0), 0, 0)); }

static void th_GetThreadInformation(ppc_cpu *c)
{
    if (!R(1))
        RETURN(paramErr);
    RETURN(thread_info((int32_t)R(0), R(1)));
}

static void th_GetThreadScheduler(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    wr32(R(0), thread_scheduler());
    RETURN(noErr);
}

static void th_GetThreadsInContext(ppc_cpu *c)
{
    if ((R(0) != 0 && R(0) != 1) || !R(2))
        RETURN(paramErr);
    uint32_t ids[64];
    unsigned n = thread_ids(ids, 64);
    unsigned put = 0;
    for (; put < n && put < R(1) && R(3); put++)
        wr32(R(3) + 4 * put, ids[put]);
    wr32(R(2), R(3) ? put : n);
    RETURN(noErr);
}

static void th_GetThreadTaskRefGivenProcID(ppc_cpu *c)
{
    if (!R(1))
        RETURN(paramErr);
    RETURN(tb_GetThreadCurrentTaskRef(R(1)));
}

static void th_ReadWriteThreadRegisters(ppc_cpu *c)
{
    if (!R(2))
        RETURN(paramErr);
    RETURN(thread_registers((int32_t)R(0), (R(1) & 0xFF) != 0, R(2)));
}

static void th_SetDebugProcsGivenTaskRef(ppc_cpu *c)
{
    if (R(0) != 1)
        RETURN(paramErr);
    RETURN(tb_SetDebuggerNotificationProcs(R(1), R(2), R(3)));
}

static void th_CreateThreadPoolExtended(ppc_cpu *c) { RETURN(tb_CreateThreadPool((int32_t)R(0), (int16_t)R(1), (int32_t)R(2))); }

static void th_CleanupThreadsPowerPC(ppc_cpu *c)
{
    thread_cleanup();
    RETURN(noErr);
}

void os9left_threads_reset(void) {}

const ppc_export os9left_threads_exports[] = {
    { "CleanupThreadsPowerPC", th_CleanupThreadsPowerPC },
    { "CreateThreadPoolExtended", th_CreateThreadPoolExtended },
    { "GetThreadInformation", th_GetThreadInformation },
    { "GetThreadScheduler", th_GetThreadScheduler },
    { "GetThreadTaskRefGivenProcID", th_GetThreadTaskRefGivenProcID },
    { "GetThreadsInContext", th_GetThreadsInContext },
    { "ReadWriteThreadRegisters", th_ReadWriteThreadRegisters },
    { "ResumeThread", th_ResumeThread },
    { "SetDebugProcsGivenTaskRef", th_SetDebugProcsGivenTaskRef },
    { "SuspendThread", th_SuspendThread },
    { "TerminateThread", th_TerminateThread },
};
const unsigned os9left_threads_export_count = sizeof os9left_threads_exports / sizeof os9left_threads_exports[0];
