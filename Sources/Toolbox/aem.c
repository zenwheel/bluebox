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
static int16_t entry_to_ptr(gaddr h, uint32_t off, uint32_t desired, gaddr type_out, gaddr data, int32_t max,
                            gaddr size_out)
{
    gaddr p = rd32(h);
    uint32_t type = rd32(p + off + 4), len = rd32(p + off + 8);
    if (desired != T("****") && desired != type)
        return errAECoercionFail;
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
    if (desired != T("****") && desired != type)
        return errAECoercionFail;
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

int16_t tb_AECoerceDesc(gaddr desc, int32_t result_type, gaddr desc_out)
{
    if ((uint32_t)result_type == desc_type(desc) || (uint32_t)result_type == T("****"))
        return tb_AEDuplicateDesc(desc, desc_out);
    return errAECoercionFail;
}

int16_t tb_AECoercePtr(int32_t data_type, gaddr data, int32_t data_size, int32_t result_type, gaddr desc_out)
{
    if (data_type == result_type || (uint32_t)result_type == T("****"))
        return tb_AECreateDesc(data_type, data, data_size, desc_out);
    return errAECoercionFail;
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

void aem_reset_handlers(void) { nhandlers = 0; }

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
