/*
 * OSDispatch selectors the archive walk found programs calling that no
 * header names (Generators/defs/vamosdispatch.yaml).  Blue Box does not
 * know what the Process Manager did for them; all but $2B answer as a call
 * with nothing to do: no error, nil, false.
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

int32_t tb_OSDispatch5A(int32_t a, int32_t b, int8_t c, int16_t d, int32_t e, int8_t f, int32_t g, int32_t h,
                        int32_t i)
{
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
    (void)f;
    (void)g;
    (void)h;
    (void)i;
    return 0;
}
