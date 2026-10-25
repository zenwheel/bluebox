/*
 * The old hardware and networking managers' state, back to its start for
 * a new machine (legacy.h).
 */
#include "legacy.h"

void legacy_reset(void)
{
    startmgr_reset();
    scsi_reset();
    dbaccess_reset();
    xpp_reset();
    ppc_toolbox_reset();
    devices_reset();
}
