/*
 * The Finder's own Process Manager calls (M21).  System 7.1's Finder runs
 * as the system's Finder process (bluebox run on its 'FNDR' file); the
 * private OSDispatch selectors it uses are found from its code
 * (Generators/defs/finder.yaml) and carried out on Blue Box's own records.
 */
#include <string.h>
#include "tb_internal.h"

/* ---- Volume notification ----
 * The Finder asks to be told when volumes come and go: its routine (a
 * Pascal function of the notice) and reference constant.  The volumes
 * that come and go in a run are the ones it ejects or puts away itself,
 * and the host's disks mounted while it runs. */

#define MAX_VN 8
static struct {
    gaddr proc;
    int32_t refcon;
} vn[MAX_VN];

int16_t tb_RequestVolumeNotification(gaddr proc, int32_t refcon)
{
    for (int i = 0; i < MAX_VN; i++)
        if (!vn[i].proc) {
            vn[i].proc = proc;
            vn[i].refcon = refcon;
            return noErr;
        }
    return -108; /* memFullErr */
}

int16_t tb_DeclineVolumeNotification(gaddr proc, int32_t refcon)
{
    for (int i = 0; i < MAX_VN; i++)
        if (vn[i].proc == proc && vn[i].refcon == refcon) {
            vn[i].proc = 0;
            return noErr;
        }
    return paramErr;
}

/* ---- The desktop ---- */

/* The desktop window (wm.c): outside the window list, so FindWindow still
 * answers inDesk on the desktop, but with the uncovered desktop as its
 * visible region and the Finder's update events for what is exposed. */
int16_t tb_NewDesktopWindow(gaddr out)
{
    gaddr w = wm_new_desk_window();
    if (out)
        wr32(out, w);
    return w ? noErr : -108;
}

/* ---- The system heap ---- */

/* What is free in the system heap now (FreeMem counts the room it can
 * still grow into, so nothing more is reported as growth). */
int16_t tb_SystemHeapRoom(gaddr grow_room, gaddr free_room)
{
    if (free_room)
        wr32(free_room, (uint32_t)tb_FreeMem(true));
    if (grow_room)
        wr32(grow_room, 0);
    return noErr;
}

/* ExpandMem: the record of system globals added after the low-memory
 * area was full, pointed to by $2B6.  Its version word and size come
 * first; the Finder reads the system heap's reserve at $1AE (the free
 * space it leaves in the system heap whatever it allocates there). */
#define LM_EXPAND_MEM 0x02B6
#define EXPANDMEM_SIZE 0x300
#define EM_SYS_HEAP_RESERVE 0x1AE

void finder_reset(void)
{
    memset(vn, 0, sizeof vn);
    gaddr em = rd32(LM_EXPAND_MEM);
    if (!em || (uint32_t)tb_GetPtrSize(em) < EXPANDMEM_SIZE) {
        gaddr grown = mm_new_ptr_in(rd32(LM_SysZone), EXPANDMEM_SIZE, true);
        if (!grown)
            return;
        if (em)
            gmemcpy(grown, em, (uint32_t)tb_GetPtrSize(em));
        else
            wr16(grown, 0x0107); /* emVersion, as System 7.1's */
        wr32(grown + 2, EXPANDMEM_SIZE);
        wr32(LM_EXPAND_MEM, grown);
        em = grown;
    }
    if (!rd32(em + EM_SYS_HEAP_RESERVE))
        wr32(em + EM_SYS_HEAP_RESERVE, 0x4000);
}
