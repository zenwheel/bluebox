/*
 * Routines a walk of the Vintage Apple Macintosh archive found programs
 * calling that Blue Box lacked: the Window Manager's layers, Open
 * Transport's timer, deferred and system tasks, and Carbon's names for
 * older international string calls.
 */
#include <stdio.h>
#include <string.h>

#include "machine.h"
#include "ppc.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

/* ---------------------------------------------------------------------- */
/* Window Manager layers.  System 7 kept each process's windows in a layer
 * (a window record whose children are its windows) under the desk's root
 * layer.  One program runs here, so there is the root and the program's
 * layer, whose windows are the window list; a layer NewLayer makes is a
 * record of its own, the program's windows staying in the window list. */

#define MAX_LAYERS 16
static gaddr layers[MAX_LAYERS]; /* [0] the root, [1] the program's */
static gaddr layer_parent[MAX_LAYERS];
static int nlayers;
static gaddr cur_layer;

static int layer_index(gaddr w)
{
    for (int i = 0; i < nlayers; i++)
        if (w && layers[i] == w)
            return i;
    return -1;
}

static gaddr new_layer(gaddr parent, bool visible, int32_t refcon)
{
    if (nlayers == MAX_LAYERS)
        return 0;
    gaddr l = mm_new_ptr_in(rd32(LM_SysZone), WindowRecord_SIZEOF, true);
    if (!l)
        return 0;
    wr8(l + WindowRecord_visible_off, visible ? 0xFF : 0);
    wr32(l + WindowRecord_refCon_off, (uint32_t)refcon);
    layers[nlayers] = l;
    layer_parent[nlayers++] = parent;
    return l;
}

/* The root and the program's layer, made when first asked for. */
static void layers_init(void)
{
    if (nlayers)
        return;
    gaddr root = new_layer(0, true, 0);
    gaddr app = new_layer(root, true, 0);
    cur_layer = app;
}

int16_t tb_NewLayer(gaddr layer, int8_t visible, int8_t neverActive, gaddr behind, int32_t refCon)
{
    (void)neverActive;
    (void)behind;
    layers_init();
    gaddr l = new_layer(cur_layer, visible != 0, refCon);
    if (layer)
        wr32(layer, l);
    return l ? noErr : memFullErr;
}

gaddr tb_GetCurLayer(void)
{
    layers_init();
    return cur_layer;
}

int16_t tb_SetCurLayer(gaddr layer)
{
    layers_init();
    if (layer_index(layer) < 0)
        return paramErr;
    cur_layer = layer;
    return noErr;
}

gaddr tb_SwapCurLayer(gaddr layer)
{
    gaddr old = tb_GetCurLayer();
    tb_SetCurLayer(layer);
    return old;
}

/* A layer's first child: the root's is the program's layer, the
 * program's the front window. */
gaddr tb_GetSubWindows(gaddr layer)
{
    layers_init();
    int i = layer_index(layer);
    if (i == 1)
        return rd32(LM_WindowList);
    for (int k = 0; k < nlayers; k++)
        if (i >= 0 && layer_parent[k] == layer)
            return layers[k];
    return 0;
}

/* The layer a window or layer is in. */
gaddr tb_GetParent(gaddr window)
{
    layers_init();
    int i = layer_index(window);
    if (i >= 0)
        return layer_parent[i];
    for (gaddr w = rd32(LM_WindowList); w; w = rd32(w + WindowRecord_nextWindow_off))
        if (w == window)
            return layers[1];
    return 0;
}

int8_t tb_IsLayer(gaddr window)
{
    layers_init();
    return layer_index(window) >= 0;
}

/* ---------------------------------------------------------------------- */
/* Open Transport's tasks: a procedure (OTProcessUPP: void (*)(void *arg))
 * run later, at deferred task time.  A timer task runs once its time has
 * come, a deferred or system task at the next chance. */

enum { TASK_TIMER = 1, TASK_DEFERRED, TASK_SYSTEM };
#define MAX_OT_TASKS 64
typedef struct {
    int kind;
    gaddr proc, arg;
    bool scheduled;
    uint64_t due; /* machine_time_ns */
} ot_task;
static ot_task ot_tasks[MAX_OT_TASKS];

static uint32_t ot_task_new(int kind, gaddr proc, gaddr arg)
{
    for (int i = 0; i < MAX_OT_TASKS; i++)
        if (!ot_tasks[i].kind) {
            ot_tasks[i] = (ot_task){ kind, proc, arg, false, 0 };
            return (uint32_t)(0x4F540000u | (uint32_t)(i + 1)); /* 'OT', the index from 1 */
        }
    return 0;
}

static ot_task *ot_task_of(uint32_t id, int kind)
{
    uint32_t i = (id & 0xFFFF) - 1;
    if ((id & 0xFFFF0000u) != 0x4F540000u || i >= MAX_OT_TASKS || ot_tasks[i].kind != kind)
        return NULL;
    return &ot_tasks[i];
}

/* Schedule: true when it was not scheduled already (a timer task's
 * time is moved either way). */
static bool ot_task_schedule(uint32_t id, int kind, uint32_t ms)
{
    ot_task *t = ot_task_of(id, kind);
    if (!t)
        return false;
    bool was = t->scheduled;
    t->scheduled = true;
    if (kind == TASK_TIMER)
        t->due = machine_time_ns() + (uint64_t)ms * 1000000u;
    return kind == TASK_TIMER || !was;
}

static bool ot_task_cancel(uint32_t id, int kind)
{
    ot_task *t = ot_task_of(id, kind);
    bool was = t && t->scheduled;
    if (t)
        t->scheduled = false;
    return was;
}

static int32_t ot_task_destroy(uint32_t id, int kind)
{
    ot_task *t = ot_task_of(id, kind);
    if (!t)
        return -3150; /* kOTBadReferenceErr */
    memset(t, 0, sizeof *t);
    return noErr;
}

/* The tasks whose time has come, from the Time Manager's pump. */
void ot_tasks_pump(void)
{
    uint64_t now = machine_time_ns();
    for (int i = 0; i < MAX_OT_TASKS; i++) {
        ot_task *t = &ot_tasks[i];
        if (!t->kind || !t->scheduled || (t->kind == TASK_TIMER && t->due > now))
            continue;
        t->scheduled = false;
        uint32_t arg = t->arg;
        if (!t->proc)
            continue;
        if (mm_is_descriptor(t->proc))
            mm_call(t->proc, 0x000000C1 /* C, one long */, &arg, 1);
        else
            ppc_call(t->proc, &arg, 1);
    }
}

static void ot_CreateTimerTask(ppc_cpu *c) { RETURN(ot_task_new(TASK_TIMER, R(0), R(1))); }
static void ot_ScheduleTimerTask(ppc_cpu *c) { RETURN(ot_task_schedule(R(0), TASK_TIMER, R(1))); }
static void ot_CancelTimerTask(ppc_cpu *c) { RETURN(ot_task_cancel(R(0), TASK_TIMER)); }
static void ot_DestroyTimerTask(ppc_cpu *c)
{
    ot_task_destroy(R(0), TASK_TIMER);
    c->pc = c->lr;
}
static void ot_CreateDeferredTask(ppc_cpu *c) { RETURN(ot_task_new(TASK_DEFERRED, R(0), R(1))); }
static void ot_ScheduleDeferredTask(ppc_cpu *c) { RETURN(ot_task_schedule(R(0), TASK_DEFERRED, 0)); }
static void ot_DestroyDeferredTask(ppc_cpu *c) { RETURN(ot_task_destroy(R(0), TASK_DEFERRED)); }
static void ot_CreateSystemTask(ppc_cpu *c) { RETURN(ot_task_new(TASK_SYSTEM, R(0), R(1))); }
static void ot_ScheduleSystemTask(ppc_cpu *c) { RETURN(ot_task_schedule(R(0), TASK_SYSTEM, 0)); }
static void ot_CancelSystemTask(ppc_cpu *c) { RETURN(ot_task_cancel(R(0), TASK_SYSTEM)); }
static void ot_DestroySystemTask(ppc_cpu *c) { RETURN(ot_task_destroy(R(0), TASK_SYSTEM)); }

/* OTCompareAndSwap8 and 16 (oldValue, newValue, *dest): Boolean. */
static void ot_CompareAndSwap8(ppc_cpu *c)
{
    bool same = R(2) && rd8(R(2)) == (uint8_t)R(0);
    if (same)
        wr8(R(2), (uint8_t)R(1));
    RETURN(same);
}
static void ot_CompareAndSwap16(ppc_cpu *c)
{
    bool same = R(2) && rd16(R(2)) == (uint16_t)R(0);
    if (same)
        wr16(R(2), (uint16_t)R(1));
    RETURN(same);
}

/* ---------------------------------------------------------------------- */
/* Carbon's names for the International Utilities' Pascal-string calls. */

/* CompareString(a, b, itl2): IUCompPString. */
static void CompareString(ppc_cpu *c)
{
    gaddr a = R(0), b = R(1);
    RETURN((int32_t)tb_IUMagPString(a + 1, b + 1, a ? rd8(a) : 0, b ? rd8(b) : 0, R(2)));
}
/* IdenticalString(a, b, itl2): IUEqualPString's answer, 0 when equal. */
static void IdenticalString(ppc_cpu *c)
{
    gaddr a = R(0), b = R(1);
    RETURN((int32_t)tb_IUMagIDPString(a + 1, b + 1, a ? rd8(a) : 0, b ? rd8(b) : 0, R(2)));
}
/* DateString(dateTime, longFlag, result, intlHandle): IUDatePString. */
static void DateString(ppc_cpu *c)
{
    tb_IUDatePString((int32_t)R(0), (int8_t)R(1), R(2), R(3));
    c->pc = c->lr;
}
/* TimeString(dateTime, wantSeconds, result, intlHandle): IUTimePString. */
static void TimeString(ppc_cpu *c)
{
    tb_IUTimePString((int32_t)R(0), (int8_t)R(1), R(2), R(3));
    c->pc = c->lr;
}


/* ---------------------------------------------------------------------- */
/* Internet Config's mappings database (the "Mapping" preference): packed
 * ICMapEntry records, each its total length, its fixed part's length,
 * version, type, creator, post-processor's creator and flags, then five
 * Pascal strings (extension, creator's and post-processor's names, MIME
 * type, entry name).  The calls read and write such a handle; given out
 * as an ICMapEntry, the strings are each a Str255 of their own. */

enum { icPrefNotFoundErr = -666, icPrefDataErr = -668, icNoURLErr = -673, icConfigInappropriateErr = -675 };
enum { kICMapFixedLength = 22, ICMapEntry_SIZE = 22 + 5 * 256 };
enum { kICMapNotIncomingMask = 0x10, kICMapNotOutgoingMask = 0x20 };

/* The entry at pos in a handle's bytes: its length, 0 if none is there. */
static int32_t map_entry_len(gaddr p, int32_t size, int32_t pos)
{
    if (pos < 0 || pos + kICMapFixedLength > size)
        return 0;
    int32_t total = (int16_t)rd16(p + (uint32_t)pos), fixed = (int16_t)rd16(p + (uint32_t)pos + 2);
    return total >= fixed && fixed >= kICMapFixedLength && pos + total <= size ? total : 0;
}

/* Unpack the entry at pos into an ICMapEntry. */
static void map_unpack(gaddr p, int32_t pos, int32_t total, gaddr entry)
{
    gaddr e = p + (uint32_t)pos;
    gmemset(entry, 0, ICMapEntry_SIZE);
    wr16(entry, ICMapEntry_SIZE);
    wr16(entry + 2, kICMapFixedLength);
    gmemcpy(entry + 4, e + 4, kICMapFixedLength - 4);
    int32_t at = (int16_t)rd16(e + 2);
    for (int k = 0; k < 5 && at < total; k++) {
        uint32_t n = rd8(e + (uint32_t)at);
        if (at + 1 + (int32_t)n > total)
            n = (uint32_t)(total - at - 1);
        wr8(entry + 22 + 256u * (uint32_t)k, (uint8_t)n);
        gmemcpy(entry + 23 + 256u * (uint32_t)k, e + (uint32_t)at + 1, n);
        at += 1 + (int32_t)rd8(e + (uint32_t)at);
    }
}

/* Pack an ICMapEntry into a new pointer; its length in *len. */
static gaddr map_pack(gaddr entry, int32_t *len)
{
    int32_t total = kICMapFixedLength;
    for (int k = 0; k < 5; k++)
        total += 1 + rd8(entry + 22 + 256u * (uint32_t)k);
    gaddr p = tb_NewPtr(total, false, false);
    if (!p)
        return 0;
    wr16(p, (uint16_t)total);
    wr16(p + 2, kICMapFixedLength);
    gmemcpy(p + 4, entry + 4, kICMapFixedLength - 4);
    uint32_t at = kICMapFixedLength;
    for (int k = 0; k < 5; k++) {
        uint32_t n = rd8(entry + 22 + 256u * (uint32_t)k);
        gmemcpy(p + at, entry + 22 + 256u * (uint32_t)k, n + 1);
        at += n + 1;
    }
    *len = total;
    return p;
}

static int32_t map_size(gaddr h) { return h && rd32(h) ? tb_GetHandleSize(h) : 0; }

int16_t ic_map_count(gaddr entries, gaddr count)
{
    int32_t size = map_size(entries), n = 0, len;
    for (int32_t pos = 0; (len = map_entry_len(rd32(entries), size, pos)) > 0; pos += len)
        n++;
    if (count)
        wr32(count, (uint32_t)n);
    return entries && rd32(entries) ? noErr : paramErr;
}

int16_t ic_map_get(gaddr entries, int32_t pos, gaddr entry)
{
    int32_t len = map_entry_len(map_size(entries) ? rd32(entries) : 0, map_size(entries), pos);
    if (!len)
        return paramErr;
    if (entry)
        map_unpack(rd32(entries), pos, len, entry);
    return noErr;
}

int16_t ic_map_get_ind(gaddr entries, int32_t index, gaddr posp, gaddr entry)
{
    int32_t size = map_size(entries), len, i = 1;
    for (int32_t pos = 0; (len = map_entry_len(rd32(entries), size, pos)) > 0; pos += len, i++)
        if (i == index) {
            if (posp)
                wr32(posp, (uint32_t)pos);
            if (entry)
                map_unpack(rd32(entries), pos, len, entry);
            return noErr;
        }
    return icPrefNotFoundErr;
}

int16_t ic_map_set(gaddr entries, int32_t pos, gaddr entry)
{
    int32_t old = map_entry_len(map_size(entries) ? rd32(entries) : 0, map_size(entries), pos), len = 0;
    if (!old || !entry)
        return paramErr;
    gaddr p = map_pack(entry, &len);
    if (!p)
        return memFullErr;
    tb_Munger(entries, pos, 0, old, p, len);
    tb_DisposePtr(p);
    return noErr;
}

int16_t ic_map_delete(gaddr entries, int32_t pos)
{
    int32_t old = map_entry_len(map_size(entries) ? rd32(entries) : 0, map_size(entries), pos);
    if (!old)
        return paramErr;
    tb_Munger(entries, pos, 0, old, rd32(entries), 0); /* replaced by nothing */
    return noErr;
}

int16_t ic_map_add(gaddr entries, gaddr entry)
{
    int32_t len = 0;
    if (!entries || !entry)
        return paramErr;
    gaddr p = map_pack(entry, &len);
    if (!p)
        return memFullErr;
    int16_t err = tb_PtrAndHand(p, entries, len);
    tb_DisposePtr(p);
    return err;
}

/* How well a file name ends in an entry's extension: its length, 0 if not. */
static int ext_match(gaddr name, gaddr e, int32_t total)
{
    int32_t at = (int16_t)rd16(e + 2);
    uint32_t n = at < total ? rd8(e + (uint32_t)at) : 0, fn = name ? rd8(name) : 0;
    if (!n || n > fn)
        return 0;
    for (uint32_t k = 0; k < n; k++) {
        uint8_t a = rd8(name + 1 + fn - n + k), b = rd8(e + (uint32_t)at + 1 + k);
        if (a >= 'A' && a <= 'Z')
            a += 32;
        if (b >= 'A' && b <= 'Z')
            b += 32;
        if (a != b)
            return 0;
    }
    return (int)n;
}

/* ICMapEntriesFilename: the incoming mapping with the longest extension
 * the name ends in.  ICMapEntriesTypeCreator: the outgoing mapping for
 * the type and creator, else the type alone, the name's extension
 * deciding between equals; else the name's mapping. */
static int16_t map_find(gaddr entries, uint32_t type, uint32_t creator, gaddr name, bool outgoing, gaddr entry)
{
    int32_t size = map_size(entries), len, best = -1, best_score = 0, best_len = 0;
    gaddr p = size ? rd32(entries) : 0;
    for (int32_t pos = 0; (len = map_entry_len(p, size, pos)) > 0; pos += len) {
        gaddr e = p + (uint32_t)pos;
        uint32_t flags = rd32(e + 18);
        if (flags & (outgoing ? kICMapNotOutgoingMask : kICMapNotIncomingMask))
            continue;
        int ext = ext_match(name, e, len), score;
        if (outgoing)
            score = rd32(e + 6) != type ? 0 : (rd32(e + 10) == creator ? 2000 : 1000) + ext;
        else
            score = ext;
        if (score > best_score) {
            best_score = score;
            best = pos;
            best_len = len;
        }
    }
    if (best < 0)
        return outgoing && name && rd8(name) ? map_find(entries, 0, 0, name, false, entry) : icPrefNotFoundErr;
    if (entry)
        map_unpack(p, best, best_len, entry);
    return noErr;
}

int16_t ic_map_entries_filename(gaddr entries, gaddr name, gaddr entry)
{
    return name && rd8(name) ? map_find(entries, 0, 0, name, false, entry) : paramErr;
}

int16_t ic_map_entries_type_creator(gaddr entries, uint32_t type, uint32_t creator, gaddr name, gaddr entry)
{
    return map_find(entries, type, creator, name, true, entry);
}

/* The same against the user's own mappings, when there are any. */
static int16_t with_mappings(int (*fn)(gaddr h, void *ctx), void *ctx)
{
    gaddr key = tb_NewPtr(8, false, false), h = tb_NewHandle(0, false, false);
    int16_t err = memFullErr;
    if (key && h) {
        wr8(key, 7);
        gput(key + 1, "Mapping", 7);
        err = ic_get_pref_handle(key, 0, 0, h);
        if (!err)
            err = (int16_t)fn(h, ctx);
    }
    if (h)
        tb_DisposeHandle(h);
    if (key)
        tb_DisposePtr(key);
    return err;
}
typedef struct { uint32_t type, creator; gaddr name, entry; } map_query;
static int by_name(gaddr h, void *ctx)
{
    map_query *q = ctx;
    return ic_map_entries_filename(h, q->name, q->entry);
}
static int by_type(gaddr h, void *ctx)
{
    map_query *q = ctx;
    return ic_map_entries_type_creator(h, q->type, q->creator, q->name, q->entry);
}
int16_t ic_map_filename(gaddr name, gaddr entry)
{
    map_query q = { 0, 0, name, entry };
    return with_mappings(by_name, &q);
}
int16_t ic_map_type_creator(uint32_t type, uint32_t creator, gaddr name, gaddr entry)
{
    map_query q = { type, creator, name, entry };
    return with_mappings(by_type, &q);
}

/* ICParseURL(hint, data, len, &selStart, &selEnd, url): the URL at the
 * selection (or around the insertion point), less any "<URL:...>" or
 * "<...>" around it, into the handle; name@address is hint:name@address
 * when there is a hint. */
static bool url_char(uint8_t ch) { return ch > ' ' && ch < 0x7F && ch != '<' && ch != '>' && ch != '"'; }

int16_t ic_parse_url(gaddr hint, gaddr data, int32_t len, gaddr sel_start, gaddr sel_end, gaddr url)
{
    int32_t a = sel_start ? (int32_t)rd32(sel_start) : 0, b = sel_end ? (int32_t)rd32(sel_end) : len;
    if (!data || len < 0 || a < 0 || b > len || a > b)
        return paramErr;
    if (a == b) { /* an insertion point: the run of URL characters around it */
        while (a > 0 && url_char(rd8(data + (uint32_t)a - 1)))
            a--;
        while (b < len && url_char(rd8(data + (uint32_t)b)))
            b++;
    }
    while (a < b && !url_char(rd8(data + (uint32_t)a)))
        a++;
    while (b > a && !url_char(rd8(data + (uint32_t)b - 1)))
        b--;
    if (b - a >= 4 && rd8(data + (uint32_t)a) == 'U' && rd8(data + (uint32_t)a + 1) == 'R' &&
        rd8(data + (uint32_t)a + 2) == 'L' && rd8(data + (uint32_t)a + 3) == ':')
        a += 4;
    char text[1024];
    int n = 0;
    for (int32_t i = a; i < b && n < (int)sizeof text - 1; i++) {
        uint8_t ch = rd8(data + (uint32_t)i);
        if (ch > ' ')
            text[n++] = (char)ch;
    }
    text[n] = 0;
    char out[1300];
    if (!n)
        return icNoURLErr;
    if (strchr(text, ':')) {
        snprintf(out, sizeof out, "%s", text);
    } else {
        uint32_t hn = hint ? rd8(hint) : 0;
        if (!hn || !strchr(text, '@'))
            return icNoURLErr;
        char h[256];
        gget(h, hint + 1, hn);
        h[hn] = 0;
        snprintf(out, sizeof out, "%s:%s", h, text);
    }
    if (sel_start)
        wr32(sel_start, (uint32_t)a);
    if (sel_end)
        wr32(sel_end, (uint32_t)b);
    if (url) {
        int32_t ol = (int32_t)strlen(out);
        tb_SetHandleSize(url, ol);
        if (tb_GetHandleSize(url) != ol)
            return memFullErr;
        gput(rd32(url), out, (uint32_t)ol);
    }
    return noErr;
}

/* ICGetConfigReference: the configuration in use, Internet Config's
 * own ('JPQE') and the preferences file it keeps; ICSetConfigReference
 * takes back one of those. */
int16_t ic_get_config_ref(gaddr ref)
{
    if (!ref)
        return paramErr;
    tb_SetHandleSize(ref, 4);
    if (tb_GetHandleSize(ref) != 4)
        return memFullErr;
    wr32(rd32(ref), fourcc("JPQE"));
    return noErr;
}

int16_t ic_set_config_ref(gaddr ref)
{
    if (!ref || !rd32(ref) || tb_GetHandleSize(ref) < 4)
        return paramErr;
    return rd32(rd32(ref)) == fourcc("JPQE") ? noErr : icConfigInappropriateErr;
}

/* ICEditPreferences: Internet Config's application, which isn't here. */
int16_t ic_edit_preferences(void) { return fnfErr; }

static void ic_CountMapEntries(ppc_cpu *c) { RETURN((int32_t)ic_map_count(R(1), R(2))); }
static void ic_GetIndMapEntry(ppc_cpu *c) { RETURN((int32_t)ic_map_get_ind(R(1), (int32_t)R(2), R(3), R(4))); }
static void ic_GetMapEntry(ppc_cpu *c) { RETURN((int32_t)ic_map_get(R(1), (int32_t)R(2), R(3))); }
static void ic_SetMapEntry(ppc_cpu *c) { RETURN((int32_t)ic_map_set(R(1), (int32_t)R(2), R(3))); }
static void ic_DeleteMapEntry(ppc_cpu *c) { RETURN((int32_t)ic_map_delete(R(1), (int32_t)R(2))); }
static void ic_AddMapEntry(ppc_cpu *c) { RETURN((int32_t)ic_map_add(R(1), R(2))); }
static void ic_MapEntriesFilename(ppc_cpu *c) { RETURN((int32_t)ic_map_entries_filename(R(1), R(2), R(3))); }
static void ic_MapEntriesTypeCreator(ppc_cpu *c)
{
    RETURN((int32_t)ic_map_entries_type_creator(R(1), R(2), R(3), R(4), R(5)));
}
static void ic_MapFilename(ppc_cpu *c) { RETURN((int32_t)ic_map_filename(R(1), R(2))); }
static void ic_MapTypeCreator(ppc_cpu *c) { RETURN((int32_t)ic_map_type_creator(R(1), R(2), R(3), R(4))); }
static void ic_ParseURL(ppc_cpu *c) { RETURN((int32_t)ic_parse_url(R(1), R(2), (int32_t)R(3), R(4), R(5), R(6))); }
static void ic_GetConfigReference(ppc_cpu *c) { RETURN((int32_t)ic_get_config_ref(R(1))); }
static void ic_SetConfigReference(ppc_cpu *c) { RETURN((int32_t)ic_set_config_ref(R(1))); }
static void ic_EditPreferences(ppc_cpu *c) { RETURN((int32_t)ic_edit_preferences()); }
/* ICGetComponentInstance(inst, &ci): the Internet Config component
 * behind the instance, opened for the program. */
static void ic_GetComponentInstance(ppc_cpu *c)
{
    gaddr ci = components_open_default(fourcc("PREF"), fourcc("ICAp"));
    if (R(1))
        wr32(R(1), ci);
    RETURN(ci ? noErr : -2000 /* invalidComponentID */);
}

/* ---------------------------------------------------------------------- */
/* The Script Manager. */

/* TruncText(width, textPtr, short *length, truncWhere): the text cut to
 * fit width with an ellipsis, as TruncString cuts a Pascal string. */
int16_t tb_TruncText(int16_t width, gaddr text, gaddr lenp, int16_t where)
{
    int16_t len = lenp ? (int16_t)rd16(lenp) : 0;
    if (len <= 0 || !text)
        return 0;
    if (len > 255)
        len = 255;
    gaddr tmp = tb_NewPtr(256, false, false);
    if (!tmp)
        return -1;
    wr8(tmp, (uint8_t)len);
    gmemcpy(tmp + 1, text, (uint32_t)len);
    int16_t r = tb_TruncString(width, tmp, where);
    if (r > 0) {
        gmemcpy(text, tmp + 1, rd8(tmp));
        wr16(lenp, rd8(tmp));
    }
    tb_DisposePtr(tmp);
    return r;
}

/* FindWordBreaks: FindWord, for the system script's word breaks. */
void tb_FindWordBreaks(gaddr text, int16_t len, int16_t offset, int8_t leading, gaddr breaks, gaddr offsets,
                       int16_t script)
{
    (void)script;
    tb_FindWord(text, len, offset, leading, breaks, offsets);
}

const ppc_export vam_exports[] = {
    { "OTCreateTimerTask", ot_CreateTimerTask }, { "OTCreateTimerTaskInContext", ot_CreateTimerTask },
    { "OTScheduleTimerTask", ot_ScheduleTimerTask }, { "OTCancelTimerTask", ot_CancelTimerTask },
    { "OTDestroyTimerTask", ot_DestroyTimerTask },
    { "OTCreateDeferredTask", ot_CreateDeferredTask }, { "OTCreateDeferredTaskInContext", ot_CreateDeferredTask },
    { "OTScheduleDeferredTask", ot_ScheduleDeferredTask }, { "OTScheduleInterruptTask", ot_ScheduleDeferredTask },
    { "OTDestroyDeferredTask", ot_DestroyDeferredTask },
    { "OTCreateSystemTask", ot_CreateSystemTask }, { "OTScheduleSystemTask", ot_ScheduleSystemTask },
    { "OTCancelSystemTask", ot_CancelSystemTask }, { "OTDestroySystemTask", ot_DestroySystemTask },
    { "OTCompareAndSwap8", ot_CompareAndSwap8 }, { "OTCompareAndSwap16", ot_CompareAndSwap16 },
    { "CompareString", CompareString }, { "IdenticalString", IdenticalString },
    { "DateString", DateString }, { "TimeString", TimeString },
    { "ICCountMapEntries", ic_CountMapEntries }, { "ICGetIndMapEntry", ic_GetIndMapEntry },
    { "ICGetMapEntry", ic_GetMapEntry }, { "ICSetMapEntry", ic_SetMapEntry },
    { "ICDeleteMapEntry", ic_DeleteMapEntry }, { "ICAddMapEntry", ic_AddMapEntry },
    { "ICMapEntriesFilename", ic_MapEntriesFilename }, { "ICMapEntriesTypeCreator", ic_MapEntriesTypeCreator },
    { "ICMapFilename", ic_MapFilename }, { "ICMapTypeCreator", ic_MapTypeCreator },
    { "ICParseURL", ic_ParseURL }, { "ICGetConfigReference", ic_GetConfigReference },
    { "ICSetConfigReference", ic_SetConfigReference }, { "ICEditPreferences", ic_EditPreferences },
    { "ICGetComponentInstance", ic_GetComponentInstance },
};
const unsigned vam_export_count = sizeof vam_exports / sizeof vam_exports[0];

/* A new run: no layers, no tasks. */
void vam_reset(void)
{
    nlayers = 0;
    cur_layer = 0;
    memset(ot_tasks, 0, sizeof ot_tasks);
}
