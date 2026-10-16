/*
 * InputSprocket, as PowerPC games call it (InputSprocketLib), on the
 * keyboard.  A game declares its needs (ISpNeed: a name, a kind, a label);
 * each becomes a virtual element mapped to keys, by label where the label
 * says what it is and by name otherwise: the arrows for directions and
 * axes, space to fire, Return to start, Escape to quit, P to pause, and
 * letters for the rest.  States are read from the keyboard when asked;
 * events are the changes since the element was last looked at.
 *
 * ISpNeed (mac68k alignment): name Str63 0, iconSuiteResourceId 64,
 * playerNum 66, group 67, theKind 68, theLabel 72, flags 76; 88 bytes.
 * ISpElementEvent: when (AbsoluteTime) 0, element 8, refCon 12, data 16.
 */
#include <ctype.h>
#include <string.h>

#include "machine.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

enum { NEED_SIZE = 88 };
#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
enum {
    kButton = FOURCC('b', 'u', 't', 'n'),
    kDPad = FOURCC('d', 'p', 'a', 'd'),
    kAxis = FOURCC('a', 'x', 'i', 's'),
    kDelta = FOURCC('d', 'l', 't', 'a'),
};
/* Axis values: the middle and the ends of an unsigned range. */
enum { AXIS_MIN = 0, AXIS_MID = 0x7FFFFFFF, AXIS_MAX = (int)0xFFFFFFFF };
/* D-pad values: kISpPadIdle 0, Left 1, UpLeft 2, Up 3, UpRight 4, Right 5,
 * DownRight 6, Down 7, DownLeft 8. */

/* Mac key codes. */
enum {
    K_LEFT = 0x7B, K_RIGHT = 0x7C, K_DOWN = 0x7D, K_UP = 0x7E, K_SPACE = 0x31, K_RETURN = 0x24,
    K_ESCAPE = 0x35, K_TAB = 0x30, K_OPTION = 0x3A, K_CONTROL = 0x3B, K_SHIFT = 0x38, K_COMMAND = 0x37,
};
/* Letters for the needs nothing else claims, with their key codes. */
static const uint8_t spare_keys[] = { 0x00 /* A */, 0x01 /* S */, 0x02 /* D */, 0x03 /* F */, 0x0D /* W */,
                                      0x0C /* Q */, 0x0E /* E */, 0x0F /* R */, 0x06 /* Z */, 0x07 /* X */,
                                      0x08 /* C */, 0x09 /* V */, 0x0B /* B */, 0x2D /* N */, 0x2E /* M */ };

typedef struct {
    gaddr ref;      /* the element reference handed out (a guest address) */
    uint32_t kind, label;
    uint8_t key, key2; /* a button's key; an axis's low and high keys */
    uint32_t last;  /* the state last reported */
    uint32_t refcon;
} element;

#define MAX_ELEMENTS 128
static element elements[MAX_ELEMENTS];
static int nelements, spare;
static gaddr ref_block;

static bool key_down(uint8_t code)
{
    static gaddr km;
    if (!km)
        km = tb_NewPtr(16, true, true);
    tb_GetKeys(km);
    return (rd8(km + (code >> 3)) >> (code & 7)) & 1;
}

static bool name_has(const char *name, const char *word) { return strstr(name, word) != NULL; }

/* The keys for a need, from its label, else its name. */
static void map_need(element *e, const char *name)
{
    e->key = e->key2 = 0xFF;
    switch (e->label) {
    case FOURCC('f', 'i', 'r', 'e'): e->key = K_SPACE; return;
    case FOURCC('s', 't', 'r', 't'): e->key = K_RETURN; return;
    case FOURCC('q', 'u', 'i', 't'): case FOURCC('a', 'b', 'r', 't'): e->key = K_ESCAPE; return;
    case FOURCC('p', 'a', 'u', 's'): e->key = 0x23; /* P */ return;
    case FOURCC('j', 'u', 'm', 'p'): e->key = K_OPTION; return;
    case FOURCC('x', 'a', 'x', 'i'): e->key = K_LEFT; e->key2 = K_RIGHT; return;
    case FOURCC('y', 'a', 'x', 'i'): e->key = K_DOWN; e->key2 = K_UP; return;
    default: break;
    }
    if (e->kind == kDPad || e->kind == kDelta)
        return;
    if (e->kind == kAxis) {
        if (name_has(name, "turn") || name_has(name, "steer") || name_has(name, "yaw") || name_has(name, "horiz")) {
            e->key = K_LEFT;
            e->key2 = K_RIGHT;
        } else {
            e->key = K_DOWN;
            e->key2 = K_UP;
        }
        return;
    }
    if (name_has(name, "left"))
        e->key = K_LEFT;
    else if (name_has(name, "right"))
        e->key = K_RIGHT;
    else if (name_has(name, "up") || name_has(name, "thrust") || name_has(name, "forward") ||
             name_has(name, "accel") || name_has(name, "gas"))
        e->key = K_UP;
    else if (name_has(name, "down") || name_has(name, "back") || name_has(name, "reverse") ||
             name_has(name, "brake"))
        e->key = K_DOWN;
    else if (name_has(name, "fire") || name_has(name, "shoot") || name_has(name, "attack") ||
             name_has(name, "punch"))
        e->key = K_SPACE;
    else if (name_has(name, "jump"))
        e->key = K_OPTION;
    else if (name_has(name, "pause"))
        e->key = 0x23;
    else if (name_has(name, "quit") || name_has(name, "abort") || name_has(name, "menu") || name_has(name, "escape"))
        e->key = K_ESCAPE;
    else if (name_has(name, "start") || name_has(name, "select") || name_has(name, "enter"))
        e->key = K_RETURN;
    else if (spare < (int)sizeof spare_keys)
        e->key = spare_keys[spare++];
}

static element *find(gaddr ref)
{
    for (int i = 0; i < nelements; i++)
        if (elements[i].ref == ref)
            return &elements[i];
    return NULL;
}

static uint32_t state_of(const element *e)
{
    if (e->kind == kDPad) {
        bool l = key_down(K_LEFT), r = key_down(K_RIGHT), u = key_down(K_UP), d = key_down(K_DOWN);
        if (u && l) return 2;
        if (u && r) return 4;
        if (d && r) return 6;
        if (d && l) return 8;
        return l ? 1 : u ? 3 : r ? 5 : d ? 7 : 0;
    }
    if (e->kind == kAxis) {
        bool lo = e->key != 0xFF && key_down(e->key), hi = e->key2 != 0xFF && key_down(e->key2);
        return lo && !hi ? (uint32_t)AXIS_MIN : hi && !lo ? (uint32_t)AXIS_MAX : (uint32_t)AXIS_MID;
    }
    if (e->kind == kDelta)
        return 0;
    return e->key != 0xFF && key_down(e->key) ? 1 : 0;
}

/* ISpElement_NewVirtualFromNeeds(count, needs, *elements, flags) */
static void isp_NewVirtualFromNeeds(ppc_cpu *c)
{
    uint32_t count = R(0);
    gaddr needs = R(1), out = R(2);
    if (!ref_block)
        ref_block = tb_NewPtr(MAX_ELEMENTS * 4, true, true);
    for (uint32_t i = 0; i < count && nelements < MAX_ELEMENTS; i++) {
        gaddr n = needs + i * NEED_SIZE;
        element *e = &elements[nelements];
        memset(e, 0, sizeof *e);
        e->ref = ref_block + (gaddr)nelements * 4;
        e->kind = rd32(n + 68);
        e->label = rd32(n + 72);
        char name[64];
        int len = rd8(n);
        for (int k = 0; k < len && k < 63; k++)
            name[k] = (char)tolower(rd8(n + 1 + (gaddr)k));
        name[len < 63 ? len : 63] = 0;
        map_need(e, name);
        e->last = state_of(e);
        nelements++;
        if (out)
            wr32(out + i * 4, e->ref);
    }
    RETURN(0);
}

/* ISpInit(count, needs, elements, appCreator, subCreator, flags, setList, reserved):
 * the elements already have their keys. */
static void isp_Init(ppc_cpu *c) { RETURN(0); }

static void isp_GetSimpleState(ppc_cpu *c)
{
    element *e = find(R(0));
    if (R(1))
        wr32(R(1), e ? state_of(e) : 0);
    RETURN(e ? 0 : -30561 /* kISpElementNotInListErr */);
}

/* An event if the element's state changed since it was last looked at. */
static bool next_event(element *e, gaddr ev)
{
    uint32_t now = state_of(e);
    if (now == e->last)
        return false;
    e->last = now;
    if (ev) {
        uint64_t us = machine_time_ns() / 1000;
        wr32(ev, (uint32_t)(us >> 32));
        wr32(ev + 4, (uint32_t)us);
        wr32(ev + 8, e->ref);
        wr32(ev + 12, e->refcon);
        wr32(ev + 16, now);
    }
    return true;
}

/* ISpElement_GetNextEvent(element, bufSize, *event, *wasEvent) */
static void isp_GetNextEvent(ppc_cpu *c)
{
    element *e = find(R(0));
    bool got = e && next_event(e, R(2));
    if (R(3))
        wr8(R(3), got);
    RETURN(0);
}

/* Element lists: our list is the elements' refs; GetNextEvent looks at each. */
#define MAX_LISTS 16
static struct {
    gaddr ref;
    gaddr items[MAX_ELEMENTS];
    uint32_t refcons[MAX_ELEMENTS];
    int count;
} lists[MAX_LISTS];
static int nlists;

static int list_of(gaddr ref)
{
    for (int i = 0; i < nlists; i++)
        if (lists[i].ref == ref)
            return i;
    return -1;
}

/* ISpElementList_New(count, items, *list, flags) */
static void isp_ListNew(ppc_cpu *c)
{
    if (nlists >= MAX_LISTS) {
        RETURN(-108);
        return;
    }
    int l = nlists++;
    lists[l].ref = tb_NewPtr(4, true, true);
    lists[l].count = 0;
    for (uint32_t i = 0; i < R(0) && lists[l].count < MAX_ELEMENTS; i++)
        lists[l].items[lists[l].count++] = rd32(R(1) + i * 4);
    if (R(2))
        wr32(R(2), lists[l].ref);
    RETURN(0);
}
/* ISpElementList_AddElements(list, refCon, count, items) */
static void isp_ListAdd(ppc_cpu *c)
{
    int l = list_of(R(0));
    for (uint32_t i = 0; l >= 0 && i < R(2) && lists[l].count < MAX_ELEMENTS; i++) {
        gaddr item = rd32(R(3) + i * 4);
        element *e = find(item);
        if (e)
            e->refcon = R(1);
        lists[l].items[lists[l].count++] = item;
    }
    RETURN(l >= 0 ? 0 : -50);
}
/* ISpElementList_GetNextEvent(list, bufSize, *event, *wasEvent) */
static void isp_ListGetNextEvent(ppc_cpu *c)
{
    int l = list_of(R(0));
    bool got = false;
    for (int i = 0; l >= 0 && i < lists[l].count && !got; i++) {
        element *e = find(lists[l].items[i]);
        if (e)
            got = next_event(e, R(2));
    }
    if (R(3))
        wr8(R(3), got);
    RETURN(0);
}
static void isp_Flush(ppc_cpu *c)
{
    element *e = find(R(0));
    if (e)
        e->last = state_of(e);
    RETURN(0);
}
static void isp_ListFlush(ppc_cpu *c)
{
    int l = list_of(R(0));
    for (int i = 0; l >= 0 && i < lists[l].count; i++) {
        element *e = find(lists[l].items[i]);
        if (e)
            e->last = state_of(e);
    }
    RETURN(0);
}
static void isp_noErr(ppc_cpu *c) { RETURN(0); }
/* 1.4, as a NumVersion through the result pointer in r3 */
static void isp_GetVersion(ppc_cpu *c)
{
    if (R(0))
        wr32(R(0), 0x01408000);
    c->pc = c->lr;
}
/* ISpConfigure(eventProc, *accepted): no dialog; the mapping stands. */
static void isp_Configure(ppc_cpu *c)
{
    if (R(1))
        wr8(R(1), 1);
    RETURN(0);
}
/* ISpGetGlobalElementList(*list): an empty list. */
static void isp_GetGlobalElementList(ppc_cpu *c)
{
    if (R(0)) {
        int l = nlists < MAX_LISTS ? nlists++ : 0;
        if (!lists[l].ref)
            lists[l].ref = tb_NewPtr(4, true, true);
        wr32(R(0), lists[l].ref);
    }
    RETURN(0);
}
/* ISpDevices_Extract(count, *found, devices): none to list. */
static void isp_DevicesExtract(ppc_cpu *c)
{
    if (R(1))
        wr32(R(1), 0);
    RETURN(0);
}

void inputsprocket_reset(void)
{
    nelements = spare = nlists = 0;
    ref_block = 0;
    memset(lists, 0, sizeof lists);
}

const ppc_export isp_exports[] = {
    { "ISpConfigure", isp_Configure },
    { "ISpDevices_ActivateClass", isp_noErr },
    { "ISpDevices_DeactivateClass", isp_noErr },
    { "ISpDevices_Activate", isp_noErr },
    { "ISpDevices_Deactivate", isp_noErr },
    { "ISpDevices_Extract", isp_DevicesExtract },
    { "ISpDevices_ExtractByClass", isp_DevicesExtract },
    { "ISpElementList_AddElements", isp_ListAdd },
    { "ISpElementList_Dispose", isp_noErr },
    { "ISpElementList_Flush", isp_ListFlush },
    { "ISpElementList_GetNextEvent", isp_ListGetNextEvent },
    { "ISpElementList_New", isp_ListNew },
    { "ISpElementList_RemoveElements", isp_noErr },
    { "ISpElement_DisposeVirtual", isp_noErr },
    { "ISpElement_Flush", isp_Flush },
    { "ISpElement_GetNextEvent", isp_GetNextEvent },
    { "ISpElement_GetSimpleState", isp_GetSimpleState },
    { "ISpElement_NewVirtual", isp_noErr },
    { "ISpElement_NewVirtualFromNeeds", isp_NewVirtualFromNeeds },
    { "ISpGetGlobalElementList", isp_GetGlobalElementList },
    { "ISpGetVersion", isp_GetVersion },
    { "ISpInit", isp_Init },
    { "ISpResume", isp_noErr },
    { "ISpShutdown", isp_noErr },
    { "ISpStartup", isp_noErr },
    { "ISpStop", isp_noErr },
    { "ISpSuspend", isp_noErr },
    { "ISpTickle", isp_noErr },
};
const unsigned isp_export_count = sizeof isp_exports / sizeof isp_exports[0];
