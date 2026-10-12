/*
 * Scrap Manager: the desk scrap, kept in memory as a list of
 * (type, length, data padded to even) entries described by the ScrapStuff
 * record in low memory.  The host shell syncs it with the host clipboard.
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "forks.h"
#include "machine.h"
#include "tb_internal.h"

enum { noScrapErr = -100, noTypeErr = -102 };

/* Host clipboard: text arriving from the host waits here until the
 * emulator thread next touches the scrap; text the app puts goes out. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char *incoming;
static machine_clipboard_fn clipboard_fn;
static void *clipboard_ctx;

void machine_set_clipboard_handler(machine_clipboard_fn fn, void *ctx)
{
    clipboard_fn = fn;
    clipboard_ctx = ctx;
}

void machine_set_clipboard_text(const char *utf8)
{
    pthread_mutex_lock(&lock);
    free(incoming);
    incoming = strdup(utf8 ? utf8 : "");
    pthread_mutex_unlock(&lock);
}

static void export_text(gaddr data, int32_t len)
{
    if (!clipboard_fn || len < 0)
        return;
    uint8_t *mac = malloc((size_t)len + 1);
    gget(mac, data, (uint32_t)len);
    char *utf8 = malloc((size_t)len * 3 + 1);
    size_t n = macroman_to_utf8(mac, (size_t)len, utf8, (size_t)len * 3 + 1);
    for (size_t i = 0; i < n; i++)
        if (utf8[i] == '\r')
            utf8[i] = '\n';
    clipboard_fn(utf8, clipboard_ctx);
    free(utf8);
    free(mac);
}

int32_t tb_ZeroScrap(void);
int32_t tb_PutScrap(int32_t len, int32_t rest, gaddr p);
static bool importing;

/* Bring in host text, if any arrived: it replaces the desk scrap. */
static void import_host(void)
{
    pthread_mutex_lock(&lock);
    char *text = incoming;
    incoming = NULL;
    pthread_mutex_unlock(&lock);
    if (!text)
        return;
    size_t n = strlen(text);
    uint8_t *mac = malloc(n + 1);
    size_t m = utf8_to_macroman(text, mac, n + 1);
    for (size_t i = 0; i < m; i++)
        if (mac[i] == '\n')
            mac[i] = '\r';
    gaddr p = tb_NewPtr((int32_t)(m ? m : 1), false, false);
    gput(p, mac, (uint32_t)m);
    importing = true;
    tb_ZeroScrap();
    tb_PutScrap((int32_t)m, (int32_t)fourcc("TEXT"), p);
    importing = false;
    tb_DisposePtr(p);
    free(mac);
    free(text);
}

void scrap_sync(void) { import_host(); }

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
    import_host();
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
    if (rest == (int32_t)fourcc("TEXT") && !importing)
        export_text(p, len);
    return noErr;
}

int32_t tb_GetScrap(gaddr h, int32_t rest, gaddr off)
{
    import_host();
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
