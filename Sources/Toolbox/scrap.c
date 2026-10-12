/*
 * Scrap Manager: the desk scrap, kept in memory as a list of
 * (type, length, data padded to even) entries described by the ScrapStuff
 * record in low memory.  The host shell syncs it with the host clipboard.
 */
#include "tb_internal.h"

enum { noScrapErr = -100, noTypeErr = -102 };

static gaddr scrap_handle(void)
{
    gaddr h = rd32(LM_ScrapHandle);
    if (!h) {
        h = tb_NewHandle(0, true, false);
        wr32(LM_ScrapHandle, h);
        wr32(LM_ScrapSize, 0);
        wr16(LM_ScrapState, 1); /* in memory */
    }
    return h;
}

gaddr tb_InfoScrap(void)
{
    scrap_handle();
    return LM_ScrapSize;
}

int32_t tb_UnloadScrap(void) { return noErr; }
int32_t tb_LoadScrap(void) { return noErr; }

int32_t tb_ZeroScrap(void)
{
    gaddr h = scrap_handle();
    tb_SetHandleSize(h, 0);
    wr32(LM_ScrapSize, 0);
    wr16(LM_ScrapCount, (uint16_t)(rd16(LM_ScrapCount) + 1));
    return noErr;
}

int32_t tb_PutScrap(int32_t len, int32_t rest, gaddr p)
{
    if (len < 0)
        return paramErr;
    gaddr h = scrap_handle();
    int32_t size = tb_GetHandleSize(h);
    int32_t padded = (len + 1) & ~1;
    tb_SetHandleSize(h, size + 8 + padded);
    if (rd16(LM_MemErr))
        return memFullErr;
    gaddr q = rd32(h) + (uint32_t)size;
    wr32(q, (uint32_t)rest);
    wr32(q + 4, (uint32_t)len);
    gmemcpy(q + 8, p, (uint32_t)len);
    if (padded != len)
        wr8(q + 8 + (uint32_t)len, 0);
    wr32(LM_ScrapSize, (uint32_t)(size + 8 + padded));
    return noErr;
}

int32_t tb_GetScrap(gaddr h, int32_t rest, gaddr off)
{
    gaddr sh = scrap_handle();
    int32_t size = tb_GetHandleSize(sh);
    int32_t pos = 0;
    while (pos + 8 <= size) {
        gaddr q = rd32(sh) + (uint32_t)pos;
        uint32_t type = rd32(q);
        int32_t len = (int32_t)rd32(q + 4);
        if (type == (uint32_t)rest) {
            if (h) {
                tb_SetHandleSize(h, len);
                if (rd16(LM_MemErr))
                    return memFullErr;
                gmemcpy(rd32(h), rd32(sh) + (uint32_t)pos + 8, (uint32_t)len);
            }
            if (off)
                wr32(off, (uint32_t)(pos + 8));
            return len;
        }
        pos += 8 + ((len + 1) & ~1);
    }
    return noTypeErr;
}
