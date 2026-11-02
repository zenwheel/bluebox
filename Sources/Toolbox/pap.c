/*
 * PAP, the Printer Access Protocol: print jobs for a LaserWriter on
 * AppleTalk.  Not yet: the job is logged and not printed.
 */
#include "chooser.h"
#include "tb_internal.h"

bool pap_print(const machine_print_job *job, const char *nbp)
{
    LOGF(LOG_UNIMPL, "PAP: %d page(s) for %s not printed\n", job->npages, nbp);
    return false;
}
