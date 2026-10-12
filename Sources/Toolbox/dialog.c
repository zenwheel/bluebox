/* Dialog Manager (initialisation only so far; dialogs and alerts follow). */
#include "tb_internal.h"

static gaddr resume_proc;

void tb_InitDialogs(gaddr rp)
{
    resume_proc = rp;
    wr16(LM_ANumber, 0);
    wr16(LM_ACount, (uint16_t)-1);
    wr16(LM_DlgFont, 0);
}
