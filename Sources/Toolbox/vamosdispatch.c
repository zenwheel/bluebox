/*
 * OSDispatch selectors the archive walk found programs calling that no
 * header names (Generators/defs/vamosdispatch.yaml).  Blue Box does not
 * know what the Process Manager did for them; $2B and $5A are worked out
 * from their callers ($64 is FindWindow), the others answer as a call with nothing to do: no
 * error, nil, false.
 */
#include "tb_internal.h"

void tb_OSDispatch28(int32_t a, int32_t b, int16_t c)
{
    (void)a;
    (void)b;
    (void)c;
}

/* Virtual walks its windows with it: starting from nil it gets each
 * window in turn (it reads each one's refCon as its own record, so the
 * program's own windows), nil at the end. */
int32_t tb_OSDispatch2B(int32_t a, int8_t b)
{
    (void)b;
    return a ? (int32_t)rd32((gaddr)a + WindowRecord_nextWindow_off) : (int32_t)rd32(LM_WindowList);
}

int8_t tb_OSDispatch55(int32_t a)
{
    (void)a;
    return 0;
}

/* $5A makes a window.  NotePad Deluxe, which has no other way to make
 * its windows, stops with "the memory is out" (-108) when it answers nil,
 * and MACAST and MacAMP use it the same way.  What they push is NewWindow's
 * arguments without the storage (the bounds, an empty title, visible,
 * procID, behind -1, goAway, refCon), then a routine descriptor and a long
 * of 0.  The descriptor is left unused: the window has its procID's
 * definition. */
gaddr tb_OSDispatch5A(gaddr bounds, gaddr title, int8_t visible, int16_t procID, gaddr behind, int8_t goAway,
                      int32_t refCon, gaddr proc, int32_t flags)
{
    (void)proc;
    (void)flags;
    return tb_NewWindow(0, bounds, title, visible, procID, behind, goAway, refCon);
}

/* $64 takes FindWindow's arguments and gives its result: MACAST and MacAMP
 * pass the point and the place for the window, and take a part code. */
int16_t tb_OSDispatch64(uint32_t thePoint, gaddr theWindow)
{
    return tb_FindWindow(thePoint, theWindow);
}
