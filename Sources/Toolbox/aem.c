/*
 * Apple Event Manager (the required events and the descriptor calls they need).
 *
 * The host queues events (oapp, odoc, pdoc, quit); the app receives a
 * kHighLevelEvent from WaitNextEvent and calls AEProcessAppleEvent, which
 * builds the AppleEvent in guest memory and calls the installed handler.
 *
 * Descriptor data is private to us (apps only use the API):
 *   records and events: u32 nattr, u32 nparam, then entries
 *   lists:              u32 count, then entries
 *   entry:              keyword(4) type(4) length(4) data (padded to even)
 * Attributes come first in events.
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "machine.h"
#include "tb_internal.h"

enum {
    errAECoercionFail = -1700, errAEDescNotFound = -1701, errAEWrongDataType = -1703, errAENotAEDesc = -1704,
    errAEBadListItem = -1705, errAEEventNotHandled = -1708, errAEHandlerNotFound = -1717,
};

#define T(s) fourcc(s)

/* ---------------------------------------------------------------------- */
/* Host queue */

typedef struct {
    uint32_t cls, id;
    int nfiles;
    char **files;
} pending_ae;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pending_ae queue[16];
static int qn;
static pending_ae current; /* delivered to the app, waiting for AEProcessAppleEvent */

static void queue_event(uint32_t cls, uint32_t id, const char *const *files, int n)
{
    pthread_mutex_lock(&lock);
    if (qn < 16) {
        pending_ae *e = &queue[qn++];
        e->cls = cls;
        e->id = id;
        e->nfiles = n;
        e->files = n ? calloc((size_t)n, sizeof *e->files) : NULL;
        for (int i = 0; i < n; i++)
            e->files[i] = strdup(files[i]);
    }
    pthread_mutex_unlock(&lock);
    void ev_wake(void);
    ev_wake();
}

void machine_open_documents(const char *const *paths, int n, bool print)
{
    queue_event(T("aevt"), print ? T("pdoc") : T("odoc"), paths, n);
}

void machine_send_open_application(void) { queue_event(T("aevt"), T("oapp"), NULL, 0); }
void machine_send_quit(void) { queue_event(T("aevt"), T("quit"), NULL, 0); }

/* Ask the app to quit as the System 7 Finder did: a Quit Application event
 * if it takes high-level events, otherwise its own Quit menu item. */
static bool has_handler(uint32_t cls, uint32_t id);

void machine_request_quit(void)
{
    bool aware = (g_app_size_flags & 0x0040) || has_handler(T("aevt"), T("quit"));
    if (aware) {
        machine_send_quit();
        return;
    }
    static machine_menu menus[32];
    int n = machine_menus(menus, 32);
    for (int i = 0; i < n; i++)
        for (int k = 0; k < menus[i].nitems; k++)
            if (!strncasecmp(menus[i].items[k].text, "Quit", 4) && menus[i].items[k].enabled) {
                machine_choose_menu(menus[i].id, (int16_t)(k + 1));
                return;
            }
    machine_post_key('q', 12, 0x0100, 1, 0); /* Command-Q */
    machine_post_key('q', 12, 0x0100, 0, 0);
}

static void free_event(pending_ae *e)
{
    for (int i = 0; i < e->nfiles; i++)
        free(e->files[i]);
    free(e->files);
    memset(e, 0, sizeof *e);
}

/* For the Event Manager: take the next queued event (true if one was waiting). */

bool aem_next_event(uint32_t *cls, uint32_t *id, bool remove)
{
    pthread_mutex_lock(&lock);
    /* High-level events go to apps that say they take them (SIZE) or that
     * installed a handler.  Others got their documents at launch
     * (AppParmHandle), as with the System 7 Finder; drop the rest. */
    bool aware = (g_app_size_flags & 0x0040) != 0;
    while (qn > 0 && !aware && !has_handler(queue[0].cls, queue[0].id)) {
        free_event(&queue[0]);
        memmove(queue, queue + 1, (size_t)(qn - 1) * sizeof *queue);
        qn--;
    }
    bool have = qn > 0;
    if (have) {
        *cls = queue[0].cls;
        *id = queue[0].id;
        if (remove) {
            free_event(&current);
            current = queue[0];
            memmove(queue, queue + 1, (size_t)(qn - 1) * sizeof *queue);
            qn--;
        }
    }
    pthread_mutex_unlock(&lock);
    return have;
}

void aem_reset(void)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < qn; i++)
        free_event(&queue[i]);
    qn = 0;
    free_event(&current);
    pthread_mutex_unlock(&lock);
}

/* ---------------------------------------------------------------------- */
/* Descriptor data */

static uint32_t pad(uint32_t n) { return (n + 1) & ~1u; }

static gaddr desc_handle(gaddr desc) { return AEDesc_dataHandle(desc); }
static uint32_t desc_type(gaddr desc) { return (uint32_t)AEDesc_descriptorType(desc); }

static void set_desc(gaddr desc, uint32_t type, gaddr h)
{
    AEDesc_set_descriptorType(desc, (int32_t)type);
    AEDesc_set_dataHandle(desc, h);
}

static gaddr new_handle_with(const void *data, uint32_t n, gaddr src)
{
    gaddr h = tb_NewHandle((int32_t)n, false, false);
    if (!h)
        return 0;
    if (data)
        gput(rd32(h), data, n);
    else if (src)
        gmemcpy(rd32(h), src, n);
    return h;
}

/* Entry walking. Returns offset of the entry with keyword (or index), 0 if none. */
static bool is_record(uint32_t type) { return type == T("aevt") || type == T("reco"); }

static uint32_t first_entry(gaddr h, uint32_t type, uint32_t *count, bool attrs)
{
    gaddr p = rd32(h);
    if (is_record(type)) {
        uint32_t na = rd32(p), np = rd32(p + 4);
        uint32_t off = 8;
        if (!attrs) { /* skip the attributes */
            for (uint32_t i = 0; i < na; i++)
                off += 12 + pad(rd32(p + off + 8));
        }
        *count = attrs ? na : np;
        return off;
    }
    *count = rd32(p);
    return 4;
}

static uint32_t find_entry(gaddr h, uint32_t type, uint32_t key, bool attrs)
{
    uint32_t n, off = first_entry(h, type, &n, attrs);
    gaddr p = rd32(h);
    for (uint32_t i = 0; i < n; i++) {
        if (rd32(p + off) == key)
            return off;
        off += 12 + pad(rd32(p + off + 8));
    }
    return 0;
}

static uint32_t nth_entry(gaddr h, uint32_t type, int32_t index)
{
    uint32_t n, off = first_entry(h, type, &n, false);
    gaddr p = rd32(h);
    if (index < 1 || (uint32_t)index > n)
        return 0;
    for (int32_t i = 1; i < index; i++)
        off += 12 + pad(rd32(p + off + 8));
    return off;
}

static void append_entry(gaddr h, uint32_t type, bool attr, uint32_t key, uint32_t etype, gaddr data, uint32_t len,
                         const void *host_data)
{
    int32_t size = tb_GetHandleSize(h);
    uint32_t entry = 12 + pad(len);
    uint32_t at = (uint32_t)size;
    if (is_record(type) && attr) {
        /* Attributes go before the parameters. */
        uint32_t n, off = first_entry(h, type, &n, true);
        for (uint32_t i = 0; i < n; i++)
            off += 12 + pad(rd32(rd32(h) + off + 8));
        at = off;
    }
    tb_SetHandleSize(h, size + (int32_t)entry);
    gaddr p = rd32(h);
    gmemcpy(p + at + entry, p + at, (uint32_t)size - at);
    wr32(p + at, key);
    wr32(p + at + 4, etype);
    wr32(p + at + 8, len);
    if (host_data)
        gput(p + at + 12, host_data, len);
    else if (len)
        gmemcpy(p + at + 12, data, len);
    if (pad(len) != len)
        wr8(p + at + 12 + len, 0);
    if (is_record(type))
        wr32(p + (attr ? 0 : 4), rd32(p + (attr ? 0 : 4)) + 1);
    else
        wr32(p, rd32(p) + 1);
}

static void remove_entry(gaddr h, uint32_t off, uint32_t type, bool attr)
{
    gaddr p = rd32(h);
    uint32_t len = 12 + pad(rd32(p + off + 8));
    int32_t size = tb_GetHandleSize(h);
    gmemcpy(p + off, p + off + len, (uint32_t)size - off - len);
    tb_SetHandleSize(h, size - (int32_t)len);
    p = rd32(h);
    if (is_record(type))
        wr32(p + (attr ? 0 : 4), rd32(p + (attr ? 0 : 4)) - 1);
    else
        wr32(p, rd32(p) - 1);
}

/* Copy an entry's data out to (data, max), or into a new descriptor. */
/* The system's built-in coercions between numbers (shor, long, magn,
 * bool, sing, doub) and among the four-character types (enum, type, keyw):
 * data of `type` at `src` as `desired`, into out (big-endian); its length,
 * or -1. */
static int coerce_bytes(uint32_t type, gaddr src, uint32_t len, uint32_t desired, uint8_t out[8])
{
    double v;
    if (type == T("shor") && len == 2)
        v = (int16_t)rd16(src);
    else if ((type == T("long") || type == T("magn")) && len == 4)
        v = type == T("long") ? (double)(int32_t)rd32(src) : (double)rd32(src);
    else if (type == T("bool") && len >= 1)
        v = rd8(src) != 0;
    else if (type == T("sing") && len == 4) {
        uint32_t b = rd32(src);
        float f;
        memcpy(&f, &b, 4);
        v = f;
    } else if (type == T("doub") && len == 8) {
        uint64_t b = (uint64_t)rd32(src) << 32 | rd32(src + 4);
        memcpy(&v, &b, 8);
    } else if ((type == T("enum") || type == T("type") || type == T("keyw")) && len == 4 &&
               (desired == T("enum") || desired == T("type") || desired == T("keyw"))) {
        uint32_t b = rd32(src);
        for (int i = 0; i < 4; i++)
            out[i] = (uint8_t)(b >> (24 - 8 * i));
        return 4;
    } else
        return -1;
    if (desired == T("shor") || desired == T("long") || desired == T("magn") || desired == T("bool")) {
        if (v != (double)(int64_t)v)
            return -1; /* not a whole number */
        int64_t n = (int64_t)v;
        if (desired == T("shor")) {
            if (n < -32768 || n > 32767)
                return -1;
            out[0] = (uint8_t)(n >> 8), out[1] = (uint8_t)n;
            return 2;
        }
        if (desired == T("bool")) {
            if (n != 0 && n != 1)
                return -1;
            out[0] = (uint8_t)n;
            return 1;
        }
        if (desired == T("long") ? (n < INT32_MIN || n > INT32_MAX) : (n < 0 || n > UINT32_MAX))
            return -1;
        for (int i = 0; i < 4; i++)
            out[i] = (uint8_t)((uint64_t)n >> (24 - 8 * i));
        return 4;
    }
    if (desired == T("sing")) {
        float f = (float)v;
        uint32_t b;
        memcpy(&b, &f, 4);
        for (int i = 0; i < 4; i++)
            out[i] = (uint8_t)(b >> (24 - 8 * i));
        return 4;
    }
    if (desired == T("doub")) {
        uint64_t b;
        memcpy(&b, &v, 8);
        for (int i = 0; i < 8; i++)
            out[i] = (uint8_t)(b >> (56 - 8 * i));
        return 8;
    }
    return -1;
}

static int16_t entry_to_ptr(gaddr h, uint32_t off, uint32_t desired, gaddr type_out, gaddr data, int32_t max,
                            gaddr size_out)
{
    gaddr p = rd32(h);
    uint32_t type = rd32(p + off + 4), len = rd32(p + off + 8);
    if (desired != T("****") && desired != type) {
        uint8_t buf[8];
        int n = coerce_bytes(type, p + off + 12, len, desired, buf);
        if (n < 0)
            return errAECoercionFail;
        if (type_out)
            wr32(type_out, desired);
        if (size_out)
            wr32(size_out, (uint32_t)n);
        for (int i = 0; data && i < n && i < max; i++)
            wr8(data + (uint32_t)i, buf[i]);
        return noErr;
    }
    if (type_out)
        wr32(type_out, type);
    if (size_out)
        wr32(size_out, len);
    if (data && max > 0)
        gmemcpy(data, p + off + 12, len < (uint32_t)max ? len : (uint32_t)max);
    return noErr;
}

static int16_t entry_to_desc(gaddr h, uint32_t off, uint32_t desired, gaddr desc_out)
{
    gaddr p = rd32(h);
    uint32_t type = rd32(p + off + 4), len = rd32(p + off + 8);
    if (desired != T("****") && desired != type) {
        uint8_t buf[8];
        int n = coerce_bytes(type, p + off + 12, len, desired, buf);
        if (n < 0)
            return errAECoercionFail;
        gaddr nh = tb_NewHandle(n, false, false);
        if (!nh)
            return memFullErr;
        for (int i = 0; i < n; i++)
            wr8(rd32(nh) + (uint32_t)i, buf[i]);
        set_desc(desc_out, desired, nh);
        return noErr;
    }
    gaddr nh = new_handle_with(NULL, len, rd32(h) + off + 12);
    set_desc(desc_out, type, nh);
    return nh ? noErr : memFullErr;
}

/* ---------------------------------------------------------------------- */
/* Descriptor calls */

int16_t tb_AECreateDesc(int32_t type, gaddr data, int32_t data_size, gaddr desc_out)
{
    gaddr h = new_handle_with(NULL, (uint32_t)data_size, data);
    set_desc(desc_out, (uint32_t)type, h);
    return h ? noErr : memFullErr;
}

int16_t tb_AEDisposeDesc(gaddr desc)
{
    if (desc_handle(desc))
        tb_DisposeHandle(desc_handle(desc));
    set_desc(desc, T("null"), 0);
    return noErr;
}

int16_t tb_AEDuplicateDesc(gaddr src, gaddr dst)
{
    gaddr h = desc_handle(src);
    if (!h) {
        set_desc(dst, desc_type(src), 0);
        return noErr;
    }
    int32_t n = tb_GetHandleSize(h);
    gaddr nh = new_handle_with(NULL, (uint32_t)n, rd32(h));
    set_desc(dst, desc_type(src), nh);
    return nh ? noErr : memFullErr;
}

/* An Apple event as Process Manager launch parameters ('appa',
 * AppParameters): a high-level event record (class in the message, ID in
 * where), a refCon, the length, then the event's data as we keep it. */
static int16_t coerce_to_app_parameters(gaddr evt, gaddr desc_out)
{
    gaddr h = desc_handle(evt);
    if (!h || !rd32(h))
        return errAENotAEDesc;
    uint32_t n = (uint32_t)tb_GetHandleSize(h);
    uint32_t cls = 0, id = 0;
    uint32_t off = find_entry(h, T("aevt"), T("evcl"), true);
    if (off)
        cls = rd32(rd32(h) + off + 12);
    off = find_entry(h, T("aevt"), T("evid"), true);
    if (off)
        id = rd32(rd32(h) + off + 12);
    gaddr out = tb_NewHandle((int32_t)(24 + n), false, true);
    if (!out)
        return memFullErr;
    gaddr p = rd32(out);
    wr16(p, 23);           /* kHighLevelEvent */
    wr32(p + 2, cls);      /* message */
    wr32(p + 10, id);      /* where */
    wr32(p + 20, n);       /* messageLength (after eventRefCon at 16) */
    gmemcpy(p + 24, rd32(h), n);
    set_desc(desc_out, T("appa"), out);
    return noErr;
}

/* The files an Open Documents event in launch parameters names (host paths,
 * malloc'd); the count, or 0. */
bool alias_host_of_record(gaddr rec, uint32_t len, char *host, size_t hostsize);
int aem_launch_documents(gaddr params, char ***paths_out)
{
    *paths_out = NULL;
    if (!params || rd16(params) != 23 || rd32(params + 2) != T("aevt") || rd32(params + 10) != T("odoc"))
        return 0;
    uint32_t n = rd32(params + 20);
    gaddr h = tb_NewHandle((int32_t)n, false, false);
    if (!h)
        return 0;
    gmemcpy(rd32(h), params + 24, n);
    tb_HLock(h); /* resolving an alias allocates */
    int count = 0;
    char **paths = NULL;
    uint32_t off = find_entry(h, T("aevt"), T("----"), false);
    if (off && rd32(rd32(h) + off + 4) == T("list")) {
        /* The list's own data: count, then entries (keyword, type, length, data). */
        gaddr lp = rd32(h) + off + 12;
        uint32_t items = rd32(lp), at = 4;
        for (uint32_t i = 0; i < items; i++) {
            uint32_t type = rd32(lp + at + 4), len = rd32(lp + at + 8);
            gaddr data = lp + at + 12;
            char host[1024];
            bool ok = false;
            if (type == T("fss ") && len >= 70)
                ok = fm_host_path((int16_t)rd16(data), (int32_t)rd32(data + 2), data + 6, true, host, sizeof host);
            else if (type == T("alis"))
                ok = alias_host_of_record(data, len, host, sizeof host);
            if (ok) {
                paths = realloc(paths, (size_t)(count + 1) * sizeof *paths);
                paths[count++] = strdup(host);
            }
            at += 12 + pad(len);
        }
    }
    tb_DisposeHandle(h);
    *paths_out = paths;
    return count;
}

int16_t tb_AECoerceDesc(gaddr desc, int32_t result_type, gaddr desc_out)
{
    if ((uint32_t)result_type == desc_type(desc) || (uint32_t)result_type == T("****"))
        return tb_AEDuplicateDesc(desc, desc_out);
    if ((uint32_t)result_type == T("appa") && desc_type(desc) == T("aevt"))
        return coerce_to_app_parameters(desc, desc_out);
    gaddr h = desc_handle(desc);
    if (!h)
        return errAECoercionFail;
    return tb_AECoercePtr((int32_t)desc_type(desc), rd32(h), tb_GetHandleSize(h), result_type, desc_out);
}

int16_t tb_AECoercePtr(int32_t data_type, gaddr data, int32_t data_size, int32_t result_type, gaddr desc_out)
{
    if (data_type == result_type || (uint32_t)result_type == T("****"))
        return tb_AECreateDesc(data_type, data, data_size, desc_out);
    uint8_t buf[8];
    int n = coerce_bytes((uint32_t)data_type, data, (uint32_t)data_size, (uint32_t)result_type, buf);
    if (n < 0)
        return errAECoercionFail;
    gaddr tmp = tb_NewPtr(n, false, false);
    for (int i = 0; i < n; i++)
        wr8(tmp + (uint32_t)i, buf[i]);
    int16_t err = tb_AECreateDesc(result_type, tmp, n, desc_out);
    tb_DisposePtr(tmp);
    return err;
}

int16_t tb_AECountItems(gaddr list, gaddr count_out)
{
    gaddr h = desc_handle(list);
    if (!h)
        return errAENotAEDesc;
    uint32_t n;
    first_entry(h, desc_type(list), &n, false);
    wr32(count_out, n);
    return noErr;
}

int16_t tb_AEGetNthPtr(gaddr list, int32_t index, int32_t desired_type, gaddr keyword_out, gaddr type_out, gaddr data,
                       int32_t max_size, gaddr size_out)
{
    gaddr h = desc_handle(list);
    uint32_t off = h ? nth_entry(h, desc_type(list), index) : 0;
    if (!off)
        return errAEBadListItem;
    if (keyword_out)
        wr32(keyword_out, rd32(rd32(h) + off));
    return entry_to_ptr(h, off, (uint32_t)desired_type, type_out, data, max_size, size_out);
}

int16_t tb_AEGetNthDesc(gaddr list, int32_t index, int32_t desired_type, gaddr keyword_out, gaddr desc_out)
{
    gaddr h = desc_handle(list);
    uint32_t off = h ? nth_entry(h, desc_type(list), index) : 0;
    if (!off)
        return errAEBadListItem;
    if (keyword_out)
        wr32(keyword_out, rd32(rd32(h) + off));
    return entry_to_desc(h, off, (uint32_t)desired_type, desc_out);
}

int16_t tb_AEGetParamPtr(gaddr record, int32_t keyword, int32_t desired_type, gaddr type_out, gaddr data,
                         int32_t max_size, gaddr size_out)
{
    gaddr h = desc_handle(record);
    uint32_t off = h ? find_entry(h, desc_type(record), (uint32_t)keyword, false) : 0;
    if (!off)
        return errAEDescNotFound;
    return entry_to_ptr(h, off, (uint32_t)desired_type, type_out, data, max_size, size_out);
}

int16_t tb_AEGetParamDesc(gaddr record, int32_t keyword, int32_t desired_type, gaddr desc_out)
{
    gaddr h = desc_handle(record);
    uint32_t off = h ? find_entry(h, desc_type(record), (uint32_t)keyword, false) : 0;
    if (!off) {
        set_desc(desc_out, T("null"), 0);
        return errAEDescNotFound;
    }
    return entry_to_desc(h, off, (uint32_t)desired_type, desc_out);
}

int16_t tb_AEGetAttributePtr(gaddr evt, int32_t keyword, int32_t desired_type, gaddr type_out, gaddr data,
                             int32_t max_size, gaddr size_out)
{
    gaddr h = desc_handle(evt);
    uint32_t off = h ? find_entry(h, desc_type(evt), (uint32_t)keyword, true) : 0;
    if (!off)
        return errAEDescNotFound; /* e.g. keyMissedKeywordAttr: nothing was missed */
    return entry_to_ptr(h, off, (uint32_t)desired_type, type_out, data, max_size, size_out);
}

int16_t tb_AEGetAttributeDesc(gaddr evt, int32_t keyword, int32_t desired_type, gaddr desc_out)
{
    gaddr h = desc_handle(evt);
    uint32_t off = h ? find_entry(h, desc_type(evt), (uint32_t)keyword, true) : 0;
    if (!off) {
        set_desc(desc_out, T("null"), 0);
        return errAEDescNotFound;
    }
    return entry_to_desc(h, off, (uint32_t)desired_type, desc_out);
}

int16_t tb_AEPutParamPtr(gaddr record, int32_t keyword, int32_t type, gaddr data, int32_t data_size)
{
    gaddr h = desc_handle(record);
    if (!h)
        return errAENotAEDesc;
    uint32_t off = find_entry(h, desc_type(record), (uint32_t)keyword, false);
    if (off)
        remove_entry(h, off, desc_type(record), false);
    append_entry(h, desc_type(record), false, (uint32_t)keyword, (uint32_t)type, data, (uint32_t)data_size, NULL);
    return noErr;
}

int16_t tb_AEPutParamDesc(gaddr record, int32_t keyword, gaddr desc)
{
    gaddr dh = desc_handle(desc);
    int32_t n = dh ? tb_GetHandleSize(dh) : 0;
    tb_HLock(dh);
    int16_t err = tb_AEPutParamPtr(record, keyword, (int32_t)desc_type(desc), dh ? rd32(dh) : 0, n);
    tb_HUnlock(dh);
    return err;
}

/* ---------------------------------------------------------------------- */
/* Handlers */

typedef struct {
    uint32_t cls, id;
    gaddr handler;
    int32_t refcon;
} handler_entry;

static handler_entry handlers[64];
static int nhandlers;
static uint8_t interaction = 1;
static gaddr current_event_desc;

int16_t tb_AEInstallEventHandler(int32_t event_class, int32_t event_id, gaddr hdlr, int32_t refcon, int8_t system_handler_p)
{
    for (int i = 0; i < nhandlers; i++)
        if (handlers[i].cls == (uint32_t)event_class && handlers[i].id == (uint32_t)event_id) {
            handlers[i].handler = hdlr;
            handlers[i].refcon = refcon;
            return noErr;
        }
    if (nhandlers >= 64)
        return memFullErr;
    handlers[nhandlers++] = (handler_entry){ (uint32_t)event_class, (uint32_t)event_id, hdlr, refcon };
    return noErr;
}

int16_t tb_AERemoveEventHandler(int32_t event_class, int32_t event_id, gaddr hdlr, int8_t system_handler_p)
{
    for (int i = 0; i < nhandlers; i++)
        if (handlers[i].cls == (uint32_t)event_class && handlers[i].id == (uint32_t)event_id) {
            handlers[i] = handlers[--nhandlers];
            return noErr;
        }
    return errAEHandlerNotFound;
}

int16_t tb_AEGetEventHandler(int32_t event_class, int32_t event_id, gaddr hdlr, gaddr refcon, int8_t system_handler_p)
{
    for (int i = 0; i < nhandlers; i++)
        if (handlers[i].cls == (uint32_t)event_class && handlers[i].id == (uint32_t)event_id) {
            wr32(hdlr, handlers[i].handler);
            wr32(refcon, (uint32_t)handlers[i].refcon);
            return noErr;
        }
    return errAEHandlerNotFound;
}

static const handler_entry *find_handler(uint32_t cls, uint32_t id)
{
    const handler_entry *best = NULL;
    for (int i = 0; i < nhandlers; i++) {
        bool c = handlers[i].cls == cls || handlers[i].cls == T("****");
        bool d = handlers[i].id == id || handlers[i].id == T("****");
        if (c && d && (!best || (handlers[i].cls == cls && handlers[i].id == id)))
            best = &handlers[i];
    }
    return best;
}

static bool has_handler(uint32_t cls, uint32_t id) { return find_handler(cls, id) != NULL; }


/* Build the event delivered to the app as an AppleEvent descriptor. */
static void build_event(const pending_ae *e, gaddr desc)
{
    gaddr h = tb_NewHandle(8, false, true);
    set_desc(desc, T("aevt"), h);
    uint32_t cls = __builtin_bswap32(e->cls), id = __builtin_bswap32(e->id);
    append_entry(h, T("aevt"), true, T("evcl"), T("type"), 0, 4, &cls);
    append_entry(h, T("aevt"), true, T("evid"), T("type"), 0, 4, &id);
    if (e->nfiles) {
        /* Direct object: a list of FSSpecs. */
        gaddr list = tb_NewHandle(4, false, true);
        gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
        for (int i = 0; i < e->nfiles; i++) {
            fm_grant(e->files[i], false); /* documents the user opened */
            fm_spec_for_path(e->files[i], spec);
            append_entry(list, T("list"), false, 0, T("fss "), spec, FSSpec_SIZEOF, NULL);
        }
        tb_DisposePtr(spec);
        int32_t n = tb_GetHandleSize(list);
        tb_HLock(list);
        append_entry(h, T("aevt"), false, T("----"), T("list"), rd32(list), (uint32_t)n, NULL);
        tb_DisposeHandle(list);
    }
}

int16_t tb_AEProcessAppleEvent(gaddr evt)
{
    pthread_mutex_lock(&lock);
    pending_ae e = current;
    memset(&current, 0, sizeof current);
    pthread_mutex_unlock(&lock);
    if (!e.cls)
        return errAEEventNotHandled;
    const handler_entry *hd = find_handler(e.cls, e.id);
    int16_t result = errAEEventNotHandled;
    if (hd) {
        gaddr event = tb_NewPtr(AEDesc_SIZEOF, false, true), reply = tb_NewPtr(AEDesc_SIZEOF, false, true);
        build_event(&e, event);
        set_desc(reply, T("aevt"), tb_NewHandle(8, false, true));
        current_event_desc = event;
        cpu68k *c = g_cpu;
        cpu_push16(c, 0); /* OSErr result */
        cpu_push32(c, event);
        cpu_push32(c, reply);
        cpu_push32(c, (uint32_t)hd->refcon);
        guest_call(hd->handler);
        result = (int16_t)cpu_pop16(c);
        current_event_desc = 0;
        tb_AEDisposeDesc(event);
        tb_AEDisposeDesc(reply);
        tb_DisposePtr(event);
        tb_DisposePtr(reply);
    }
    free_event(&e);
    return result;
}

/* ---------------------------------------------------------------------- */
/* Building lists, records and events */

int16_t tb_AECreateList(gaddr prefix, int32_t prefix_size, int8_t is_record, gaddr list_out)
{
    gaddr h = tb_NewHandle(is_record ? 8 : 4, false, true);
    set_desc(list_out, is_record ? T("reco") : T("list"), h);
    return h ? noErr : memFullErr;
}

/* index 0 (or one past the end) appends; otherwise the item is replaced. */
int16_t tb_AEPutPtr(gaddr list, int32_t index, int32_t type, gaddr data, int32_t size)
{
    gaddr h = desc_handle(list);
    if (!h)
        return errAENotAEDesc;
    uint32_t n;
    first_entry(h, desc_type(list), &n, false);
    if (index < 0 || (uint32_t)index > n + 1)
        return errAEBadListItem;
    if (index == 0 || (uint32_t)index == n + 1) {
        append_entry(h, desc_type(list), false, 0, (uint32_t)type, data, (uint32_t)size, NULL);
        return noErr;
    }
    /* Replace: rebuild with the new item in place. */
    gaddr nh = tb_NewHandle(is_record(desc_type(list)) ? 8 : 4, false, true);
    for (uint32_t i = 1; i <= n; i++) {
        uint32_t off = nth_entry(h, desc_type(list), (int32_t)i);
        gaddr p = rd32(h);
        if ((int32_t)i == index) {
            append_entry(nh, desc_type(list), false, rd32(p + off), (uint32_t)type, data, (uint32_t)size, NULL);
        } else {
            uint32_t len = rd32(p + off + 8);
            uint8_t *tmp = malloc(len ? len : 1);
            gget(tmp, p + off + 12, len);
            append_entry(nh, desc_type(list), false, rd32(p + off), rd32(p + off + 4), 0, len, tmp);
            free(tmp);
        }
    }
    tb_DisposeHandle(h);
    AEDesc_set_dataHandle(list, nh);
    return noErr;
}

int16_t tb_AEPutDesc(gaddr list, int32_t index, gaddr desc)
{
    gaddr dh = desc_handle(desc);
    int32_t n = dh ? tb_GetHandleSize(dh) : 0;
    if (dh)
        tb_HLock(dh);
    int16_t err = tb_AEPutPtr(list, index, (int32_t)desc_type(desc), dh ? rd32(dh) : 0, n);
    if (dh)
        tb_HUnlock(dh);
    return err;
}

int16_t tb_AEDeleteItem(gaddr list, int32_t index)
{
    gaddr h = desc_handle(list);
    uint32_t off = h ? nth_entry(h, desc_type(list), index) : 0;
    if (!off)
        return errAEBadListItem;
    remove_entry(h, off, desc_type(list), false);
    return noErr;
}

int16_t tb_AEDeleteParam(gaddr record, int32_t keyword)
{
    gaddr h = desc_handle(record);
    uint32_t off = h ? find_entry(h, desc_type(record), (uint32_t)keyword, false) : 0;
    if (!off)
        return errAEDescNotFound;
    remove_entry(h, off, desc_type(record), false);
    return noErr;
}

static int16_t size_of(gaddr h, uint32_t off, gaddr type_out, gaddr size_out)
{
    if (!off)
        return errAEDescNotFound;
    if (type_out)
        wr32(type_out, rd32(rd32(h) + off + 4));
    if (size_out)
        wr32(size_out, rd32(rd32(h) + off + 8));
    return noErr;
}

int16_t tb_AESizeOfNthItem(gaddr list, int32_t index, gaddr type_out, gaddr size_out)
{
    gaddr h = desc_handle(list);
    uint32_t off = h ? nth_entry(h, desc_type(list), index) : 0;
    return off ? size_of(h, off, type_out, size_out) : errAEBadListItem;
}

int16_t tb_AESizeOfParam(gaddr record, int32_t keyword, gaddr type_out, gaddr size_out)
{
    gaddr h = desc_handle(record);
    return size_of(h, h ? find_entry(h, desc_type(record), (uint32_t)keyword, false) : 0, type_out, size_out);
}

int16_t tb_AESizeOfAttribute(gaddr evt, int32_t keyword, gaddr type_out, gaddr size_out)
{
    gaddr h = desc_handle(evt);
    return size_of(h, h ? find_entry(h, desc_type(evt), (uint32_t)keyword, true) : 0, type_out, size_out);
}

int16_t tb_AEPutAttributePtr(gaddr evt, int32_t keyword, int32_t type, gaddr data, int32_t size)
{
    gaddr h = desc_handle(evt);
    if (!h || desc_type(evt) != T("aevt"))
        return errAENotAEDesc;
    uint32_t off = find_entry(h, T("aevt"), (uint32_t)keyword, true);
    if (off)
        remove_entry(h, off, T("aevt"), true);
    append_entry(h, T("aevt"), true, (uint32_t)keyword, (uint32_t)type, data, (uint32_t)size, NULL);
    return noErr;
}

int16_t tb_AEPutAttributeDesc(gaddr evt, int32_t keyword, gaddr desc)
{
    gaddr dh = desc_handle(desc);
    int32_t n = dh ? tb_GetHandleSize(dh) : 0;
    if (dh)
        tb_HLock(dh);
    int16_t err = tb_AEPutAttributePtr(evt, keyword, (int32_t)desc_type(desc), dh ? rd32(dh) : 0, n);
    if (dh)
        tb_HUnlock(dh);
    return err;
}

int16_t tb_AECreateAppleEvent(int32_t cls, int32_t id, gaddr target, int16_t return_id, int32_t transaction,
                              gaddr evt)
{
    gaddr h = tb_NewHandle(8, false, true);
    if (!h)
        return memFullErr;
    set_desc(evt, T("aevt"), h);
    uint32_t c = __builtin_bswap32((uint32_t)cls), i = __builtin_bswap32((uint32_t)id);
    uint32_t rid = __builtin_bswap32((uint32_t)(int32_t)return_id), tid = __builtin_bswap32((uint32_t)transaction);
    append_entry(h, T("aevt"), true, T("evcl"), T("type"), 0, 4, &c);
    append_entry(h, T("aevt"), true, T("evid"), T("type"), 0, 4, &i);
    append_entry(h, T("aevt"), true, T("rtid"), T("shor"), 0, 2, (uint8_t *)&rid + 2);
    append_entry(h, T("aevt"), true, T("tran"), T("long"), 0, 4, &tid);
    if (target && desc_handle(target))
        tb_AEPutAttributeDesc(evt, (int32_t)T("addr"), target);
    return noErr;
}

int16_t tb_AEGetArray(gaddr list, uint8_t array_type, gaddr array_ptr, int32_t max_size, gaddr item_type,
                      gaddr item_size, gaddr item_count)
{
    return errAEWrongDataType;
}

int16_t tb_AEPutArray(gaddr list, uint8_t type, gaddr data, int32_t item_type, int32_t item_size, int32_t count)
{
    return errAEWrongDataType;
}

/* AESend: events to ourselves go straight to our handler; there is no
 * other process to send to. */
static bool to_self(gaddr evt)
{
    gaddr h = desc_handle(evt);
    uint32_t off = h ? find_entry(h, T("aevt"), T("addr"), true) : 0;
    if (!off)
        return true;
    gaddr p = rd32(h);
    uint32_t type = rd32(p + off + 4);
    if (type == T("psn "))
        return rd32(p + off + 12) == 0 && (rd32(p + off + 16) == 2 || rd32(p + off + 16) == 0x1000); /* us */
    return false;
}

static uint32_t attr_long(gaddr evt, uint32_t key)
{
    gaddr h = desc_handle(evt);
    uint32_t off = h ? find_entry(h, T("aevt"), key, true) : 0;
    return off ? rd32(rd32(h) + off + 12) : 0;
}

/* ---- The Finder ----
 * There is no Finder process, so the events applications send it to open
 * things (Open Documents, and the Finder's own Open Selection from file
 * browsers such as Greg's Browser) are carried out here: applications are
 * launched, documents opened with their creator's application. */

static bool to_finder(gaddr evt)
{
    gaddr h = desc_handle(evt);
    uint32_t off = h ? find_entry(h, T("aevt"), T("addr"), true) : 0;
    return off && rd32(rd32(h) + off + 4) == T("sign") && rd32(rd32(h) + off + 12) == T("MACS");
}

/* Host paths of an alias, FSSpec or list of them (in a handle, at off). */
static int item_paths(gaddr h, uint32_t off, char ***paths, int count)
{
    gaddr p = rd32(h) + off;
    uint32_t type = rd32(p + 4), len = rd32(p + 8);
    gaddr data = p + 12;
    if (type == T("list")) {
        /* Entries (keyword, type, length, data) after the count; resolving
         * an alias allocates, so offsets, not addresses, are kept. */
        uint32_t items = rd32(data), at = off + 16;
        for (uint32_t i = 0; i < items && at < off + 12 + len; i++) {
            count = item_paths(h, at, paths, count);
            at += 12 + pad(rd32(rd32(h) + at + 8));
        }
        return count;
    }
    char host[1024];
    bool ok = false;
    if (type == T("fss ") && len >= 70)
        ok = fm_host_path((int16_t)rd16(data), (int32_t)rd32(data + 2), data + 6, true, host, sizeof host);
    else if (type == T("alis"))
        ok = alias_host_of_record(data, len, host, sizeof host);
    if (ok) {
        *paths = realloc(*paths, (size_t)(count + 1) * sizeof **paths);
        (*paths)[count++] = strdup(host);
    }
    return count;
}

/* Open one item as the Finder would. */
static int16_t finder_open(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -43; /* fnfErr */
    if (S_ISDIR(st.st_mode))
        return noErr; /* a folder: there is no Finder window to open */
    uint32_t type = fm_type_of_path(path);
    if (type == T("APPL") || type == T("APPC") || type == T("APPD") || type == T("appe"))
        return process_launch(path, NULL, 0) ? noErr : -600;
    char app[1024];
    if (!fm_app_for_document(path, app, sizeof app))
        return -35; /* the Finder's "application not found" */
    const char *docs[1] = { path };
    return process_launch(app, docs, 1) ? noErr : -600;
}

static int16_t finder_event(gaddr evt)
{
    uint32_t cls = attr_long(evt, T("evcl")), id = attr_long(evt, T("evid"));
    bool open = (cls == T("aevt") && id == T("odoc")) || (cls == T("FNDR") && id == T("sope"));
    if (!open)
        return cls == T("aevt") && id == T("oapp") ? noErr : errAEEventNotHandled;
    gaddr h = desc_handle(evt);
    /* Open Selection names the items in 'fsel' (its direct object is their
     * folder); Open Documents in the direct object. */
    uint32_t off = cls == T("FNDR") ? find_entry(h, T("aevt"), T("fsel"), false) : 0;
    if (!off)
        off = find_entry(h, T("aevt"), T("----"), false);
    if (!off)
        return errAEDescNotFound;
    char **paths = NULL;
    int n = item_paths(h, off, &paths, 0);
    int16_t err = noErr;
    for (int i = 0; i < n; i++) {
        int16_t e = finder_open(paths[i]);
        if (e && !err)
            err = e;
        free(paths[i]);
    }
    free(paths);
    return err;
}

int16_t tb_AESend(gaddr evt, gaddr reply, int32_t send_mode, int16_t priority, int32_t timeout, gaddr idle,
                  gaddr filter)
{
    if (reply)
        set_desc(reply, T("null"), 0);
    if (to_finder(evt))
        return finder_event(evt);
    if (!to_self(evt))
        return -600; /* procNotFound */
    const handler_entry *hd = find_handler(attr_long(evt, T("evcl")), attr_long(evt, T("evid")));
    if (!hd)
        return errAEEventNotHandled;
    gaddr r = tb_NewPtr(AEDesc_SIZEOF, false, true);
    set_desc(r, T("aevt"), tb_NewHandle(8, false, true));
    gaddr saved = current_event_desc;
    current_event_desc = evt;
    cpu68k *c = g_cpu;
    cpu_push16(c, 0);
    cpu_push32(c, evt);
    cpu_push32(c, r);
    cpu_push32(c, (uint32_t)hd->refcon);
    guest_call(hd->handler);
    int16_t result = (int16_t)cpu_pop16(c);
    current_event_desc = saved;
    /* kAEWaitReply (3): the caller gets the reply. */
    if (reply && (send_mode & 3) == 3)
        gmemcpy(reply, r, AEDesc_SIZEOF);
    else
        tb_AEDisposeDesc(r);
    tb_DisposePtr(r);
    return result;
}

int16_t tb_AEResetTimer(gaddr evt) { return noErr; }
int16_t tb_AESetTheCurrentEvent(gaddr evt)
{
    current_event_desc = evt;
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Coercion and special handlers: recorded; our coercions are identities. */

typedef struct {
    uint32_t from, to;
    gaddr handler;
    int32_t refcon;
    bool from_desc;
} coercion_entry;

static coercion_entry coercions[32];
static int ncoercions;

int16_t tb_AEInstallCoercionHandler(int32_t from, int32_t to, gaddr hdlr, int32_t refcon, int8_t from_desc, int8_t sys)
{
    for (int i = 0; i < ncoercions; i++)
        if (coercions[i].from == (uint32_t)from && coercions[i].to == (uint32_t)to) {
            coercions[i] = (coercion_entry){ (uint32_t)from, (uint32_t)to, hdlr, refcon, from_desc != 0 };
            return noErr;
        }
    if (ncoercions >= 32)
        return memFullErr;
    coercions[ncoercions++] = (coercion_entry){ (uint32_t)from, (uint32_t)to, hdlr, refcon, from_desc != 0 };
    return noErr;
}

int16_t tb_AERemoveCoercionHandler(int32_t from, int32_t to, gaddr hdlr, int8_t sys)
{
    for (int i = 0; i < ncoercions; i++)
        if (coercions[i].from == (uint32_t)from && coercions[i].to == (uint32_t)to) {
            coercions[i] = coercions[--ncoercions];
            return noErr;
        }
    return errAEHandlerNotFound;
}

int16_t tb_AEGetCoercionHandler(int32_t from, int32_t to, gaddr hdlr_out, gaddr refcon_out, gaddr from_desc_out,
                                int8_t sys)
{
    for (int i = 0; i < ncoercions; i++)
        if (coercions[i].from == (uint32_t)from && coercions[i].to == (uint32_t)to) {
            if (hdlr_out)
                wr32(hdlr_out, coercions[i].handler);
            if (refcon_out)
                wr32(refcon_out, (uint32_t)coercions[i].refcon);
            if (from_desc_out)
                wr8(from_desc_out, coercions[i].from_desc);
            return noErr;
        }
    return errAEHandlerNotFound;
}

static struct {
    uint32_t cls;
    gaddr handler;
} specials[16];
static int nspecials;

int16_t tb_AEInstallSpecialHandler(int32_t cls, gaddr hdlr, int8_t sys)
{
    for (int i = 0; i < nspecials; i++)
        if (specials[i].cls == (uint32_t)cls) {
            specials[i].handler = hdlr;
            return noErr;
        }
    if (nspecials >= 16)
        return memFullErr;
    specials[nspecials].cls = (uint32_t)cls;
    specials[nspecials++].handler = hdlr;
    return noErr;
}

int16_t tb_AERemoveSpecialHandler(int32_t cls, gaddr hdlr, int8_t sys)
{
    for (int i = 0; i < nspecials; i++)
        if (specials[i].cls == (uint32_t)cls) {
            specials[i] = specials[--nspecials];
            return noErr;
        }
    return errAEHandlerNotFound;
}

int16_t tb_AEGetSpecialHandler(int32_t cls, gaddr hdlr_out, int8_t sys)
{
    for (int i = 0; i < nspecials; i++)
        if (specials[i].cls == (uint32_t)cls) {
            wr32(hdlr_out, specials[i].handler);
            return noErr;
        }
    return errAEHandlerNotFound;
}

/* AEManagerInfo: the number of Apple Events processed and the like. */
int16_t tb_AEManagerInfo(gaddr result)
{
    if (result)
        wr32(result, 0);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Object Support Library: accessors are recorded; resolving is up to a
 * scripting client, which we don't have, so nothing resolves. */

typedef struct {
    uint32_t desired, container;
    gaddr accessor;
    int32_t refcon;
} accessor_entry;

static accessor_entry accessors[32];
static int naccessors;

int16_t tb_AEInstallObjectAccessor(int32_t desired, int32_t container, gaddr accessor, int32_t refcon, int8_t sys)
{
    for (int i = 0; i < naccessors; i++)
        if (accessors[i].desired == (uint32_t)desired && accessors[i].container == (uint32_t)container) {
            accessors[i].accessor = accessor;
            accessors[i].refcon = refcon;
            return noErr;
        }
    if (naccessors >= 32)
        return memFullErr;
    accessors[naccessors++] = (accessor_entry){ (uint32_t)desired, (uint32_t)container, accessor, refcon };
    return noErr;
}

int16_t tb_AERemoveObjectAccessor(int32_t desired, int32_t container, gaddr accessor, int8_t sys)
{
    for (int i = 0; i < naccessors; i++)
        if (accessors[i].desired == (uint32_t)desired && accessors[i].container == (uint32_t)container) {
            accessors[i] = accessors[--naccessors];
            return noErr;
        }
    return errAEHandlerNotFound;
}

int16_t tb_AEGetObjectAccessor(int32_t desired, int32_t container, gaddr accessor_out, gaddr refcon_out, int8_t sys)
{
    for (int i = 0; i < naccessors; i++)
        if (accessors[i].desired == (uint32_t)desired && accessors[i].container == (uint32_t)container) {
            wr32(accessor_out, accessors[i].accessor);
            wr32(refcon_out, (uint32_t)accessors[i].refcon);
            return noErr;
        }
    return errAEHandlerNotFound;
}

int16_t tb_AECallObjectAccessor(int32_t desired, gaddr container_token, int32_t container_class, int32_t key_form,
                                gaddr key_data, gaddr token)
{
    return -1723; /* errAEAccessorNotFound */
}

int16_t tb_AESetObjectCallbacks(gaddr compare, gaddr count, gaddr dispose, gaddr getmark, gaddr mark, gaddr adjust,
                                gaddr errdesc)
{
    return noErr;
}

int16_t tb_AEResolve(gaddr spec, int16_t flags, gaddr token)
{
    set_desc(token, T("null"), 0);
    return -1728; /* errAENoSuchObject */
}

int16_t tb_AEDisposeToken(gaddr token) { return tb_AEDisposeDesc(token); }

int16_t tb_AEInteractWithUser(int32_t timeout, gaddr nm_req, gaddr idle_proc) { return noErr; }
int16_t tb_AESetInteractionAllowed(uint8_t level) { interaction = level; return noErr; }
int16_t tb_AEGetInteractionAllowed(gaddr return_level) { wr8(return_level, interaction); return noErr; }
int16_t tb_AEGetTheCurrentEvent(gaddr return_evt)
{
    if (current_event_desc)
        gmemcpy(return_evt, current_event_desc, AEDesc_SIZEOF);
    else
        set_desc(return_evt, T("null"), 0);
    return noErr;
}
int16_t tb_AESuspendTheCurrentEvent(gaddr evt) { return noErr; }
int16_t tb_AEResumeTheCurrentEvent(gaddr evt, gaddr reply, gaddr dispatcher, int32_t refcon) { return noErr; }

void aem_reset_handlers(void)
{
    nhandlers = 0;
    ncoercions = 0;
    nspecials = 0;
    naccessors = 0;
}

/* Raw high-level events.  Early System 7 apps (TeachText) accept the
 * message themselves and walk the Apple Event's wire format:
 *   'aevt' 0x00010001
 *   attributes: keyword(4) type(4) size(4) data, ...   ';;;;'
 *   parameters: keyword(4) type(4) size(4) data, ...
 * A list's data is count(4), 0(4), then items: type(4) size(4) data.
 * Documents travel as aliases. */
static uint32_t put_entry(uint8_t *buf, uint32_t n, uint32_t key, uint32_t type, const void *data, uint32_t len)
{
    uint32_t hdr[3] = { __builtin_bswap32(key), __builtin_bswap32(type), __builtin_bswap32(len) };
    memcpy(buf + n, hdr, 12);
    if (len)
        memcpy(buf + n + 12, data, len);
    return n + 12 + len;
}

static uint8_t *flatten_event(const pending_ae *e, uint32_t *len_out)
{
    uint32_t cap = 256 + (uint32_t)e->nfiles * 2048, n = 0;
    uint8_t *buf = calloc(1, cap);
    uint32_t head[2] = { __builtin_bswap32(T("aevt")), __builtin_bswap32(0x00010001) };
    memcpy(buf, head, 8);
    n = 8;
    uint32_t cls = __builtin_bswap32(e->cls), id = __builtin_bswap32(e->id);
    uint32_t psn[2] = { 0, __builtin_bswap32(2) }; /* kCurrentProcess */
    n = put_entry(buf, n, T("evcl"), T("type"), &cls, 4);
    n = put_entry(buf, n, T("evid"), T("type"), &id, 4);
    n = put_entry(buf, n, T("addr"), T("psn "), psn, 8);
    memcpy(buf + n, ";;;;", 4);
    n += 4;
    if (e->nfiles) {
        uint8_t *list = calloc(1, cap);
        uint32_t ln = 8, count = 0;
        gaddr ah = tb_NewPtr(4, false, true);
        for (int i = 0; i < e->nfiles; i++) {
            if (alias_new_for_host(e->files[i], ah) != noErr)
                continue;
            gaddr h = rd32(ah);
            uint32_t size = (uint32_t)tb_GetHandleSize(h);
            uint32_t hdr[2] = { __builtin_bswap32(T("alis")), __builtin_bswap32(size) };
            memcpy(list + ln, hdr, 8);
            gget(list + ln + 8, rd32(h), size);
            ln += 8 + size;
            count++;
            tb_DisposeHandle(h);
        }
        count = __builtin_bswap32(count);
        memcpy(list, &count, 4);
        tb_DisposePtr(ah);
        n = put_entry(buf, n, T("----"), T("list"), list, ln);
        free(list);
    }
    *len_out = n;
    return buf;
}

int16_t tb_AcceptHighLevelEvent(gaddr sender_id_return, gaddr refcon_return, gaddr msg_buf, gaddr msg_length_return)
{
    pthread_mutex_lock(&lock);
    pending_ae e = current;
    pthread_mutex_unlock(&lock);
    if (!e.cls)
        return -615; /* noOutstandingHLE */
    if (refcon_return)
        wr32(refcon_return, 0);
    if (sender_id_return)
        gmemset(sender_id_return, 0, TargetID_SIZEOF);
    uint32_t len;
    uint8_t *buf = flatten_event(&e, &len);
    int16_t err = noErr;
    if (msg_length_return) {
        if (!msg_buf || rd32(msg_length_return) < len)
            err = -607; /* bufferIsSmall */
        else
            gput(msg_buf, buf, len);
        wr32(msg_length_return, len);
    }
    free(buf);
    return err;
}

int8_t tb_GetSpecificHighLevelEvent(gaddr fn, gaddr data, gaddr err_return)
{
    if (err_return)
        wr16(err_return, noErr);
    return 0;
}

int16_t tb_PostHighLevelEvent(gaddr evt, gaddr receiver_id, int32_t refcon, gaddr msg_buf, int32_t msg_length,
                              int32_t post_options)
{
    return -903; /* noPortErr: there is no one else to talk to */
}

/* The Apple Event Manager's private handler-table allocation, which some
 * applications call directly: an empty table. */
int16_t tb__AE_hdlr_table_alloc(int32_t a, int32_t b, int32_t c, int8_t sys, gaddr tablep)
{
    if (tablep)
        wr32(tablep, tb_NewHandle(0, false, true));
    return noErr;
}
