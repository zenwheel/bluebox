/*
 * Apple Event Manager.
 *
 * The host queues events (oapp, odoc, pdoc, quit, do script, or an event
 * another process sent); the app receives a kHighLevelEvent from
 * WaitNextEvent and calls AEProcessAppleEvent, which builds the AppleEvent
 * in guest memory and dispatches it to the installed handler.  Events an
 * application sends itself go straight to its handler.
 *
 * Descriptor data is private to us (apps only use the API), tagged so any
 * descriptor type can hold a list or a record ('obj ', 'rang' and the other
 * object-model records are records coerced to their types):
 *   lists:              'BLst', u32 count, then entries
 *   records and events: 'BRec', u32 nattr, u32 nparam, then entries
 *   entry:              keyword(4) type(4) length(4) data (padded to even)
 * Attributes come first in events.
 */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "forks.h"
#include "machine.h"
#include "tb_internal.h"

enum {
    errAECoercionFail = -1700, errAEDescNotFound = -1701, errAEWrongDataType = -1703, errAENotAEDesc = -1704,
    errAEBadListItem = -1705, errAEEventNotHandled = -1708, errAENoUserInteraction = -1713,
    errAEHandlerNotFound = -1717,
};

enum { kAENoReply = 1, kAEQueueReply = 2, kAEWaitReply = 3 };
enum { kAENeverInteract = 0x10, kAECanInteract = 0x20, kAEAlwaysInteract = 0x30 };
enum { kAEDirectCall = 1, kAESameProcess = 2, kAELocalProcess = 3, kAERemoteProcess = 4 };
enum { kAEInteractWithSelf = 0, kAEInteractWithLocal = 1, kAEInteractWithAll = 2 };

#define T(s) fourcc(s)

static uint32_t rd32be(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

enum { LIST_TAG = 0x424C7374 /* 'BLst' */, RECO_TAG = 0x42526563 /* 'BRec' */ };

static void grow(uint8_t **buf, uint32_t *cap, uint32_t need)
{
    if (need <= *cap)
        return;
    while (*cap < need)
        *cap = *cap ? *cap * 2 : 256;
    *buf = realloc(*buf, *cap);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

/* An entry on a host buffer. */
static void host_entry(uint8_t **buf, uint32_t *n, uint32_t *cap, uint32_t key, uint32_t type, const void *data,
                       uint32_t len)
{
    grow(buf, cap, *n + 12 + len + 1);
    put32(*buf + *n, key);
    put32(*buf + *n + 4, type);
    put32(*buf + *n + 8, len);
    if (len)
        memcpy(*buf + *n + 12, data, len);
    *n += 12 + len;
    if (len & 1)
        (*buf)[(*n)++] = 0;
}

/* An event from the host (as from the Finder), flattened as aem_flatten
 * does, without guest memory: files as host paths, text in MacRoman with
 * Mac line ends.  malloc'd. */
uint8_t *aem_build_flat(uint32_t cls, uint32_t id, const char *const *files, int nfiles, const char *text,
                        bool reply, uint32_t *len_out)
{
    uint8_t *buf = NULL, v[4] = { 0 };
    uint32_t n = 12, cap = 0;
    grow(&buf, &cap, n);
    put32(buf, RECO_TAG);
    put32(buf + 4, reply ? 6 : 5);
    put32(buf + 8, nfiles || text ? 1 : 0);
    put32(v, cls);
    host_entry(&buf, &n, &cap, T("evcl"), T("type"), v, 4);
    put32(v, id);
    host_entry(&buf, &n, &cap, T("evid"), T("type"), v, 4);
    put32(v, T("MACS")); /* from the Finder, as it were */
    host_entry(&buf, &n, &cap, T("addr"), T("sign"), v, 4);
    v[0] = v[1] = 0;
    host_entry(&buf, &n, &cap, T("rtid"), T("shor"), v, 2);
    v[1] = kAELocalProcess;
    host_entry(&buf, &n, &cap, T("esrc"), T("shor"), v, 2);
    if (reply) {
        v[0] = 1;
        host_entry(&buf, &n, &cap, T("repq"), T("bool"), v, 1);
    }
    if (nfiles) {
        /* Direct object: a list of files ('bbfp': FSSpecs to be). */
        uint8_t *list = NULL;
        uint32_t ln = 8, lcap = 0;
        grow(&list, &lcap, ln);
        put32(list, LIST_TAG);
        put32(list + 4, (uint32_t)nfiles);
        for (int i = 0; i < nfiles; i++) {
            size_t plen = strlen(files[i]);
            uint8_t *item = malloc(4 + plen);
            put32(item, T("fss "));
            memcpy(item + 4, files[i], plen);
            host_entry(&list, &ln, &lcap, 0, T("bbfp"), item, (uint32_t)(4 + plen));
            free(item);
        }
        host_entry(&buf, &n, &cap, T("----"), T("list"), list, ln);
        free(list);
    } else if (text) {
        /* Direct object: the text, MacRoman with Mac line ends. */
        size_t tn = strlen(text);
        uint8_t *mac = malloc(tn + 1);
        size_t m = utf8_to_macroman(text, mac, tn + 1);
        for (size_t i = 0; i < m; i++)
            if (mac[i] == '\n')
                mac[i] = '\r';
        host_entry(&buf, &n, &cap, T("----"), T("TEXT"), mac, (uint32_t)m);
        free(mac);
    }
    *len_out = n;
    return buf;
}

/* ---------------------------------------------------------------------- */
/* Host queue */

typedef struct {
    uint32_t cls, id;
    uint8_t *flat;       /* the event, as aem_flatten makes them; a raw event's message */
    uint32_t flatlen;
    machine_ae_reply_fn reply;  /* who wants the reply, if anyone */
    void *reply_ctx;
    /* A high-level event other than an Apple Event (PostHighLevelEvent):
     * its message is the program's own bytes, read with
     * AcceptHighLevelEvent, perhaps a piece at a time. */
    bool raw;
    bool self;           /* posted by this program to itself */
    bool accepted;       /* AcceptHighLevelEvent has taken (some of) it */
    bool receipt;        /* a return receipt, its result in modifiers */
    uint16_t modifiers;
    uint32_t taken;      /* the bytes of the message accepted so far */
    uint32_t refcon, options;
    int sender_pid;      /* the sender, for its TargetID and its receipt */
    uint32_t sender_sig;
    uint8_t sender_name[32];
} pending_ae;

enum { procNotFound = -600, bufferIsSmall = -607, noOutstandingHLE = -608, connectionInvalid = -609 };
enum {
    nAttnMsg = 0x0001, nReturnReceipt = 0x0200, receiverIDMask = 0xF000, receiverIDisTargetID = 0x5000,
    receiverIDisSessionID = 0x6000, receiverIDisSignature = 0x7000, receiverIDisPSN = 0x8000,
};
enum { msgWasNotAccepted = 0, msgWasFullyAccepted = 1, msgWasPartiallyAccepted = 2 };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pending_ae queue[16];
static int qn;
static pending_ae current; /* delivered to the app, waiting for AEProcessAppleEvent */

/* Return receipts owed for raw events that are done with.  Events are
 * freed with and without the queue's lock held, so the receipts wait here
 * (under a lock of their own) and go out once the queue's lock is free. */
static pthread_mutex_t outbox_lock = PTHREAD_MUTEX_INITIALIZER;
static pending_ae outbox[16];
static int nout;

static void free_event(pending_ae *e, bool unanswered)
{
    if (unanswered && e->reply)
        e->reply(e->reply_ctx, NULL, 0, errAEEventNotHandled);
    if (e->raw && (e->options & nReturnReceipt)) {
        /* The receipt says how much of the message the receiver took. */
        uint16_t result = !e->accepted ? msgWasNotAccepted
                          : e->taken >= e->flatlen ? msgWasFullyAccepted
                                                   : msgWasPartiallyAccepted;
        pthread_mutex_lock(&outbox_lock);
        if (nout < 16)
            outbox[nout++] = (pending_ae){ .cls = e->cls, .id = e->id, .refcon = e->refcon,
                                           .modifiers = result, .sender_pid = e->sender_pid,
                                           .sender_sig = e->sender_sig };
        pthread_mutex_unlock(&outbox_lock);
    }
    free(e->flat);
    memset(e, 0, sizeof *e);
}

/* Into the queue, an urgent event (nAttnMsg) at its head; false if it is
 * full, and the event is then dropped. */
static bool enqueue(pending_ae *e)
{
    pthread_mutex_lock(&lock);
    bool full = qn >= 16;
    if (!full) {
        if (e->raw && (e->options & nAttnMsg)) {
            memmove(queue + 1, queue, (size_t)qn * sizeof *queue);
            queue[0] = *e;
            qn++;
        } else
            queue[qn++] = *e;
    }
    pthread_mutex_unlock(&lock);
    if (full)
        free_event(e, true);
    void ev_wake(void);
    ev_wake();
    return !full;
}

static void send_receipts(void);

static void queue_event(uint32_t cls, uint32_t id, const char *const *files, int n, const char *text,
                        machine_ae_reply_fn reply, void *ctx)
{
    uint32_t len;
    uint8_t *flat = aem_build_flat(cls, id, files, n, text, reply != NULL, &len);
    aem_queue_flat(flat, len, reply, ctx);
    free(flat);
}

void machine_open_documents(const char *const *paths, int n, bool print)
{
    queue_event(T("aevt"), print ? T("pdoc") : T("odoc"), paths, n, NULL, NULL, NULL);
}

void machine_send_open_application(void) { queue_event(T("aevt"), T("oapp"), NULL, 0, NULL, NULL, NULL); }
void machine_send_quit(void) { queue_event(T("aevt"), T("quit"), NULL, 0, NULL, NULL, NULL); }

void machine_send_apple_event(uint32_t cls, uint32_t id, const char *const *files, int nfiles, const char *text,
                              machine_ae_reply_fn reply, void *ctx)
{
    queue_event(cls, id, files, nfiles, text, reply, ctx);
}

/* A raw high-level event between processes: a header, then the message.
 *   'BHLE', class, ID, refCon, posting options, the sender's process ID
 *   and signature (4 bytes each), a receipt's result (2), 0 (2), the
 *   sender's name (Str31, 32 bytes) */
enum { RAW_TAG = 0x42484C45 /* 'BHLE' */, RAW_HEAD = 64 };

static uint8_t *raw_flatten(const pending_ae *e, const uint8_t *msg, uint32_t len, uint32_t *len_out)
{
    uint8_t *buf = calloc(1, RAW_HEAD + len);
    put32(buf, RAW_TAG);
    put32(buf + 4, e->cls);
    put32(buf + 8, e->id);
    put32(buf + 12, e->refcon);
    put32(buf + 16, e->options);
    put32(buf + 20, (uint32_t)e->sender_pid);
    put32(buf + 24, e->sender_sig);
    buf[28] = (uint8_t)(e->modifiers >> 8), buf[29] = (uint8_t)e->modifiers;
    buf[30] = e->receipt;
    memcpy(buf + 32, e->sender_name, 32);
    if (len)
        memcpy(buf + RAW_HEAD, msg, len);
    *len_out = RAW_HEAD + len;
    return buf;
}

static void queue_raw(const uint8_t *data, uint32_t len)
{
    pending_ae e = { .cls = rd32be(data + 4), .id = rd32be(data + 8), .raw = true,
                     .refcon = rd32be(data + 12), .options = rd32be(data + 16),
                     .sender_pid = (int)rd32be(data + 20), .sender_sig = rd32be(data + 24),
                     .modifiers = (uint16_t)(data[28] << 8 | data[29]), .receipt = data[30] != 0 };
    memcpy(e.sender_name, data + 32, 32);
    e.sender_name[0] = e.sender_name[0] > 31 ? 31 : e.sender_name[0];
    e.flatlen = len - RAW_HEAD;
    e.flat = malloc(e.flatlen ? e.flatlen : 1); /* never NULL: an empty message is still an event */
    memcpy(e.flat, data + RAW_HEAD, e.flatlen);
    enqueue(&e);
}

/* An event from another process, flattened (aem_flatten), or a raw
 * high-level event (raw_flatten). */
void aem_queue_flat(const uint8_t *data, uint32_t len, machine_ae_reply_fn reply, void *ctx)
{
    if (len >= RAW_HEAD && rd32be(data) == RAW_TAG) {
        queue_raw(data, len);
        if (reply) /* nothing answers a raw event */
            reply(ctx, NULL, 0, noErr);
        return;
    }
    if (len < 12 || rd32be(data) != RECO_TAG) {
        if (reply)
            reply(ctx, NULL, 0, -1704);
        return;
    }
    uint32_t cls = 0, id = 0;
    /* The class and ID, for the event record's message and where. */
    uint32_t na = rd32be(data + 4), off = 12;
    for (uint32_t i = 0; i < na && off + 12 <= len; i++) {
        uint32_t key = rd32be(data + off), elen = rd32be(data + off + 8);
        if (elen == 4 && off + 16 <= len) {
            if (key == T("evcl"))
                cls = rd32be(data + off + 12);
            else if (key == T("evid"))
                id = rd32be(data + off + 12);
        }
        off += 12 + ((elen + 1) & ~1u);
    }
    pending_ae e = { cls, id, malloc(len), len, reply, ctx };
    memcpy(e.flat, data, len);
    enqueue(&e);
}

/* Ask the app to quit as the System 7 Finder did: a Quit Application event
 * if it takes high-level events, otherwise its own Quit menu item.  One
 * whose SIZE says it takes them but that installed handlers for others
 * and none for Quit Application couldn't act on it: a Director 4
 * projector (Total Distortion) handles only Do Script and Evaluate, and
 * reads no events through WaitNextEvent while a movie plays, but quits
 * on Command-Q. */
static bool has_handler(uint32_t cls, uint32_t id);
static bool app_has_handlers(void);

void machine_request_quit(void)
{
    bool aware = has_handler(T("aevt"), T("quit")) || ((g_app_size_flags & 0x0040) && !app_has_handlers());
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

/* For the Event Manager: take the next queued event (true if one was
 * waiting), with the modifiers a return receipt carries (else -1). */

bool aem_next_event(uint32_t *cls, uint32_t *id, int32_t *modifiers, bool remove)
{
    /* A raw event the program asked for the next one without accepting is
     * gone, its sender told so (perhaps itself, in time for this call) if
     * it wanted a receipt. */
    pthread_mutex_lock(&lock);
    if (remove && current.raw)
        free_event(&current, false);
    pthread_mutex_unlock(&lock);
    send_receipts();
    pthread_mutex_lock(&lock);
    /* High-level events go to apps that say they take them (SIZE) or that
     * installed a handler.  Others got their documents at launch
     * (AppParmHandle), as with the System 7 Finder; drop the rest.  A
     * program that posts itself events takes them, whatever its SIZE says. */
    bool aware = (g_app_size_flags & 0x0040) != 0;
    while (qn > 0 && !aware && !(queue[0].raw ? queue[0].self : has_handler(queue[0].cls, queue[0].id))) {
        free_event(&queue[0], true);
        memmove(queue, queue + 1, (size_t)(qn - 1) * sizeof *queue);
        qn--;
    }
    bool have = qn > 0;
    if (have) {
        *cls = queue[0].cls;
        *id = queue[0].id;
        if (modifiers)
            *modifiers = queue[0].receipt ? queue[0].modifiers : -1;
        if (remove) {
            free_event(&current, true);
            current = queue[0];
            memmove(queue, queue + 1, (size_t)(qn - 1) * sizeof *queue);
            qn--;
        }
    }
    pthread_mutex_unlock(&lock);
    send_receipts();
    return have;
}

static void forget_suspended(void);

void aem_reset(void)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < qn; i++)
        free_event(&queue[i], true);
    qn = 0;
    free_event(&current, true);
    pthread_mutex_unlock(&lock);
    pthread_mutex_lock(&outbox_lock);
    nout = 0; /* the program that was owed them is gone */
    pthread_mutex_unlock(&outbox_lock);
    forget_suspended();
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

static gaddr new_handle_with(const void *data, uint32_t n)
{
    gaddr h = tb_NewHandle((int32_t)n, false, false);
    if (h && data && n)
        gput(rd32(h), data, n);
    return h;
}

/* A copy of guest bytes in a new handle (the source may be in a handle
 * that moves when the new one is made, so it goes through the host). */
static gaddr handle_from_guest(gaddr src, uint32_t n)
{
    uint8_t *tmp = malloc(n ? n : 1);
    if (n)
        gget(tmp, src, n);
    gaddr h = new_handle_with(tmp, n);
    free(tmp);
    return h;
}

enum { PLAIN, LIST, RECORD };

/* What a descriptor's data holds. */
static int shape(gaddr h)
{
    if (!h || !rd32(h))
        return PLAIN;
    int32_t n = tb_GetHandleSize(h);
    uint32_t tag = n >= 8 ? rd32(rd32(h)) : 0;
    if (tag == LIST_TAG)
        return LIST;
    return tag == RECO_TAG && n >= 12 ? RECORD : PLAIN;
}

static gaddr new_container(bool record)
{
    gaddr h = tb_NewHandle(record ? 12 : 8, false, true);
    if (h)
        wr32(rd32(h), record ? RECO_TAG : LIST_TAG);
    return h;
}

/* The offset of the first entry (attributes or parameters) and how many. */
static uint32_t first_entry(gaddr h, uint32_t *count, bool attrs)
{
    gaddr p = rd32(h);
    if (shape(h) == RECORD) {
        uint32_t na = rd32(p + 4), np = rd32(p + 8);
        uint32_t off = 12;
        if (!attrs) { /* skip the attributes */
            for (uint32_t i = 0; i < na; i++)
                off += 12 + pad(rd32(p + off + 8));
        }
        *count = attrs ? na : np;
        return off;
    }
    *count = shape(h) == LIST ? rd32(p + 4) : 0;
    return 8;
}

/* Entry walking. Returns offset of the entry with keyword (or index), 0 if none. */
static uint32_t find_entry(gaddr h, uint32_t key, bool attrs)
{
    if (shape(h) != RECORD)
        return 0;
    uint32_t n, off = first_entry(h, &n, attrs);
    gaddr p = rd32(h);
    for (uint32_t i = 0; i < n; i++) {
        if (rd32(p + off) == key)
            return off;
        off += 12 + pad(rd32(p + off + 8));
    }
    return 0;
}

static uint32_t nth_entry(gaddr h, int32_t index)
{
    if (shape(h) == PLAIN)
        return 0;
    uint32_t n, off = first_entry(h, &n, false);
    gaddr p = rd32(h);
    if (index < 1 || (uint32_t)index > n)
        return 0;
    for (int32_t i = 1; i < index; i++)
        off += 12 + pad(rd32(p + off + 8));
    return off;
}

static void bump_count(gaddr h, bool attr, int delta)
{
    gaddr p = rd32(h);
    gaddr at = p + (shape(h) == RECORD ? (attr ? 4 : 8) : 4);
    wr32(at, rd32(at) + (uint32_t)delta);
}

/* Insert an entry at `at` (an entry's offset, or the end of its part);
 * its data from the guest (data) or the host (host_data). */
static bool insert_entry(gaddr h, uint32_t at, bool attr, uint32_t key, uint32_t etype, gaddr data, uint32_t len,
                         const void *host_data)
{
    uint8_t *tmp = NULL;
    if (!host_data && len) { /* data may live in a handle that moves */
        tmp = malloc(len);
        gget(tmp, data, len);
        host_data = tmp;
    }
    int32_t size = tb_GetHandleSize(h);
    uint32_t entry = 12 + pad(len);
    tb_SetHandleSize(h, size + (int32_t)entry);
    if (tb_GetHandleSize(h) != size + (int32_t)entry) {
        free(tmp);
        return false;
    }
    gaddr p = rd32(h);
    gmemcpy(p + at + entry, p + at, (uint32_t)size - at);
    wr32(p + at, key);
    wr32(p + at + 4, etype);
    wr32(p + at + 8, len);
    if (len)
        gput(p + at + 12, host_data, len);
    if (pad(len) != len)
        wr8(p + at + 12 + len, 0);
    bump_count(h, attr, 1);
    free(tmp);
    return true;
}

/* The end of the attributes (attr) or of everything. */
static uint32_t end_of(gaddr h, bool attr)
{
    if (attr) {
        uint32_t n, off = first_entry(h, &n, true);
        for (uint32_t i = 0; i < n; i++)
            off += 12 + pad(rd32(rd32(h) + off + 8));
        return off;
    }
    return (uint32_t)tb_GetHandleSize(h);
}

static bool append_entry(gaddr h, bool attr, uint32_t key, uint32_t etype, gaddr data, uint32_t len,
                         const void *host_data)
{
    return insert_entry(h, end_of(h, attr), attr, key, etype, data, len, host_data);
}

static void remove_entry(gaddr h, uint32_t off, bool attr)
{
    gaddr p = rd32(h);
    uint32_t len = 12 + pad(rd32(p + off + 8));
    int32_t size = tb_GetHandleSize(h);
    gmemcpy(p + off, p + off + len, (uint32_t)size - off - len);
    bump_count(h, attr, -1);
    tb_SetHandleSize(h, size - (int32_t)len);
}

static uint32_t attr_long(gaddr evt, uint32_t key)
{
    gaddr h = desc_handle(evt);
    uint32_t off = find_entry(h, key, true);
    return off ? rd32(rd32(h) + off + 12) : 0;
}

/* ---------------------------------------------------------------------- */
/* Coercion */

/* The system's built-in coercions between plain data: numbers (shor, long,
 * magn, bool, sing, doub), the four-character types (enum, type, keyw),
 * Boolean and the enumerators true and false, and text to and from all of
 * these.  `len` bytes of `type` at src as `desired`, into out; the length,
 * or -1. */
static int coerce_bytes(uint32_t type, const uint8_t *src, uint32_t len, uint32_t desired, uint8_t out[64])
{
    bool code_in = type == T("enum") || type == T("type") || type == T("keyw");
    bool code_out = desired == T("enum") || desired == T("type") || desired == T("keyw");
    if (type == T("TEXT") && code_out) {
        if (len != 4)
            return -1;
        memcpy(out, src, 4);
        return 4;
    }
    if (code_in && len == 4 && (code_out || desired == T("TEXT"))) {
        memcpy(out, src, 4);
        return 4;
    }
    uint32_t code = len == 4 ? (uint32_t)src[0] << 24 | (uint32_t)src[1] << 16 | (uint32_t)src[2] << 8 | src[3] : 0;
    if (type == T("bool") && len >= 1 && desired == T("enum")) {
        memcpy(out, src[0] ? "true" : "fals", 4);
        return 4;
    }
    if (type == T("enum") && desired == T("bool") && (code == T("true") || code == T("fals"))) {
        out[0] = code == T("true");
        return 1;
    }
    double v;
    if (type == T("shor") && len == 2)
        v = (int16_t)(src[0] << 8 | src[1]);
    else if ((type == T("long") || type == T("magn")) && len == 4)
        v = type == T("long") ? (double)(int32_t)code : (double)code;
    else if (type == T("bool") && len >= 1)
        v = src[0] != 0;
    else if (type == T("sing") && len == 4) {
        float f;
        memcpy(&f, &code, 4);
        v = f;
    } else if (type == T("doub") && len == 8) {
        uint64_t b = 0;
        for (int i = 0; i < 8; i++)
            b = b << 8 | src[i];
        memcpy(&v, &b, 8);
    } else if (type == T("TEXT") && len > 0 && len < 64) {
        char s[64], *end;
        memcpy(s, src, len);
        s[len] = 0;
        v = strtod(s, &end);
        while (*end == ' ')
            end++;
        if (end == s || *end || isnan(v))
            return -1;
    } else
        return -1;
    if (desired == T("TEXT")) {
        if (type == T("bool"))
            return snprintf((char *)out, 64, "%s", v ? "true" : "false");
        if (v == (double)(int64_t)v && fabs(v) < 1e15)
            return snprintf((char *)out, 64, "%lld", (long long)v);
        return snprintf((char *)out, 64, "%.15g", v);
    }
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

/* Coercion handlers, the application's [0] and the system's [1]. */
typedef struct {
    uint32_t from, to;
    gaddr handler;
    int32_t refcon;
    bool from_desc;
} coercion_entry;

static coercion_entry coercions[2][32];
static int ncoercions[2];

static const coercion_entry *find_coercion(uint32_t from, uint32_t to)
{
    const uint32_t any = T("****");
    const uint32_t tries[4][2] = { { from, to }, { from, any }, { any, to }, { any, any } };
    for (int sys = 0; sys < 2; sys++)
        for (int t = 0; t < 4; t++)
            for (int i = 0; i < ncoercions[sys]; i++)
                if (coercions[sys][i].from == tries[t][0] && coercions[sys][i].to == tries[t][1])
                    return &coercions[sys][i];
    return NULL;
}

/* A universal procedure pointer: a routine descriptor, 68k code, or (from
 * PowerPC code that skipped NewRoutineDescriptor) a transition vector. */
uint32_t aem_call_proc(gaddr upp, uint32_t procinfo, const uint32_t *args, int n)
{
    if (!upp)
        return 0;
    if (!mm_is_descriptor(upp) && g_ppc_mode)
        return ppc_call(upp, args, n);
    return mm_call(upp, procinfo, args, n);
}

static int16_t coerce_to_app_parameters(gaddr evt, gaddr desc_out);

static int16_t builtin_coercion(gaddr desc, uint32_t to, gaddr out)
{
    uint32_t from = desc_type(desc);
    gaddr h = desc_handle(desc);
    if (from == T("aevt") && to == T("appa"))
        return coerce_to_app_parameters(desc, out);
    if (to == T("list")) { /* a list of the one item */
        gaddr l = new_container(false);
        if (!l)
            return memFullErr;
        int32_t n = h ? tb_GetHandleSize(h) : 0;
        append_entry(l, false, 0, from, h ? rd32(h) : 0, (uint32_t)n, NULL);
        set_desc(out, to, l);
        return noErr;
    }
    if (shape(h) == RECORD) { /* any record is any record type */
        gaddr nh = handle_from_guest(rd32(h), (uint32_t)tb_GetHandleSize(h));
        set_desc(out, to, nh);
        return nh ? noErr : memFullErr;
    }
    if (!h || !rd32(h))
        return errAECoercionFail;
    if (from == T("alis") && to == T("fss ")) {
        gaddr spec = tb_NewHandle(FSSpec_SIZEOF, false, true), changed = tb_NewPtr(2, false, true);
        tb_HLock(spec);
        int16_t err = tb_ResolveAlias(0, h, rd32(spec), changed);
        tb_HUnlock(spec);
        tb_DisposePtr(changed);
        if (err) {
            tb_DisposeHandle(spec);
            return err;
        }
        set_desc(out, to, spec);
        return noErr;
    }
    if (from == T("fss ") && to == T("alis")) {
        gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true), ah = tb_NewPtr(4, false, true);
        gmemcpy(spec, rd32(h), FSSpec_SIZEOF);
        int16_t err = tb_NewAlias(0, spec, ah);
        gaddr alias = rd32(ah);
        tb_DisposePtr(spec);
        tb_DisposePtr(ah);
        if (err)
            return err;
        set_desc(out, to, alias);
        return noErr;
    }
    uint32_t len = (uint32_t)tb_GetHandleSize(h);
    if (len > 64)
        return errAECoercionFail;
    uint8_t in[64], buf[64];
    gget(in, rd32(h), len);
    int n = coerce_bytes(from, in, len, to, buf);
    if (n < 0)
        return errAECoercionFail;
    gaddr nh = new_handle_with(buf, (uint32_t)n);
    set_desc(out, to, nh);
    return nh ? noErr : memFullErr;
}

/* Coerce a descriptor: the same type copies; then the application's and
 * the system's coercion handlers; then the built-in coercions. */
static int16_t coerce_desc(gaddr desc, uint32_t to, gaddr out)
{
    uint32_t from = desc_type(desc);
    set_desc(out, T("null"), 0);
    if (to == from || to == T("****"))
        return tb_AEDuplicateDesc(desc, out);
    const coercion_entry *c = find_coercion(from, to);
    if (c) {
        int16_t err;
        if (c->from_desc) {
            uint32_t args[4] = { desc, to, (uint32_t)c->refcon, out };
            err = (int16_t)aem_call_proc(c->handler, 0x00003FE0, args, 4);
        } else {
            gaddr h = desc_handle(desc);
            int8_t state = h ? tb_HGetState(h) : 0;
            if (h)
                tb_HLock(h);
            uint32_t args[6] = { from, h ? rd32(h) : 0, h ? (uint32_t)tb_GetHandleSize(h) : 0, to,
                                 (uint32_t)c->refcon, out };
            err = (int16_t)aem_call_proc(c->handler, 0x0003FFE0, args, 6);
            if (h)
                tb_HSetState(h, state);
        }
        return err;
    }
    return builtin_coercion(desc, to, out);
}

/* Copy an entry's data out to (data, max), or into a new descriptor,
 * coerced as asked. */
static int16_t entry_to_desc(gaddr h, uint32_t off, uint32_t desired, gaddr desc_out)
{
    gaddr p = rd32(h);
    uint32_t type = rd32(p + off + 4), len = rd32(p + off + 8);
    gaddr nh = handle_from_guest(p + off + 12, len);
    if (!nh)
        return memFullErr;
    if (desired == T("****") || desired == type) {
        set_desc(desc_out, type, nh);
        return noErr;
    }
    gaddr tmp = tb_NewPtr(AEDesc_SIZEOF, false, true);
    set_desc(tmp, type, nh);
    int16_t err = coerce_desc(tmp, desired, desc_out);
    tb_AEDisposeDesc(tmp);
    tb_DisposePtr(tmp);
    return err;
}

static int16_t entry_to_ptr(gaddr h, uint32_t off, uint32_t desired, gaddr type_out, gaddr data, int32_t max,
                            gaddr size_out)
{
    gaddr p = rd32(h);
    uint32_t type = rd32(p + off + 4), len = rd32(p + off + 8);
    if (desired != T("****") && desired != type) {
        gaddr d = tb_NewPtr(AEDesc_SIZEOF, false, true);
        int16_t err = entry_to_desc(h, off, desired, d);
        if (!err) {
            gaddr dh = desc_handle(d);
            uint32_t n = dh ? (uint32_t)tb_GetHandleSize(dh) : 0;
            if (type_out)
                wr32(type_out, desc_type(d));
            if (size_out)
                wr32(size_out, n);
            if (data && max > 0 && n)
                gmemcpy(data, rd32(dh), n < (uint32_t)max ? n : (uint32_t)max);
        }
        tb_AEDisposeDesc(d);
        tb_DisposePtr(d);
        return err;
    }
    if (type_out)
        wr32(type_out, type);
    if (size_out)
        wr32(size_out, len);
    if (data && max > 0)
        gmemcpy(data, p + off + 12, len < (uint32_t)max ? len : (uint32_t)max);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Descriptor calls */

int16_t tb_AECreateDesc(int32_t type, gaddr data, int32_t data_size, gaddr desc_out)
{
    gaddr h;
    if (data_size == 0 && ((uint32_t)type == T("list") || (uint32_t)type == T("reco") || (uint32_t)type == T("aevt")))
        h = new_container((uint32_t)type != T("list")); /* an empty list or record */
    else
        h = handle_from_guest(data, (uint32_t)data_size);
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
    uint32_t type = desc_type(src);
    if (!h || !rd32(h)) {
        set_desc(dst, type, 0);
        return noErr;
    }
    gaddr nh = handle_from_guest(rd32(h), (uint32_t)tb_GetHandleSize(h));
    set_desc(dst, type, nh);
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
    uint32_t cls = attr_long(evt, T("evcl")), id = attr_long(evt, T("evid"));
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

/* Host paths of an alias, FSSpec or list of them (in a handle, at the
 * entry at off). */
bool alias_host_of_record(gaddr rec, uint32_t len, char *host, size_t hostsize);

static int item_paths(gaddr h, uint32_t off, char ***paths, int count)
{
    gaddr p = rd32(h) + off;
    uint32_t type = rd32(p + 4), len = rd32(p + 8);
    gaddr data = p + 12;
    if (len >= 8 && rd32(data) == LIST_TAG) {
        /* Entries after the tag and count; resolving an alias allocates,
         * so offsets, not addresses, are kept. */
        uint32_t items = rd32(data + 4), at = off + 20;
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

/* The files an Open Documents event in launch parameters names (host paths,
 * malloc'd); the count, or 0. */
int aem_launch_documents(gaddr params, char ***paths_out)
{
    *paths_out = NULL;
    if (!params || rd16(params) != 23 || rd32(params + 2) != T("aevt") || rd32(params + 10) != T("odoc"))
        return 0;
    uint32_t n = rd32(params + 20);
    gaddr h = handle_from_guest(params + 24, n);
    if (!h)
        return 0;
    tb_HLock(h); /* resolving an alias allocates */
    char **paths = NULL;
    uint32_t off = find_entry(h, T("----"), false);
    int count = off ? item_paths(h, off, &paths, 0) : 0;
    tb_DisposeHandle(h);
    *paths_out = paths;
    return count;
}

int16_t tb_AECoerceDesc(gaddr desc, int32_t result_type, gaddr desc_out)
{
    return coerce_desc(desc, (uint32_t)result_type, desc_out);
}

int16_t tb_AECoercePtr(int32_t data_type, gaddr data, int32_t data_size, int32_t result_type, gaddr desc_out)
{
    gaddr tmp = tb_NewPtr(AEDesc_SIZEOF, false, true);
    int16_t err = tb_AECreateDesc(data_type, data, data_size, tmp);
    if (!err)
        err = coerce_desc(tmp, (uint32_t)result_type, desc_out);
    tb_AEDisposeDesc(tmp);
    tb_DisposePtr(tmp);
    return err;
}

int16_t tb_AECountItems(gaddr list, gaddr count_out)
{
    gaddr h = desc_handle(list);
    if (!h)
        return errAENotAEDesc;
    if (shape(h) == PLAIN)
        return errAEWrongDataType;
    uint32_t n;
    first_entry(h, &n, false);
    wr32(count_out, n);
    return noErr;
}

int16_t tb_AEGetNthPtr(gaddr list, int32_t index, int32_t desired_type, gaddr keyword_out, gaddr type_out, gaddr data,
                       int32_t max_size, gaddr size_out)
{
    gaddr h = desc_handle(list);
    uint32_t off = nth_entry(h, index);
    if (!off)
        return shape(h) == PLAIN ? errAEWrongDataType : errAEBadListItem;
    if (keyword_out)
        wr32(keyword_out, shape(h) == RECORD ? rd32(rd32(h) + off) : T("****"));
    return entry_to_ptr(h, off, (uint32_t)desired_type, type_out, data, max_size, size_out);
}

int16_t tb_AEGetNthDesc(gaddr list, int32_t index, int32_t desired_type, gaddr keyword_out, gaddr desc_out)
{
    gaddr h = desc_handle(list);
    uint32_t off = nth_entry(h, index);
    if (!off) {
        set_desc(desc_out, T("null"), 0);
        return shape(h) == PLAIN ? errAEWrongDataType : errAEBadListItem;
    }
    if (keyword_out)
        wr32(keyword_out, shape(h) == RECORD ? rd32(rd32(h) + off) : T("****"));
    return entry_to_desc(h, off, (uint32_t)desired_type, desc_out);
}

int16_t tb_AEGetParamPtr(gaddr record, int32_t keyword, int32_t desired_type, gaddr type_out, gaddr data,
                         int32_t max_size, gaddr size_out)
{
    gaddr h = desc_handle(record);
    uint32_t off = find_entry(h, (uint32_t)keyword, false);
    if (!off)
        return errAEDescNotFound;
    return entry_to_ptr(h, off, (uint32_t)desired_type, type_out, data, max_size, size_out);
}

int16_t tb_AEGetParamDesc(gaddr record, int32_t keyword, int32_t desired_type, gaddr desc_out)
{
    gaddr h = desc_handle(record);
    uint32_t off = find_entry(h, (uint32_t)keyword, false);
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
    uint32_t off = find_entry(h, (uint32_t)keyword, true);
    if (!off)
        return errAEDescNotFound; /* e.g. keyMissedKeywordAttr: nothing was missed */
    return entry_to_ptr(h, off, (uint32_t)desired_type, type_out, data, max_size, size_out);
}

int16_t tb_AEGetAttributeDesc(gaddr evt, int32_t keyword, int32_t desired_type, gaddr desc_out)
{
    gaddr h = desc_handle(evt);
    uint32_t off = find_entry(h, (uint32_t)keyword, true);
    if (!off) {
        set_desc(desc_out, T("null"), 0);
        return errAEDescNotFound;
    }
    return entry_to_desc(h, off, (uint32_t)desired_type, desc_out);
}

static int16_t put_keyed(gaddr record, bool attr, uint32_t keyword, uint32_t type, gaddr data, uint32_t size,
                         const void *host_data)
{
    gaddr h = desc_handle(record);
    if (!h || !rd32(h))
        return errAENotAEDesc;
    if (shape(h) != RECORD)
        return errAEWrongDataType;
    uint32_t off = find_entry(h, keyword, attr);
    if (off) /* replaced in place */
        remove_entry(h, off, attr);
    else
        off = end_of(h, attr);
    return insert_entry(h, off, attr, keyword, type, data, size, host_data) ? noErr : memFullErr;
}

int16_t tb_AEPutParamPtr(gaddr record, int32_t keyword, int32_t type, gaddr data, int32_t data_size)
{
    return put_keyed(record, false, (uint32_t)keyword, (uint32_t)type, data, (uint32_t)data_size, NULL);
}

int16_t tb_AEPutParamDesc(gaddr record, int32_t keyword, gaddr desc)
{
    gaddr dh = desc_handle(desc);
    int32_t n = dh ? tb_GetHandleSize(dh) : 0;
    return put_keyed(record, false, (uint32_t)keyword, desc_type(desc), dh ? rd32(dh) : 0, (uint32_t)n, NULL);
}

/* ---------------------------------------------------------------------- */
/* Handlers: the application's [0] and the system's [1] */

typedef struct {
    uint32_t cls, id;
    gaddr handler;
    int32_t refcon;
} handler_entry;

static handler_entry handlers[2][64];
static int nhandlers[2];
static uint8_t interaction = kAEInteractWithLocal;
static gaddr current_event_desc;

int16_t tb_AEInstallEventHandler(int32_t event_class, int32_t event_id, gaddr hdlr, int32_t refcon, int8_t system_handler_p)
{
    int s = system_handler_p != 0;
    for (int i = 0; i < nhandlers[s]; i++)
        if (handlers[s][i].cls == (uint32_t)event_class && handlers[s][i].id == (uint32_t)event_id) {
            handlers[s][i].handler = hdlr;
            handlers[s][i].refcon = refcon;
            return noErr;
        }
    if (nhandlers[s] >= 64)
        return memFullErr;
    handlers[s][nhandlers[s]++] = (handler_entry){ (uint32_t)event_class, (uint32_t)event_id, hdlr, refcon };
    return noErr;
}

int16_t tb_AERemoveEventHandler(int32_t event_class, int32_t event_id, gaddr hdlr, int8_t system_handler_p)
{
    int s = system_handler_p != 0;
    for (int i = 0; i < nhandlers[s]; i++)
        if (handlers[s][i].cls == (uint32_t)event_class && handlers[s][i].id == (uint32_t)event_id) {
            handlers[s][i] = handlers[s][--nhandlers[s]];
            return noErr;
        }
    return errAEHandlerNotFound;
}

int16_t tb_AEGetEventHandler(int32_t event_class, int32_t event_id, gaddr hdlr, gaddr refcon, int8_t system_handler_p)
{
    int s = system_handler_p != 0;
    for (int i = 0; i < nhandlers[s]; i++)
        if (handlers[s][i].cls == (uint32_t)event_class && handlers[s][i].id == (uint32_t)event_id) {
            wr32(hdlr, handlers[s][i].handler);
            wr32(refcon, (uint32_t)handlers[s][i].refcon);
            return noErr;
        }
    return errAEHandlerNotFound;
}

/* The handler for an event in one table: exact, then wildcards. */
static const handler_entry *find_handler_in(int s, uint32_t cls, uint32_t id)
{
    const handler_entry *best = NULL;
    for (int i = 0; i < nhandlers[s]; i++) {
        bool c = handlers[s][i].cls == cls || handlers[s][i].cls == T("****");
        bool d = handlers[s][i].id == id || handlers[s][i].id == T("****");
        if (c && d && (!best || (handlers[s][i].cls == cls && handlers[s][i].id == id)))
            best = &handlers[s][i];
    }
    return best;
}

static bool has_handler(uint32_t cls, uint32_t id)
{
    return find_handler_in(0, cls, id) || find_handler_in(1, cls, id);
}

static bool app_has_handlers(void) { return nhandlers[0] > 0 || nhandlers[1] > 0; }

/* ---------------------------------------------------------------------- */
/* Coercion and special handlers */

int16_t tb_AEInstallCoercionHandler(int32_t from, int32_t to, gaddr hdlr, int32_t refcon, int8_t from_desc, int8_t sys)
{
    int s = sys != 0;
    coercion_entry e = { (uint32_t)from, (uint32_t)to, hdlr, refcon, from_desc != 0 };
    for (int i = 0; i < ncoercions[s]; i++)
        if (coercions[s][i].from == (uint32_t)from && coercions[s][i].to == (uint32_t)to) {
            coercions[s][i] = e;
            return noErr;
        }
    if (ncoercions[s] >= 32)
        return memFullErr;
    coercions[s][ncoercions[s]++] = e;
    return noErr;
}

int16_t tb_AERemoveCoercionHandler(int32_t from, int32_t to, gaddr hdlr, int8_t sys)
{
    int s = sys != 0;
    for (int i = 0; i < ncoercions[s]; i++)
        if (coercions[s][i].from == (uint32_t)from && coercions[s][i].to == (uint32_t)to) {
            coercions[s][i] = coercions[s][--ncoercions[s]];
            return noErr;
        }
    return errAEHandlerNotFound;
}

int16_t tb_AEGetCoercionHandler(int32_t from, int32_t to, gaddr hdlr_out, gaddr refcon_out, gaddr from_desc_out,
                                int8_t sys)
{
    int s = sys != 0;
    for (int i = 0; i < ncoercions[s]; i++)
        if (coercions[s][i].from == (uint32_t)from && coercions[s][i].to == (uint32_t)to) {
            if (hdlr_out)
                wr32(hdlr_out, coercions[s][i].handler);
            if (refcon_out)
                wr32(refcon_out, (uint32_t)coercions[s][i].refcon);
            if (from_desc_out)
                wr8(from_desc_out, coercions[s][i].from_desc);
            return noErr;
        }
    return errAEHandlerNotFound;
}

/* Special handlers: the predispatch handler ('phac'), and the Object
 * Support Library's callbacks, which AESetObjectCallbacks installs here. */
static struct {
    uint32_t cls;
    gaddr handler;
} specials[2][16];
static int nspecials[2];

int16_t tb_AEInstallSpecialHandler(int32_t cls, gaddr hdlr, int8_t sys)
{
    int s = sys != 0;
    for (int i = 0; i < nspecials[s]; i++)
        if (specials[s][i].cls == (uint32_t)cls) {
            specials[s][i].handler = hdlr;
            return noErr;
        }
    if (nspecials[s] >= 16)
        return memFullErr;
    specials[s][nspecials[s]].cls = (uint32_t)cls;
    specials[s][nspecials[s]++].handler = hdlr;
    return noErr;
}

int16_t tb_AERemoveSpecialHandler(int32_t cls, gaddr hdlr, int8_t sys)
{
    int s = sys != 0;
    for (int i = 0; i < nspecials[s]; i++)
        if (specials[s][i].cls == (uint32_t)cls) {
            specials[s][i] = specials[s][--nspecials[s]];
            return noErr;
        }
    return errAEHandlerNotFound;
}

int16_t tb_AEGetSpecialHandler(int32_t cls, gaddr hdlr_out, int8_t sys)
{
    int s = sys != 0;
    for (int i = 0; i < nspecials[s]; i++)
        if (specials[s][i].cls == (uint32_t)cls) {
            wr32(hdlr_out, specials[s][i].handler);
            return noErr;
        }
    return errAEHandlerNotFound;
}

/* The application's special handler for key, else the system's; 0 if none. */
gaddr aem_special_handler(uint32_t key)
{
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < nspecials[s]; i++)
            if (specials[s][i].cls == key)
                return specials[s][i].handler;
    return 0;
}

/* AEManagerInfo: the number of Apple Events processed and the like. */
int16_t tb_AEManagerInfo(gaddr result)
{
    if (result)
        wr32(result, 0);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Dispatching */

static int16_t call_handler(gaddr upp, gaddr evt, gaddr reply, int32_t refcon)
{
    uint32_t args[3] = { evt, reply, (uint32_t)refcon };
    return (int16_t)aem_call_proc(upp, 0x00000FE0 /* pascal OSErr (ptr, ptr, long) */, args, 3);
}

/* As the Apple Event Manager does: the predispatch handler, then the
 * application's handler, then the system's (also when the application's
 * declines with errAEEventNotHandled). */
static int16_t dispatch(gaddr evt, gaddr reply)
{
    gaddr saved = current_event_desc;
    current_event_desc = evt;
    uint32_t cls = attr_long(evt, T("evcl")), id = attr_long(evt, T("evid"));
    int16_t result = errAEEventNotHandled;
    gaddr pre = aem_special_handler(T("phac"));
    if (pre)
        result = call_handler(pre, evt, reply, 0);
    for (int s = 0; s < 2 && result == errAEEventNotHandled; s++) {
        const handler_entry *hd = find_handler_in(s, cls, id);
        if (hd)
            result = call_handler(hd->handler, evt, reply, hd->refcon);
    }
    current_event_desc = saved;
    return result;
}

/* A reply for an event: an Answer event with its return ID and
 * transaction, or a null descriptor when no reply was asked for. */
static void make_reply(gaddr evt, gaddr reply, bool wanted)
{
    set_desc(reply, T("null"), 0);
    if (!wanted)
        return;
    gaddr h = new_container(true);
    if (!h)
        return;
    set_desc(reply, T("aevt"), h);
    uint32_t cls = __builtin_bswap32(T("aevt")), id = __builtin_bswap32(T("ansr"));
    append_entry(h, true, T("evcl"), T("type"), 0, 4, &cls);
    append_entry(h, true, T("evid"), T("type"), 0, 4, &id);
    gaddr eh = desc_handle(evt);
    static const char *const copied[] = { "rtid", "tran" };
    for (int i = 0; i < 2; i++) {
        uint32_t off = find_entry(eh, T(copied[i]), true);
        if (off) {
            gaddr p = rd32(eh) + off;
            uint32_t len = rd32(p + 8);
            uint8_t buf[8] = { 0 };
            gget(buf, p + 12, len > 8 ? 8 : len);
            append_entry(h, true, rd32(p), rd32(p + 4), 0, len > 8 ? 8 : len, buf);
        }
    }
}

/* A handler's error goes into the reply (keyErrorNumber), unless the
 * handler put one there itself. */
static void note_error(gaddr reply, int16_t err)
{
    gaddr h = desc_handle(reply);
    if (!err || desc_type(reply) != T("aevt") || shape(h) != RECORD || find_entry(h, T("errn"), false))
        return;
    uint32_t v = __builtin_bswap32((uint32_t)(int32_t)err);
    put_keyed(reply, false, T("errn"), T("long"), 0, 4, &v);
}

/* Events whose handler called AESuspendTheCurrentEvent, by their data
 * handle, with whoever waits for the reply. */
typedef struct {
    gaddr data;
    machine_ae_reply_fn reply;
    void *ctx;
} suspended_ae;

static suspended_ae suspended[16];
static int nsuspended;
static gaddr suspending; /* the data handle of the event just suspended */

static void forget_suspended(void)
{
    for (int i = 0; i < nsuspended; i++)
        if (suspended[i].reply)
            suspended[i].reply(suspended[i].ctx, NULL, 0, errAEEventNotHandled);
    nsuspended = 0;
    suspending = 0;
}

/* The flattened form of a descriptor's data, for another process: as we
 * keep it, with files as host paths ('bbfp', where FSSpecs and aliases
 * mean nothing to the other side).  malloc'd. */
static uint8_t *flatten_data(gaddr h, uint32_t *len_out);

/* Done with an event: its reply to the process that waits for it, and both
 * disposed of. */
static void finish(gaddr evt, gaddr reply, int16_t result, machine_ae_reply_fn reply_fn, void *ctx)
{
    note_error(reply, result);
    if (reply_fn) {
        uint32_t len = 0;
        uint8_t *data = desc_handle(reply) ? flatten_data(desc_handle(reply), &len) : NULL;
        reply_fn(ctx, data, len, result);
        free(data);
    }
    tb_AEDisposeDesc(evt);
    tb_AEDisposeDesc(reply);
}

/* Dispatch an event received; true if it was suspended (and so not done). */
static int16_t receive(gaddr evt, gaddr reply, machine_ae_reply_fn reply_fn, void *ctx)
{
    suspending = 0;
    int16_t result = dispatch(evt, reply);
    if (suspending && suspending == desc_handle(evt) && nsuspended < 16) {
        suspended[nsuspended++] = (suspended_ae){ suspending, reply_fn, ctx };
        suspending = 0;
        return result;
    }
    suspending = 0;
    finish(evt, reply, result, reply_fn, ctx);
    return result;
}

static gaddr unflatten_data(const uint8_t *data, uint32_t len);

/* Build an event delivered to the app as an AppleEvent descriptor. */
static void build_event(const pending_ae *e, gaddr desc)
{
    set_desc(desc, T("aevt"), unflatten_data(e->flat, e->flatlen));
}

int16_t tb_AEProcessAppleEvent(gaddr evt)
{
    pthread_mutex_lock(&lock);
    pending_ae e = current;
    /* A raw high-level event is no Apple Event: it stays for
     * AcceptHighLevelEvent. */
    if (e.raw)
        e.flat = NULL;
    else
        memset(&current, 0, sizeof current);
    pthread_mutex_unlock(&lock);
    if (!e.flat)
        return errAEEventNotHandled;
    gaddr event = tb_NewPtr(AEDesc_SIZEOF, false, true), reply = tb_NewPtr(AEDesc_SIZEOF, false, true);
    build_event(&e, event);
    /* The sender says in the event whether it wants the reply. */
    bool wanted = e.reply != NULL && (attr_long(event, T("repq")) >> 24) != 0;
    make_reply(event, reply, wanted);
    int16_t result = receive(event, reply, e.reply, e.reply_ctx);
    e.reply = NULL; /* answered, or waiting in the suspended list */
    tb_DisposePtr(event);
    tb_DisposePtr(reply);
    free_event(&e, false);
    return result;
}

int16_t tb_AEGetTheCurrentEvent(gaddr return_evt)
{
    if (current_event_desc)
        gmemcpy(return_evt, current_event_desc, AEDesc_SIZEOF);
    else
        set_desc(return_evt, T("null"), 0);
    return noErr;
}

int16_t tb_AESetTheCurrentEvent(gaddr evt)
{
    current_event_desc = evt;
    return noErr;
}

/* The handler keeps the event (and its reply) to finish later with
 * AEResumeTheCurrentEvent: nothing is disposed of or answered yet. */
int16_t tb_AESuspendTheCurrentEvent(gaddr evt)
{
    suspending = evt ? desc_handle(evt) : 0;
    return noErr;
}

/* kAEUseStandardDispatch (-1) dispatches the event as usual, kAENoDispatch
 * (0) takes it as handled, and anything else is an event handler to call;
 * then the reply goes back and both are disposed of, as for any event. */
int16_t tb_AEResumeTheCurrentEvent(gaddr evt, gaddr reply, gaddr dispatcher, int32_t refcon)
{
    suspended_ae s = { 0 };
    for (int i = 0; i < nsuspended; i++)
        if (suspended[i].data == desc_handle(evt)) {
            s = suspended[i];
            suspended[i] = suspended[--nsuspended];
            break;
        }
    int16_t result = noErr;
    suspending = 0;
    if (dispatcher == 0xFFFFFFFF)
        result = dispatch(evt, reply);
    else if (dispatcher) {
        gaddr saved = current_event_desc;
        current_event_desc = evt;
        result = call_handler(dispatcher, evt, reply, refcon);
        current_event_desc = saved;
    }
    if (suspending && suspending == desc_handle(evt) && nsuspended < 16) { /* suspended again */
        suspended[nsuspended++] = (suspended_ae){ suspending, s.reply, s.ctx };
        suspending = 0;
        return noErr;
    }
    suspending = 0;
    finish(evt, reply, result, s.reply, s.ctx);
    return noErr;
}

/* Whether the handler of the current event may talk to the user: not if
 * the sender said never; otherwise as the interaction level allows for
 * where the event came from (kAEInteractWithLocal: anything on this Mac). */
int16_t tb_AEInteractWithUser(int32_t timeout, gaddr nm_req, gaddr idle_proc)
{
    gaddr evt = current_event_desc;
    if (!evt || !desc_handle(evt))
        return noErr;
    uint32_t inte = 0, source = kAESameProcess;
    gaddr h = desc_handle(evt);
    uint32_t off = find_entry(h, T("inte"), true);
    if (off)
        inte = rd8(rd32(h) + off + 12) & 0x30;
    off = find_entry(h, T("esrc"), true);
    if (off)
        source = rd16(rd32(h) + off + 12);
    if (inte == kAENeverInteract)
        return errAENoUserInteraction;
    if (source <= kAESameProcess || inte == kAEAlwaysInteract)
        return noErr;
    if (interaction == kAEInteractWithSelf || (source == kAERemoteProcess && interaction != kAEInteractWithAll))
        return errAENoUserInteraction;
    return noErr;
}

int16_t tb_AESetInteractionAllowed(uint8_t level)
{
    interaction = level;
    return noErr;
}

int16_t tb_AEGetInteractionAllowed(gaddr return_level)
{
    wr8(return_level, interaction);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Building lists, records and events */

int16_t tb_AECreateList(gaddr prefix, int32_t prefix_size, int8_t is_record, gaddr list_out)
{
    gaddr h = new_container(is_record != 0);
    set_desc(list_out, is_record ? T("reco") : T("list"), h);
    return h ? noErr : memFullErr;
}

/* index 0 (or one past the end) appends; otherwise the item is replaced. */
static int16_t put_nth(gaddr list, int32_t index, uint32_t type, gaddr data, uint32_t size)
{
    gaddr h = desc_handle(list);
    if (!h || !rd32(h))
        return errAENotAEDesc;
    if (shape(h) == PLAIN)
        return errAEWrongDataType;
    uint32_t n;
    first_entry(h, &n, false);
    if (index < 0 || (uint32_t)index > n + 1)
        return errAEBadListItem;
    if (index == 0 || (uint32_t)index == n + 1)
        return append_entry(h, false, 0, type, data, size, NULL) ? noErr : memFullErr;
    uint32_t off = nth_entry(h, index);
    uint32_t key = rd32(rd32(h) + off);
    remove_entry(h, off, false);
    return insert_entry(h, off, false, key, type, data, size, NULL) ? noErr : memFullErr;
}

int16_t tb_AEPutPtr(gaddr list, int32_t index, int32_t type, gaddr data, int32_t size)
{
    return put_nth(list, index, (uint32_t)type, data, (uint32_t)size);
}

int16_t tb_AEPutDesc(gaddr list, int32_t index, gaddr desc)
{
    gaddr dh = desc_handle(desc);
    int32_t n = dh ? tb_GetHandleSize(dh) : 0;
    return put_nth(list, index, desc_type(desc), dh ? rd32(dh) : 0, (uint32_t)n);
}

int16_t tb_AEDeleteItem(gaddr list, int32_t index)
{
    gaddr h = desc_handle(list);
    uint32_t off = nth_entry(h, index);
    if (!off)
        return errAEBadListItem;
    remove_entry(h, off, false);
    return noErr;
}

int16_t tb_AEDeleteParam(gaddr record, int32_t keyword)
{
    gaddr h = desc_handle(record);
    uint32_t off = find_entry(h, (uint32_t)keyword, false);
    if (!off)
        return errAEDescNotFound;
    remove_entry(h, off, false);
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
    uint32_t off = nth_entry(h, index);
    return off ? size_of(h, off, type_out, size_out) : errAEBadListItem;
}

int16_t tb_AESizeOfParam(gaddr record, int32_t keyword, gaddr type_out, gaddr size_out)
{
    gaddr h = desc_handle(record);
    return size_of(h, find_entry(h, (uint32_t)keyword, false), type_out, size_out);
}

int16_t tb_AESizeOfAttribute(gaddr evt, int32_t keyword, gaddr type_out, gaddr size_out)
{
    gaddr h = desc_handle(evt);
    return size_of(h, find_entry(h, (uint32_t)keyword, true), type_out, size_out);
}

int16_t tb_AEPutAttributePtr(gaddr evt, int32_t keyword, int32_t type, gaddr data, int32_t size)
{
    if (desc_type(evt) != T("aevt"))
        return errAENotAEDesc;
    return put_keyed(evt, true, (uint32_t)keyword, (uint32_t)type, data, (uint32_t)size, NULL);
}

int16_t tb_AEPutAttributeDesc(gaddr evt, int32_t keyword, gaddr desc)
{
    gaddr dh = desc_handle(desc);
    int32_t n = dh ? tb_GetHandleSize(dh) : 0;
    return tb_AEPutAttributePtr(evt, keyword, (int32_t)desc_type(desc), dh ? rd32(dh) : 0, n);
}

static int16_t next_return_id = 1;

int16_t tb_AECreateAppleEvent(int32_t cls, int32_t id, gaddr target, int16_t return_id, int32_t transaction,
                              gaddr evt)
{
    gaddr h = new_container(true);
    if (!h)
        return memFullErr;
    set_desc(evt, T("aevt"), h);
    if (return_id == -1) /* kAutoGenerateReturnID */
        return_id = next_return_id++ & 0x7FFF;
    uint32_t c = __builtin_bswap32((uint32_t)cls), i = __builtin_bswap32((uint32_t)id);
    uint16_t rid = __builtin_bswap16((uint16_t)return_id);
    uint32_t tid = __builtin_bswap32((uint32_t)transaction);
    append_entry(h, true, T("evcl"), T("type"), 0, 4, &c);
    append_entry(h, true, T("evid"), T("type"), 0, 4, &i);
    append_entry(h, true, T("rtid"), T("shor"), 0, 2, &rid);
    append_entry(h, true, T("tran"), T("long"), 0, 4, &tid);
    if (target && desc_handle(target))
        tb_AEPutAttributeDesc(evt, (int32_t)T("addr"), target);
    return noErr;
}

/* Arrays (AEArrayType): kAEDataArray and kAEPackedArray hold items of one
 * type and size; kAEHandleArray handles; kAEDescArray descriptors;
 * kAEKeyDescArray keyword and descriptor pairs (for records). */
enum { kAEDataArray, kAEPackedArray, kAEHandleArray, kAEDescArray, kAEKeyDescArray };

int16_t tb_AEPutArray(gaddr list, uint8_t type, gaddr data, int32_t item_type, int32_t item_size, int32_t count)
{
    gaddr h = desc_handle(list);
    if (!h || !rd32(h))
        return errAENotAEDesc;
    if (shape(h) == PLAIN || (type == kAEKeyDescArray) != (shape(h) == RECORD))
        return errAEWrongDataType;
    for (int32_t i = 0; i < count; i++) {
        int16_t err;
        switch (type) {
        case kAEDataArray:
        case kAEPackedArray:
            err = put_nth(list, 0, (uint32_t)item_type, data + (uint32_t)(i * item_size), (uint32_t)item_size);
            break;
        case kAEHandleArray: {
            gaddr ih = rd32(data + 4 * (uint32_t)i);
            err = put_nth(list, 0, (uint32_t)item_type, ih ? rd32(ih) : 0, ih ? (uint32_t)tb_GetHandleSize(ih) : 0);
            break;
        }
        case kAEDescArray:
            err = tb_AEPutDesc(list, 0, data + 8 * (uint32_t)i);
            break;
        case kAEKeyDescArray:
            err = tb_AEPutParamDesc(list, (int32_t)rd32(data + 12 * (uint32_t)i), data + 12 * (uint32_t)i + 4);
            break;
        default:
            return paramErr;
        }
        if (err)
            return err;
    }
    return noErr;
}

int16_t tb_AEGetArray(gaddr list, uint8_t array_type, gaddr array_ptr, int32_t max_size, gaddr item_type,
                      gaddr item_size, gaddr item_count)
{
    gaddr h = desc_handle(list);
    if (!h || !rd32(h))
        return errAENotAEDesc;
    if (shape(h) == PLAIN || (array_type == kAEKeyDescArray) != (shape(h) == RECORD))
        return errAEWrongDataType;
    uint32_t n, got = 0, at = 0;
    first_entry(h, &n, false);
    uint32_t want = item_type ? rd32(item_type) : T("****");
    uint32_t size = item_size ? rd32(item_size) : 0;
    for (uint32_t i = 1; i <= n; i++) {
        uint32_t off = nth_entry(h, (int32_t)i);
        gaddr p = rd32(h);
        uint32_t len = rd32(p + off + 8);
        if (array_type == kAEDataArray || array_type == kAEPackedArray) {
            /* The first item's type and size unless the caller named them. */
            if (want == T("****"))
                want = rd32(p + off + 4);
            if (!size)
                size = array_type == kAEDataArray ? pad(len) : len;
            if (at + size > (uint32_t)max_size)
                break;
            gaddr tsize = tb_NewPtr(4, false, true);
            int16_t err = entry_to_ptr(h, off, want, 0, array_ptr + at, (int32_t)size, tsize);
            tb_DisposePtr(tsize);
            if (err)
                return err;
            at += size;
        } else if (array_type == kAEHandleArray) {
            if (at + 4 > (uint32_t)max_size)
                break;
            gaddr d = tb_NewPtr(AEDesc_SIZEOF, false, true);
            int16_t err = entry_to_desc(h, off, want, d);
            if (!err)
                wr32(array_ptr + at, desc_handle(d));
            tb_DisposePtr(d);
            if (err)
                return err;
            at += 4;
        } else if (array_type == kAEDescArray || array_type == kAEKeyDescArray) {
            uint32_t step = array_type == kAEDescArray ? 8 : 12;
            if (at + step > (uint32_t)max_size)
                break;
            gaddr d = array_ptr + at + (step - 8);
            if (step == 12)
                wr32(array_ptr + at, rd32(p + off));
            int16_t err = entry_to_desc(h, off, T("****"), d);
            if (err)
                return err;
            at += step;
        } else
            return paramErr;
        got++;
    }
    if (item_type)
        wr32(item_type, want);
    if (item_size)
        wr32(item_size, size);
    if (item_count)
        wr32(item_count, got);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Sending */

extern uint32_t g_app_creator;

/* Whether an address is this process: none, kCurrentProcess or our serial
 * number, or our signature. */
static bool to_self(gaddr evt)
{
    gaddr h = desc_handle(evt);
    uint32_t off = find_entry(h, T("addr"), true);
    if (!off)
        return true;
    gaddr p = rd32(h);
    uint32_t type = rd32(p + off + 4);
    if (type == T("psn ")) /* other runs are {1, process ID} */
        return rd32(p + off + 12) == 0 && (rd32(p + off + 16) == 2 || rd32(p + off + 16) == 0x1000); /* us */
    if (type == T("sign"))
        return rd32(p + off + 12) == g_app_creator;
    if (type == T("kpid"))
        return rd32(p + off + 12) == (uint32_t)getpid();
    return false;
}

/* ---- The Finder ----
 * The Finder is only a name in the Process Manager's list, so the events
 * applications send it to open things (Open Documents, and the Finder's own
 * Open Selection from file browsers such as Greg's Browser) are carried out
 * here: applications are launched, documents opened with their creator's
 * application. */

static bool to_finder(gaddr evt)
{
    gaddr h = desc_handle(evt);
    uint32_t off = find_entry(h, T("addr"), true);
    if (!off)
        return false;
    gaddr p = rd32(h) + off;
    if (rd32(p + 4) == T("psn ")) /* the serial number the Process Manager lists it under */
        return process_is_finder(rd32(p + 12), rd32(p + 16));
    return rd32(p + 4) == T("sign") && rd32(p + 12) == T("MACS");
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
    uint32_t off = cls == T("FNDR") ? find_entry(h, T("fsel"), false) : 0;
    if (!off)
        off = find_entry(h, T("----"), false);
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

static int16_t send_to_self(gaddr evt, gaddr reply, int32_t send_mode)
{
    int mode = send_mode & 3;
    gaddr r = tb_NewPtr(AEDesc_SIZEOF, false, true);
    make_reply(evt, r, mode != kAENoReply);
    uint16_t source = kAEDirectCall;
    put_keyed(evt, true, T("esrc"), T("shor"), 0, 2, &(uint16_t){ __builtin_bswap16(source) });
    if (!find_handler_in(0, attr_long(evt, T("evcl")), attr_long(evt, T("evid"))) &&
        !find_handler_in(1, attr_long(evt, T("evcl")), attr_long(evt, T("evid"))) && !aem_special_handler(T("phac"))) {
        tb_AEDisposeDesc(r);
        tb_DisposePtr(r);
        return errAEEventNotHandled;
    }
    int16_t result = dispatch(evt, r);
    note_error(r, result);
    if (mode == kAEWaitReply && reply)
        gmemcpy(reply, r, AEDesc_SIZEOF);
    else if (mode == kAEQueueReply && desc_handle(r)) {
        /* The reply comes back later as an Answer event. */
        uint32_t len;
        uint8_t *data = flatten_data(desc_handle(r), &len);
        aem_queue_flat(data, len, NULL, NULL);
        free(data);
        tb_AEDisposeDesc(r);
    } else
        tb_AEDisposeDesc(r);
    tb_DisposePtr(r);
    return result;
}

int16_t tb_AESend(gaddr evt, gaddr reply, int32_t send_mode, int16_t priority, int32_t timeout, gaddr idle,
                  gaddr filter)
{
    if (reply)
        set_desc(reply, T("null"), 0);
    if (desc_type(evt) != T("aevt") || shape(desc_handle(evt)) != RECORD)
        return errAENotAEDesc;
    if (to_finder(evt))
        return finder_event(evt);
    if (to_self(evt))
        return send_to_self(evt, reply, send_mode);
    gaddr h = desc_handle(evt);
    uint32_t off = find_entry(h, T("addr"), true);
    gaddr p = rd32(h) + off;
    uint32_t atype = rd32(p + 4), alen = rd32(p + 8);
    uint8_t addr[64] = { 0 };
    gget(addr, p + 12, alen > 64 ? 64 : alen);
    int mode = send_mode & 3;
    uint8_t repq = mode != kAENoReply, inte = (uint8_t)(send_mode & 0x30);
    put_keyed(evt, true, T("repq"), T("bool"), 0, 1, &repq);
    put_keyed(evt, true, T("inte"), T("byte"), 0, 1, &inte);
    uint32_t len;
    uint8_t *data = flatten_data(h, &len);
    uint8_t *answer = NULL;
    uint32_t answer_len = 0;
    int16_t err = aem_remote_send(addr, atype, alen > 64 ? 64 : alen, data, len, mode, timeout, &answer, &answer_len);
    free(data);
    if (answer && mode == kAEWaitReply && reply) {
        gaddr rh = unflatten_data(answer, answer_len);
        set_desc(reply, T("aevt"), rh);
    }
    free(answer);
    return err;
}

int16_t tb_AEResetTimer(gaddr evt) { return noErr; }

void aem_reset_handlers(void)
{
    for (int s = 0; s < 2; s++)
        nhandlers[s] = ncoercions[s] = nspecials[s] = 0;
    interaction = kAEInteractWithLocal;
    current_event_desc = 0;
    osl_reset();
}

/* ---------------------------------------------------------------------- */
/* Flattening, for other processes */

/* Data at p (len bytes) flattened onto buf at *n.  Lists and records
 * recurse; FSSpecs and aliases become host paths. */
static void flatten_into(gaddr p, uint32_t len, uint8_t **buf, uint32_t *n, uint32_t *cap)
{
    uint32_t tag = len >= 8 ? rd32(p) : 0;
    if ((tag == LIST_TAG || (tag == RECO_TAG && len >= 12))) {
        uint32_t head = tag == LIST_TAG ? 8 : 12;
        uint32_t count = tag == LIST_TAG ? rd32(p + 4) : rd32(p + 4) + rd32(p + 8);
        grow(buf, cap, *n + head);
        gget(*buf + *n, p, head);
        *n += head;
        uint32_t off = head;
        for (uint32_t i = 0; i < count && off + 12 <= len; i++) {
            uint32_t key = rd32(p + off), type = rd32(p + off + 4), elen = rd32(p + off + 8);
            gaddr data = p + off + 12;
            uint32_t at = *n;
            grow(buf, cap, at + 12);
            *n += 12;
            char host[1024];
            bool path = false;
            if (type == T("fss ") && elen >= 70)
                path = fm_host_path((int16_t)rd16(data), (int32_t)rd32(data + 2), data + 6, true, host, sizeof host);
            else if (type == T("alis"))
                path = alias_host_of_record(data, elen, host, sizeof host);
            if (path) {
                /* 'bbfp': the original type, then the path. */
                uint32_t plen = (uint32_t)strlen(host);
                grow(buf, cap, *n + 4 + plen + 1);
                put32(*buf + *n, type);
                memcpy(*buf + *n + 4, host, plen);
                *n += 4 + plen;
                type = T("bbfp");
            } else
                flatten_into(data, elen, buf, n, cap);
            uint32_t flen = *n - at - 12;
            put32(*buf + at, key);
            put32(*buf + at + 4, type);
            put32(*buf + at + 8, flen);
            if (flen & 1) {
                grow(buf, cap, *n + 1);
                (*buf)[(*n)++] = 0;
            }
            off += 12 + pad(elen);
        }
        return;
    }
    grow(buf, cap, *n + len);
    gget(*buf + *n, p, len);
    *n += len;
}

static uint8_t *flatten_data(gaddr h, uint32_t *len_out)
{
    uint8_t *buf = NULL;
    uint32_t n = 0, cap = 0;
    int8_t state = tb_HGetState(h);
    tb_HLock(h); /* aliases resolve with memory allocated */
    flatten_into(rd32(h), (uint32_t)tb_GetHandleSize(h), &buf, &n, &cap);
    tb_HSetState(h, state);
    *len_out = n;
    return buf;
}

uint8_t *aem_flatten(gaddr desc, uint32_t *len_out)
{
    gaddr h = desc_handle(desc);
    *len_out = 0;
    return h && rd32(h) ? flatten_data(h, len_out) : NULL;
}

/* The other way: into a new handle, with host paths made FSSpecs (or
 * aliases) of ours again. */
static void unflatten_into(gaddr h, const uint8_t *p, uint32_t len)
{
    uint32_t tag = len >= 8 ? rd32be(p) : 0;
    int32_t base = tb_GetHandleSize(h);
    if (tag == LIST_TAG || (tag == RECO_TAG && len >= 12)) {
        uint32_t head = tag == LIST_TAG ? 8 : 12;
        uint32_t count = tag == LIST_TAG ? rd32be(p + 4) : rd32be(p + 4) + rd32be(p + 8);
        tb_SetHandleSize(h, base + (int32_t)head);
        gput(rd32(h) + (uint32_t)base, p, head);
        uint32_t off = head;
        for (uint32_t i = 0; i < count && off + 12 <= len; i++) {
            uint32_t key = rd32be(p + off), type = rd32be(p + off + 4), elen = rd32be(p + off + 8);
            const uint8_t *data = p + off + 12;
            if (off + 12 + elen > len)
                break;
            int32_t at = tb_GetHandleSize(h);
            tb_SetHandleSize(h, at + 12);
            if (type == T("bbfp") && elen >= 4) {
                char path[1024];
                uint32_t plen = elen - 4 < sizeof path - 1 ? elen - 4 : (uint32_t)sizeof path - 1;
                memcpy(path, data + 4, plen);
                path[plen] = 0;
                type = rd32be(data);
                fm_grant(path, false); /* documents named by the user or another program */
                gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
                fm_spec_for_path(path, spec);
                gaddr ah = tb_NewPtr(4, false, true);
                if (type == T("alis") && alias_new_for_host(path, ah) == noErr) {
                    gaddr alias = rd32(ah);
                    int32_t an = tb_GetHandleSize(alias);
                    int32_t end = tb_GetHandleSize(h);
                    tb_SetHandleSize(h, end + an);
                    gmemcpy(rd32(h) + (uint32_t)end, rd32(alias), (uint32_t)an);
                    tb_DisposeHandle(alias);
                } else {
                    type = T("fss ");
                    int32_t end = tb_GetHandleSize(h);
                    tb_SetHandleSize(h, end + FSSpec_SIZEOF);
                    gmemcpy(rd32(h) + (uint32_t)end, spec, FSSpec_SIZEOF);
                }
                tb_DisposePtr(ah);
                tb_DisposePtr(spec);
            } else
                unflatten_into(h, data, elen);
            uint32_t ulen = (uint32_t)(tb_GetHandleSize(h) - at - 12);
            gaddr q = rd32(h) + (uint32_t)at;
            wr32(q, key);
            wr32(q + 4, type);
            wr32(q + 8, ulen);
            if (ulen & 1) {
                int32_t end = tb_GetHandleSize(h);
                tb_SetHandleSize(h, end + 1);
                wr8(rd32(h) + (uint32_t)end, 0);
            }
            off += 12 + pad(elen);
        }
        return;
    }
    tb_SetHandleSize(h, base + (int32_t)len);
    if (len)
        gput(rd32(h) + (uint32_t)base, p, len);
}

static gaddr unflatten_data(const uint8_t *data, uint32_t len)
{
    gaddr h = tb_NewHandle(0, false, false);
    if (h)
        unflatten_into(h, data, len);
    return h;
}

/* ---------------------------------------------------------------------- */
/* Raw high-level events.  Early System 7 apps (TeachText) accept the
 * message themselves and walk the Apple Event's wire format:
 *   'aevt' 0x00010001
 *   attributes: keyword(4) type(4) size(4) data, ...   ';;;;'
 *   parameters: keyword(4) type(4) size(4) data, ...
 * A list's data is count(4), 0(4), then items: type(4) size(4) data
 * (a record's items with their keywords first).  Documents travel as
 * aliases. */

static void wire_entry(uint8_t **buf, uint32_t *n, uint32_t *cap, bool keyed, uint32_t key, uint32_t type, gaddr data,
                       uint32_t len);

static void wire_value(uint8_t **buf, uint32_t *n, uint32_t *cap, gaddr p, uint32_t len)
{
    uint32_t tag = len >= 8 ? rd32(p) : 0;
    if (tag == LIST_TAG || (tag == RECO_TAG && len >= 12)) {
        bool rec = tag == RECO_TAG;
        uint32_t count = rec ? rd32(p + 8) : rd32(p + 4), off = rec ? 12 : 8;
        if (rec) /* a record's attributes are left out */
            for (uint32_t i = 0; i < rd32(p + 4); i++)
                off += 12 + pad(rd32(p + off + 8));
        grow(buf, cap, *n + 8);
        put32(*buf + *n, count);
        put32(*buf + *n + 4, 0);
        *n += 8;
        for (uint32_t i = 0; i < count; i++) {
            wire_entry(buf, n, cap, rec, rd32(p + off), rd32(p + off + 4), p + off + 12, rd32(p + off + 8));
            off += 12 + pad(rd32(p + off + 8));
        }
        return;
    }
    grow(buf, cap, *n + len);
    gget(*buf + *n, p, len);
    *n += len;
}

static void wire_entry(uint8_t **buf, uint32_t *n, uint32_t *cap, bool keyed, uint32_t key, uint32_t type, gaddr data,
                       uint32_t len)
{
    uint32_t at = *n, head = keyed ? 12 : 8;
    grow(buf, cap, at + head);
    *n += head;
    char host[1024];
    gaddr ah = 0;
    if (type == T("fss ") && len >= 70 &&
        fm_host_path((int16_t)rd16(data), (int32_t)rd32(data + 2), data + 6, true, host, sizeof host)) {
        ah = tb_NewPtr(4, false, true);
        if (alias_new_for_host(host, ah) == noErr) {
            gaddr alias = rd32(ah);
            uint32_t size = (uint32_t)tb_GetHandleSize(alias);
            grow(buf, cap, *n + size);
            gget(*buf + *n, rd32(alias), size);
            *n += size;
            type = T("alis");
            tb_DisposeHandle(alias);
        } else
            wire_value(buf, n, cap, data, len);
        tb_DisposePtr(ah);
    } else
        wire_value(buf, n, cap, data, len);
    if (keyed)
        put32(*buf + at, key);
    put32(*buf + at + head - 8, type);
    put32(*buf + at + head - 4, *n - at - head);
}

static uint8_t *wire_event(gaddr evt, uint32_t *len_out)
{
    uint8_t *buf = NULL;
    uint32_t n = 0, cap = 0;
    grow(&buf, &cap, 8);
    put32(buf, T("aevt"));
    put32(buf + 4, 0x00010001);
    n = 8;
    gaddr h = desc_handle(evt);
    tb_HLock(h); /* aliases are made as we go */
    for (int attrs = 1; attrs >= 0; attrs--) {
        uint32_t count, off = first_entry(h, &count, attrs);
        gaddr p = rd32(h);
        for (uint32_t i = 0; i < count; i++) {
            wire_entry(&buf, &n, &cap, true, rd32(p + off), rd32(p + off + 4), p + off + 12, rd32(p + off + 8));
            off += 12 + pad(rd32(p + off + 8));
        }
        if (attrs) {
            grow(&buf, &cap, n + 4);
            memcpy(buf + n, ";;;;", 4);
            n += 4;
        }
    }
    tb_HUnlock(h);
    *len_out = n;
    return buf;
}

/* The event AcceptHighLevelEvent takes: the one WaitNextEvent returned
 * last (-1), or the queued one a GetSpecificHighLevelEvent filter is
 * looking at (its index). */
static int examining = -1;
static bool examined_gone; /* the filter accepted all of it */

static pending_ae *accepting(void)
{
    if (examining >= 0)
        return examining < qn ? &queue[examining] : NULL;
    return current.flat ? &current : NULL;
}

/* An event's message: a raw event's own bytes, or an Apple Event in its
 * wire format.  *owned when the caller frees it. */
static uint8_t *event_message(const pending_ae *e, uint32_t *len, bool *owned)
{
    *owned = !e->raw;
    if (e->raw) {
        *len = e->flatlen;
        return e->flat;
    }
    gaddr evt = tb_NewPtr(AEDesc_SIZEOF, false, true);
    build_event(e, evt);
    uint8_t *buf = wire_event(evt, len);
    tb_AEDisposeDesc(evt);
    tb_DisposePtr(evt);
    return buf;
}

static void our_name(uint8_t name[32])
{
    gget(name, LM_CurApName, 32);
    if (name[0] > 31)
        name[0] = 31;
}

/* The sender's TargetID: its session (Blue Box's are process IDs), and its
 * port as the Process Manager names them (GetPortNameFromProcessSerialNumber),
 * on this machine.  Apple Events from the host come from no one. */
static void put_sender(gaddr target, const pending_ae *e)
{
    gmemset(target, 0, TargetID_SIZEOF);
    if (!e->raw)
        return;
    gaddr port = target + TargetID_name_off;
    wr32(target + TargetID_sessionID_off, (uint32_t)e->sender_pid);
    gput(port + PPCPortRec_name_off, e->sender_name, 32);
    PPCPortRec_set_portKindsSelector(port, 1); /* ppcByCreatorAndType */
    wr32(port + PPCPortRec_u_port_creator_off, e->sender_sig);
    wr32(port + PPCPortRec_u_port_type_off, T("ep01"));
}

/* Take the event out of the queue once it is accepted in full: a raw
 * event, or an Apple Event a filter took (one WaitNextEvent returned stays
 * for AEProcessAppleEvent, which Blue Box does not route through here). */
static void accepted_in_full(pending_ae *p)
{
    if (p == &current) {
        if (p->raw)
            free_event(p, false);
        return;
    }
    int i = (int)(p - queue);
    free_event(p, true);
    memmove(queue + i, queue + i + 1, (size_t)(qn - i - 1) * sizeof *queue);
    qn--;
    examined_gone = true;
}

int16_t tb_AcceptHighLevelEvent(gaddr sender_id_return, gaddr refcon_return, gaddr msg_buf, gaddr msg_length_return)
{
    pthread_mutex_lock(&lock);
    pending_ae *p = accepting();
    pending_ae e = p ? *p : (pending_ae){ 0 };
    pthread_mutex_unlock(&lock);
    if (!p)
        return noOutstandingHLE;
    if (refcon_return)
        wr32(refcon_return, e.refcon);
    if (sender_id_return)
        put_sender(sender_id_return, &e);
    /* As much as the buffer holds; with bufferIsSmall, msgLen is what is
     * left, for another call to take. */
    bool owned;
    uint32_t len;
    uint8_t *msg = event_message(&e, &len, &owned);
    uint32_t at = e.taken < len ? e.taken : len, rest = len - at;
    uint32_t room = msg_buf && msg_length_return ? rd32(msg_length_return) : 0;
    uint32_t n = rest < room ? rest : room;
    if (n)
        gput(msg_buf, msg + at, n);
    int16_t err = n < rest ? bufferIsSmall : noErr;
    if (msg_length_return)
        wr32(msg_length_return, err ? rest - n : n);
    if (owned)
        free(msg);
    pthread_mutex_lock(&lock);
    p = accepting(); /* the queue may have grown meanwhile; entries stay put */
    if (p) {
        p->accepted = true;
        p->taken = at + n;
        if (!err)
            accepted_in_full(p);
    }
    pthread_mutex_unlock(&lock);
    send_receipts();
    return err;
}

/* The filter sees each event in the queue (the one WaitNextEvent returned
 * first) as a HighLevelEventMsg and the sender's TargetID; it may accept
 * the event it is looking at. */
int8_t tb_GetSpecificHighLevelEvent(gaddr fn, gaddr data, gaddr err_return)
{
    if (err_return)
        wr16(err_return, noErr);
    pthread_mutex_lock(&lock);
    bool any = current.flat || qn > 0;
    pthread_mutex_unlock(&lock);
    if (!any) {
        if (err_return)
            wr16(err_return, noOutstandingHLE);
        return 0;
    }
    gaddr msg = tb_NewPtr(HighLevelEventMsg_SIZEOF, false, true), sender = tb_NewPtr(TargetID_SIZEOF, false, true);
    bool found = false;
    for (int i = current.flat ? -1 : 0; !found; ) {
        pthread_mutex_lock(&lock);
        bool have = i < 0 ? current.flat != NULL : i < qn;
        pending_ae e = have ? i < 0 ? current : queue[i] : (pending_ae){ 0 };
        pthread_mutex_unlock(&lock);
        if (!have)
            break;
        bool owned;
        uint32_t len;
        uint8_t *bytes = event_message(&e, &len, &owned);
        if (owned)
            free(bytes);
        gmemset(msg, 0, HighLevelEventMsg_SIZEOF);
        HighLevelEventMsg_set_HighLevelEventMsgHeaderlength(msg, HighLevelEventMsg_SIZEOF);
        gaddr ev = msg + HighLevelEventMsg_theMsgEvent_off;
        wr16(ev, 23); /* kHighLevelEvent */
        wr32(ev + EventRecord_message_off, e.cls);
        wr32(ev + EventRecord_when_off, rd32(LM_Ticks));
        wr32(ev + EventRecord_where_off, e.id);
        wr16(ev + EventRecord_modifiers_off, e.receipt ? e.modifiers : 0);
        HighLevelEventMsg_set_userRefCon(msg, (int32_t)e.refcon);
        HighLevelEventMsg_set_postingOptions(msg, (int32_t)e.options);
        HighLevelEventMsg_set_msgLength(msg, (int32_t)(len - (e.taken < len ? e.taken : len)));
        put_sender(sender, &e);
        examining = i;
        examined_gone = false;
        uint32_t args[3] = { data, msg, sender };
        found = (aem_call_proc(fn, 0x00000FD0 /* pascal Boolean (ptr, ptr, ptr) */, args, 3) & 0xFF) != 0;
        examining = -1;
        if (!examined_gone || i < 0)
            i++;
    }
    tb_DisposePtr(msg);
    tb_DisposePtr(sender);
    return found;
}

/* Our serial number: kCurrentProcess or the one GetCurrentProcess gives. */
static bool psn_is_us(uint32_t hi, uint32_t lo) { return hi == 0 && (lo == 2 || lo == 0x1000); }

/* The other run an event goes to, by an address as AESend's ('sign',
 * 'kpid', 'psn '); procNotFound if none listens. */
static int16_t post_to_run(const pending_ae *e, uint32_t atype, uint32_t a0, uint32_t a1, gaddr msg_buf,
                           uint32_t msg_len)
{
    uint8_t addr[8];
    put32(addr, a0);
    put32(addr + 4, a1);
    uint8_t *msg = malloc(msg_len ? msg_len : 1);
    if (msg_len)
        gget(msg, msg_buf, msg_len);
    uint32_t len;
    uint8_t *flat = raw_flatten(e, msg, msg_len, &len);
    free(msg);
    uint8_t *reply = NULL;
    uint32_t reply_len = 0;
    int16_t err = aem_remote_send(addr, atype, atype == T("psn ") ? 8 : 4, flat, len, kAENoReply, 0, &reply,
                                  &reply_len);
    free(reply);
    free(flat);
    return err;
}

/* To this program itself, into its own queue (a common use), or to
 * another Blue Box run through the Apple Events' sockets, addressed by
 * serial number ({1, its process ID}), signature, session (a process ID,
 * as the sender's TargetID gives it) or a TargetID's port (by creator).
 * There are no other machines to reach. */
int16_t tb_PostHighLevelEvent(gaddr evt, gaddr receiver_id, int32_t refcon, gaddr msg_buf, int32_t msg_length,
                              int32_t post_options)
{
    if (!evt || msg_length < 0 || (msg_length && !msg_buf))
        return paramErr;
    uint32_t options = (uint32_t)post_options;
    pending_ae e = { .cls = rd32(evt + EventRecord_message_off), .id = rd32(evt + EventRecord_where_off),
                     .raw = true, .refcon = (uint32_t)refcon, .options = options, .sender_pid = getpid(),
                     .sender_sig = g_app_creator };
    our_name(e.sender_name);
    bool self = false;
    uint32_t atype = 0, a0 = 0, a1 = 0;
    int16_t missing = procNotFound;
    switch (options & receiverIDMask) {
    case receiverIDisPSN:
        a0 = rd32(receiver_id), a1 = rd32(receiver_id + 4);
        self = psn_is_us(a0, a1);
        atype = T("psn ");
        break;
    case receiverIDisSignature:
        a0 = receiver_id;
        self = a0 == g_app_creator;
        atype = T("sign");
        break;
    case receiverIDisSessionID:
        a0 = receiver_id;
        self = a0 == (uint32_t)getpid();
        atype = T("kpid");
        missing = connectionInvalid;
        break;
    case receiverIDisTargetID: {
        gaddr port = receiver_id + TargetID_name_off;
        if (PPCPortRec_portKindsSelector(port) != 1 /* ppcByCreatorAndType */)
            return procNotFound;
        a0 = rd32(port + PPCPortRec_u_port_creator_off);
        self = a0 == g_app_creator;
        atype = T("sign");
        break;
    }
    default:
        return paramErr;
    }
    if (!self) {
        int16_t err = post_to_run(&e, atype, a0, a1, msg_buf, (uint32_t)msg_length);
        return err == procNotFound ? missing : err;
    }
    e.self = true;
    e.flatlen = (uint32_t)msg_length;
    e.flat = malloc(e.flatlen ? e.flatlen : 1); /* never NULL: an empty message is still an event */
    if (e.flatlen)
        gget(e.flat, msg_buf, e.flatlen);
    pthread_mutex_lock(&lock);
    bool full = qn >= 16;
    pthread_mutex_unlock(&lock);
    if (full) {
        free(e.flat);
        return memFullErr;
    }
    enqueue(&e);
    return noErr;
}

/* The receipts owed, to their senders: as a 'jaym' 'rtrn' event with the
 * original's refCon and the result in its modifiers. */
static void send_receipts(void)
{
    pthread_mutex_lock(&outbox_lock);
    pending_ae owed[16];
    int n = nout;
    memcpy(owed, outbox, (size_t)n * sizeof *owed);
    nout = 0;
    pthread_mutex_unlock(&outbox_lock);
    for (int i = 0; i < n; i++) {
        pending_ae r = { .cls = T("jaym"), .id = T("rtrn"), .raw = true, .receipt = true,
                         .modifiers = owed[i].modifiers, .refcon = owed[i].refcon, .sender_pid = getpid(),
                         .sender_sig = g_app_creator };
        our_name(r.sender_name);
        if (owed[i].sender_pid == getpid()) {
            r.self = true;
            r.flat = malloc(1);
            enqueue(&r);
            continue;
        }
        uint32_t len, reply_len;
        uint8_t *flat = raw_flatten(&r, NULL, 0, &len), *reply = NULL;
        machine_ae_send(owed[i].sender_pid, owed[i].sender_sig, flat, len, kAENoReply, 0, &reply, &reply_len);
        free(reply);
        free(flat);
    }
}

/* ---------------------------------------------------------------------- */
/* The Apple Event Manager's private handler tables, which applications
 * that link ObjectSupportLib call directly: a handle holding a count and
 * then entries of an 8-byte selector (class and ID) and an 8-byte handler
 * (routine and refCon). */
enum { HDLR_ENTRY = 16 };

int16_t tb__AE_hdlr_table_alloc(int32_t a, int32_t b, int32_t c, int8_t sys, gaddr tablep)
{
    gaddr h = tb_NewHandle(4, false, true);
    if (!h)
        return memFullErr;
    if (tablep)
        wr32(tablep, h);
    return noErr;
}

static int hdlr_find(gaddr table, gaddr selector)
{
    if (!table || !rd32(table) || !selector)
        return -1;
    int n = (int)rd32(rd32(table));
    for (int i = 0; i < n; i++) {
        gaddr e = rd32(table) + 4 + (uint32_t)i * HDLR_ENTRY;
        if (rd32(e) == rd32(selector) && rd32(e + 4) == rd32(selector + 4))
            return i;
    }
    return -1;
}

int16_t tb__AE_hdlr_install(gaddr table, int32_t unused, gaddr selector, gaddr hdlr)
{
    if (!table || !rd32(table) || !selector || !hdlr)
        return paramErr;
    int i = hdlr_find(table, selector);
    if (i < 0) {
        i = (int)rd32(rd32(table));
        tb_SetHandleSize(table, 4 + (i + 1) * HDLR_ENTRY);
        if (tb_GetHandleSize(table) < 4 + (i + 1) * HDLR_ENTRY)
            return memFullErr;
        wr32(rd32(table), (uint32_t)(i + 1));
    }
    gaddr e = rd32(table) + 4 + (uint32_t)i * HDLR_ENTRY;
    gmemcpy(e, selector, 8);
    gmemcpy(e + 8, hdlr, 8);
    return noErr;
}

int16_t tb__AE_hdlr_insert(gaddr table, int32_t unused, gaddr selector, gaddr hdlr)
{
    return tb__AE_hdlr_install(table, unused, selector, hdlr);
}

int16_t tb__AE_hdlr_lookup(gaddr table, int32_t unused, gaddr selector, gaddr hdlr_return)
{
    int i = hdlr_find(table, selector);
    if (i < 0)
        return errAEHandlerNotFound;
    if (hdlr_return)
        gmemcpy(hdlr_return, rd32(table) + 4 + (uint32_t)i * HDLR_ENTRY + 8, 8);
    return noErr;
}

int16_t tb__AE_hdlr_delete(gaddr table, int32_t unused, gaddr selector)
{
    int i = hdlr_find(table, selector);
    if (i < 0)
        return errAEHandlerNotFound;
    int n = (int)rd32(rd32(table));
    gaddr e = rd32(table) + 4 + (uint32_t)i * HDLR_ENTRY;
    gmemcpy(e, e + HDLR_ENTRY, (uint32_t)(n - i - 1) * HDLR_ENTRY);
    wr32(rd32(table), (uint32_t)(n - 1));
    tb_SetHandleSize(table, 4 + (n - 1) * HDLR_ENTRY);
    return noErr;
}
