/*
 * InputSprocket, as PowerPC games call it (InputSprocketLib), on the
 * keyboard, the mouse and the host's game controllers.
 *
 * A game declares its needs (ISpNeed: a name, a kind, a label); each
 * becomes a virtual element mapped by its label where the label says what
 * it is and by its name otherwise: on the keyboard the arrows for
 * directions and axes, space to fire, Return to start, Escape to quit, P to
 * pause and letters for the rest; on a controller the left stick and the
 * pad for movement, the right stick for looking, A to fire, B to jump, Menu
 * to start or pause, Options to quit and the other buttons for the rest.
 * A need for player 1 or 2 reads that controller, one for no player any of
 * them.  Delta needs follow the mouse.  The configuration dialog accepts
 * this mapping.
 *
 * The devices are the keyboard, the mouse and each connected controller,
 * each with elements of its own (keys; the button and the two deltas; the
 * buttons, the pad and the axes); the global element list holds them all.
 * States are read when asked; events are the changes since the element was
 * last looked at.  Suspended (ISpSuspend), every element is idle.
 *
 * ISpNeed (mac68k alignment): name Str63 0, iconSuiteResourceId 64,
 * playerNum 66, group 67, theKind 68, theLabel 72, flags 76, three reserved
 * longs; 92 bytes.
 * ISpElementEvent: when (AbsoluteTime) 0, element 8, refCon 12, data 16.
 * ISpDeviceDefinition: deviceName Str63 0, manufacturerName Str63 64,
 * version 128, theDeviceClass 132, theDeviceIdentifier 136, permanentID
 * 140, flags 144, permanentIDExtend 148, two reserved longs; 160 bytes.
 * ISpElementInfo: theLabel 0, theKind 4, theString Str63 8, two reserved
 * longs; 80 bytes.
 * ISpMovementData: xAxis 0, yAxis 4, direction 8; 12 bytes.
 * AbsoluteTime here counts microseconds.
 */
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <string.h>

#include "forks.h"
#include "lowmem_gen.h"
#include "machine.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

enum { NEED_SIZE = 92, DEFINITION_SIZE = 160, INFO_SIZE = 80 };
#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
enum {
    kButton = FOURCC('b', 'u', 't', 'n'),
    kDPad = FOURCC('d', 'p', 'a', 'd'),
    kAxis = FOURCC('a', 'x', 'i', 's'),
    kDelta = FOURCC('d', 'l', 't', 'a'),
    kMovement = FOURCC('m', 'o', 'v', 'm'),
    kLabelNone = FOURCC('n', 'o', 'n', 'e'),
    kClassKeyboard = FOURCC('k', 'e', 'y', 'd'),
    kClassMouse = FOURCC('m', 'o', 'u', 's'),
    kClassGamepad = FOURCC('g', 'm', 'p', 'd'),
};
enum { kISpBufferToSmallErr = -30342, kISpElementNotInListErr = -30344 };
/* Axis values: the middle and the ends of an unsigned range. */
enum { AXIS_MIN = 0, AXIS_MID = 0x7FFFFFFF, AXIS_MAX = (int)0xFFFFFFFF };
/* D-pad values: kISpPadIdle 0, Left 1, UpLeft 2, Up 3, UpRight 4, Right 5,
 * DownRight 6, Down 7, DownLeft 8. */

/* Mac key codes. */
enum {
    K_LEFT = 0x7B, K_RIGHT = 0x7C, K_DOWN = 0x7D, K_UP = 0x7E, K_SPACE = 0x31, K_RETURN = 0x24,
    K_ESCAPE = 0x35, K_TAB = 0x30, K_OPTION = 0x3A, K_CONTROL = 0x3B, K_SHIFT = 0x38, K_COMMAND = 0x37,
    K_P = 0x23, K_NONE = 0xFF,
};
/* Letters for the needs nothing else claims, with their key codes. */
static const uint8_t spare_keys[] = { 0x00 /* A */, 0x01 /* S */, 0x02 /* D */, 0x03 /* F */, 0x0D /* W */,
                                      0x0C /* Q */, 0x0E /* E */, 0x0F /* R */, 0x06 /* Z */, 0x07 /* X */,
                                      0x08 /* C */, 0x09 /* V */, 0x0B /* B */, 0x2D /* N */, 0x2E /* M */ };
/* Controller buttons for the needs nothing else claims. */
static const uint32_t spare_buttons[] = { MACHINE_PAD_X, MACHINE_PAD_Y, MACHINE_PAD_L1, MACHINE_PAD_R1,
                                          MACHINE_PAD_L2, MACHINE_PAD_R2, MACHINE_PAD_L3, MACHINE_PAD_R3 };

/* ---- The controllers, as the host last set them ---- */

static pthread_mutex_t pad_lock = PTHREAD_MUTEX_INITIALIZER;
static machine_gamepad host_pads[MACHINE_PADS];

void machine_set_gamepad(int index, const machine_gamepad *pad)
{
    if (index < 0 || index >= MACHINE_PADS || !pad)
        return;
    pthread_mutex_lock(&pad_lock);
    host_pads[index] = *pad;
    pthread_mutex_unlock(&pad_lock);
}

/* The controllers as of this call into InputSprocket. */
static machine_gamepad pads[MACHINE_PADS];

static void read_pads(void)
{
    pthread_mutex_lock(&pad_lock);
    memcpy(pads, host_pads, sizeof pads);
    pthread_mutex_unlock(&pad_lock);
}

/* ---- Devices ---- */

enum { DEV_KEYBOARD, DEV_MOUSE, DEV_PAD, NDEVICES = DEV_PAD + MACHINE_PADS };
static struct {
    gaddr ref;
    bool inactive;
    bool listed; /* its elements made */
} devices[NDEVICES];
static gaddr device_block;

static bool device_present(int d) { return d < DEV_PAD || pads[d - DEV_PAD].connected; }

static uint32_t device_class(int d)
{
    return d == DEV_KEYBOARD ? kClassKeyboard : d == DEV_MOUSE ? kClassMouse : kClassGamepad;
}

static gaddr device_ref(int d)
{
    if (!device_block)
        device_block = tb_NewPtr(NDEVICES * 4, true, true);
    devices[d].ref = device_block + (gaddr)d * 4;
    return devices[d].ref;
}

static int device_of(gaddr ref)
{
    for (int d = 0; d < NDEVICES; d++)
        if (ref && devices[d].ref == ref)
            return d;
    return -1;
}

/* ---- Elements ---- */

typedef struct {
    gaddr ref;      /* the element reference handed out (a guest address) */
    uint32_t kind, label;
    uint8_t name[64]; /* a Str63 */
    int device;     /* a device's own element: which; -1 for a virtual one */
    int player;     /* the controller a virtual element reads: 0 any, else player n's */
    uint8_t group;
    uint8_t key, key2; /* a button's key; an axis's low and high keys */
    uint32_t pad, pad2; /* a button's controller buttons; an axis's low and high buttons */
    int8_t stick;   /* a button's stick direction (axis * 2, +1 for the positive end), an axis's stick; -1 none */
    bool mouse;     /* the mouse button, or a delta following the mouse */
    uint32_t last;  /* the state last reported */
    int16_t mouse_at; /* a delta: the mouse coordinate it was last read at */
    uint32_t refcon;
} element;

#define MAX_ELEMENTS 256
static element elements[MAX_ELEMENTS];
static int nelements, spare, spare_button;
static gaddr ref_block;
static bool suspended;

static element *new_element(uint32_t kind, uint32_t label)
{
    if (nelements >= MAX_ELEMENTS)
        return NULL;
    if (!ref_block)
        ref_block = tb_NewPtr(MAX_ELEMENTS * 4, true, true);
    element *e = &elements[nelements];
    memset(e, 0, sizeof *e);
    e->ref = ref_block + (gaddr)nelements * 4;
    e->kind = kind;
    e->label = label;
    e->device = -1;
    e->key = e->key2 = K_NONE;
    e->stick = -1;
    nelements++;
    return e;
}

static void set_name(element *e, const char *s)
{
    size_t n = strlen(s) < 63 ? strlen(s) : 63;
    e->name[0] = (uint8_t)n;
    memcpy(e->name + 1, s, n);
}

static bool name_has(const char *name, const char *word) { return strstr(name, word) != NULL; }

static uint8_t next_spare_key(void) { return spare < (int)sizeof spare_keys ? spare_keys[spare++] : K_NONE; }
static uint32_t next_spare_button(void)
{
    int n = (int)(sizeof spare_buttons / sizeof spare_buttons[0]);
    return spare_button < n ? spare_buttons[spare_button++] : 0;
}

#define STICK(axis, positive) ((int8_t)((axis) * 2 + ((positive) ? 1 : 0)))

/* A button need's keys and controller buttons: a direction, an action. */
static void map_direction(element *e, uint8_t key, uint32_t button, int8_t stick)
{
    e->key = key;
    e->pad = button;
    e->stick = stick;
}

/* Buttons by label (K_SPARE: a spare letter): turning and moving are the
 * arrows and the left stick, sliding the shoulder buttons, looking the
 * right stick. */
enum { K_SPARE = 0xFE };
static const struct {
    uint32_t label;
    uint8_t key;
    uint32_t pad;
    int8_t stick;
} button_labels[] = {
    { FOURCC('f', 'i', 'r', 'e'), K_SPACE, MACHINE_PAD_A | MACHINE_PAD_R2, -1 },
    { FOURCC('s', 'f', 'i', 'r'), K_TAB, MACHINE_PAD_X | MACHINE_PAD_L2, -1 },
    { FOURCC('j', 'u', 'm', 'p'), K_OPTION, MACHINE_PAD_B, -1 },
    { FOURCC('s', 't', 'r', 't'), K_RETURN, MACHINE_PAD_MENU, -1 },
    { FOURCC('q', 'u', 'i', 't'), K_ESCAPE, MACHINE_PAD_OPTIONS, -1 },
    { FOURCC('a', 'b', 'r', 't'), K_ESCAPE, MACHINE_PAD_OPTIONS, -1 },
    { FOURCC('p', 'a', 'u', 's'), K_P, MACHINE_PAD_MENU, -1 },
    { FOURCC('b', 't', 't', 'l'), K_LEFT, MACHINE_PAD_LEFT, STICK(MACHINE_PAD_LX, false) },
    { FOURCC('b', 't', 't', 'r'), K_RIGHT, MACHINE_PAD_RIGHT, STICK(MACHINE_PAD_LX, true) },
    { FOURCC('b', 't', 'm', 'f'), K_UP, MACHINE_PAD_UP, STICK(MACHINE_PAD_LY, true) },
    { FOURCC('b', 't', 'm', 'b'), K_DOWN, MACHINE_PAD_DOWN, STICK(MACHINE_PAD_LY, false) },
    { FOURCC('b', 'l', 'f', 't'), K_SPARE, MACHINE_PAD_L1, -1 },
    { FOURCC('b', 'r', 'g', 't'), K_SPARE, MACHINE_PAD_R1, -1 },
    { FOURCC('b', 't', 'l', 'u'), K_SPARE, 0, STICK(MACHINE_PAD_RY, true) },
    { FOURCC('b', 't', 'l', 'd'), K_SPARE, 0, STICK(MACHINE_PAD_RY, false) },
    { FOURCC('b', 't', 'l', 'l'), K_SPARE, 0, STICK(MACHINE_PAD_RX, false) },
    { FOURCC('b', 't', 'l', 'r'), K_SPARE, 0, STICK(MACHINE_PAD_RX, true) },
};

static bool map_button_label(element *e)
{
    for (unsigned i = 0; i < sizeof button_labels / sizeof button_labels[0]; i++)
        if (button_labels[i].label == e->label) {
            uint8_t key = button_labels[i].key;
            map_direction(e, key == K_SPARE ? next_spare_key() : key, button_labels[i].pad, button_labels[i].stick);
            return true;
        }
    return false;
}

static void map_button_name(element *e, const char *name)
{
    if (name_has(name, "left"))
        map_direction(e, K_LEFT, MACHINE_PAD_LEFT, STICK(MACHINE_PAD_LX, false));
    else if (name_has(name, "right"))
        map_direction(e, K_RIGHT, MACHINE_PAD_RIGHT, STICK(MACHINE_PAD_LX, true));
    else if (name_has(name, "up") || name_has(name, "thrust") || name_has(name, "forward") ||
             name_has(name, "accel") || name_has(name, "gas"))
        map_direction(e, K_UP, MACHINE_PAD_UP, STICK(MACHINE_PAD_LY, true));
    else if (name_has(name, "down") || name_has(name, "back") || name_has(name, "reverse") ||
             name_has(name, "brake"))
        map_direction(e, K_DOWN, MACHINE_PAD_DOWN, STICK(MACHINE_PAD_LY, false));
    else if (name_has(name, "fire") || name_has(name, "shoot") || name_has(name, "attack") ||
             name_has(name, "punch"))
        e->key = K_SPACE, e->pad = MACHINE_PAD_A | MACHINE_PAD_R2;
    else if (name_has(name, "jump"))
        e->key = K_OPTION, e->pad = MACHINE_PAD_B;
    else if (name_has(name, "pause"))
        e->key = K_P, e->pad = MACHINE_PAD_MENU;
    else if (name_has(name, "quit") || name_has(name, "abort") || name_has(name, "menu") || name_has(name, "escape"))
        e->key = K_ESCAPE, e->pad = MACHINE_PAD_OPTIONS;
    else if (name_has(name, "start") || name_has(name, "select") || name_has(name, "enter"))
        e->key = K_RETURN, e->pad = MACHINE_PAD_MENU;
    else
        e->key = next_spare_key(), e->pad = next_spare_button();
}

/* An axis need: its keys, its stick and its pad buttons. */
static void map_axis(element *e, const char *name)
{
    bool horizontal;
    switch (e->label) {
    case FOURCC('x', 'a', 'x', 'i'): e->stick = MACHINE_PAD_LX; horizontal = true; break;
    case FOURCC('y', 'a', 'x', 'i'): e->stick = MACHINE_PAD_LY; horizontal = false; break;
    case FOURCC('r', 'x', 'a', 'x'): e->stick = MACHINE_PAD_RX; horizontal = true; break;
    case FOURCC('r', 'y', 'a', 'x'): e->stick = MACHINE_PAD_RY; horizontal = false; break;
    /* Moving along z (forward and back) and rolling: the left stick, which
     * the turns about x and y leave free. */
    case FOURCC('z', 'a', 'x', 'i'): e->stick = MACHINE_PAD_LY; horizontal = false; break;
    case FOURCC('r', 'z', 'a', 'x'): e->stick = MACHINE_PAD_LX; horizontal = true; break;
    default:
        horizontal = name_has(name, "turn") || name_has(name, "steer") || name_has(name, "yaw") ||
                     name_has(name, "horiz") || name_has(name, "left") || name_has(name, "roll");
        e->stick = horizontal ? MACHINE_PAD_LX : MACHINE_PAD_LY;
        break;
    }
    if (horizontal) {
        e->key = K_LEFT, e->key2 = K_RIGHT;
        e->pad = MACHINE_PAD_LEFT, e->pad2 = MACHINE_PAD_RIGHT;
    } else {
        e->key = K_DOWN, e->key2 = K_UP;
        e->pad = MACHINE_PAD_DOWN, e->pad2 = MACHINE_PAD_UP;
    }
}

static void map_need(element *e, const char *name)
{
    switch (e->kind) {
    case kButton:
        if (!map_button_label(e))
            map_button_name(e, name);
        break;
    case kAxis: map_axis(e, name); break;
    case kDPad: case kMovement: e->stick = MACHINE_PAD_LX; break;
    case kDelta:
        e->mouse = true;
        e->stick = e->label == FOURCC('y', 'a', 'x', 'i') ? MACHINE_PAD_LY : MACHINE_PAD_LX;
        break;
    default: break;
    }
}

static element *find(gaddr ref)
{
    for (int i = 0; i < nelements; i++)
        if (ref && elements[i].ref == ref)
            return &elements[i];
    return NULL;
}

/* ---- Reading the hardware ---- */

static bool key_down(uint8_t code)
{
    static gaddr km;
    if (code == K_NONE || devices[DEV_KEYBOARD].inactive)
        return false;
    if (!km)
        km = tb_NewPtr(16, true, true);
    tb_GetKeys(km);
    return (rd8(km + (code >> 3)) >> (code & 7)) & 1;
}

/* Whether controller p counts for e (and is there and active). */
static bool pad_for(const element *e, int p)
{
    if (!pads[p].connected || devices[DEV_PAD + p].inactive)
        return false;
    if (e->device >= DEV_PAD)
        return e->device == DEV_PAD + p;
    return e->device < 0 && (e->player == 0 || e->player == p + 1);
}

static bool pad_button(const element *e, uint32_t mask)
{
    for (int p = 0; mask && p < MACHINE_PADS; p++)
        if (pad_for(e, p) && (pads[p].buttons & mask))
            return true;
    return false;
}

/* A stick's position on the controllers e reads: the one pushed furthest. */
static float pad_axis(const element *e, int axis)
{
    float v = 0;
    for (int p = 0; p < MACHINE_PADS; p++)
        if (pad_for(e, p) && fabsf(pads[p].axes[axis]) > fabsf(v))
            v = pads[p].axes[axis];
    return v;
}

static bool mouse_down(void) { return !devices[DEV_MOUSE].inactive && !(rd8(LM_MBState) & 0x80); }

static uint32_t axis_value(float v)
{
    if (v >= 1)
        return (uint32_t)AXIS_MAX;
    if (v <= -1)
        return AXIS_MIN;
    return (uint32_t)((double)AXIS_MID + v * (double)AXIS_MID);
}

/* A pad direction (kISpPad*) from four switches. */
static uint32_t pad_direction(bool l, bool r, bool u, bool d)
{
    if (u && l) return 2;
    if (u && r) return 4;
    if (d && r) return 6;
    if (d && l) return 8;
    return l ? 1 : u ? 3 : r ? 5 : d ? 7 : 0;
}

enum { DEAD_ZONE_PCT = 20, PRESS_PCT = 50 };

static uint32_t dpad_state(const element *e)
{
    float x = pad_axis(e, MACHINE_PAD_LX), y = pad_axis(e, MACHINE_PAD_LY);
    bool l = pad_button(e, MACHINE_PAD_LEFT) || x < -PRESS_PCT / 100.0f;
    bool r = pad_button(e, MACHINE_PAD_RIGHT) || x > PRESS_PCT / 100.0f;
    bool u = pad_button(e, MACHINE_PAD_UP) || y > PRESS_PCT / 100.0f;
    bool d = pad_button(e, MACHINE_PAD_DOWN) || y < -PRESS_PCT / 100.0f;
    if (e->device < 0 || e->device == DEV_KEYBOARD) {
        l |= key_down(K_LEFT), r |= key_down(K_RIGHT), u |= key_down(K_UP), d |= key_down(K_DOWN);
    }
    return pad_direction(l, r, u, d);
}

/* An axis from a stick, else a controller's pad, else two keys. */
static uint32_t axis_state(const element *e, int stick, uint8_t key_lo, uint8_t key_hi, uint32_t pad_lo,
                           uint32_t pad_hi)
{
    float v = stick >= 0 ? pad_axis(e, stick) : 0;
    if (fabsf(v) > DEAD_ZONE_PCT / 100.0f)
        return axis_value(v);
    bool lo = pad_button(e, pad_lo) || key_down(key_lo), hi = pad_button(e, pad_hi) || key_down(key_hi);
    return lo && !hi ? (uint32_t)AXIS_MIN : hi && !lo ? (uint32_t)AXIS_MAX : (uint32_t)AXIS_MID;
}

static bool button_state(const element *e)
{
    if (key_down(e->key) || pad_button(e, e->pad) || (e->mouse && mouse_down()))
        return true;
    if (e->stick >= 0) {
        float v = pad_axis(e, e->stick / 2);
        return (e->stick & 1) ? v > PRESS_PCT / 100.0f : v < -PRESS_PCT / 100.0f;
    }
    return false;
}

/* A delta's movement since it was last read (Fixed points; up positive). */
static int32_t delta_state(element *e, bool consume)
{
    if (devices[DEV_MOUSE].inactive)
        return 0;
    bool y = e->label == FOURCC('y', 'a', 'x', 'i');
    int16_t at = (int16_t)rd16(LM_MouseLocation + (y ? 0 : 2));
    int32_t d = y ? e->mouse_at - at : at - e->mouse_at;
    if (consume)
        e->mouse_at = at;
    return d * 65536;
}

static uint32_t state_of(element *e)
{
    if (suspended)
        return e->kind == kAxis ? (uint32_t)AXIS_MID : 0;
    switch (e->kind) {
    case kDPad: case kMovement: return dpad_state(e);
    case kAxis: return axis_state(e, e->stick, e->key, e->key2, e->pad, e->pad2);
    case kDelta: return (uint32_t)delta_state(e, false);
    default: return button_state(e);
    }
}

/* ---- Making elements ---- */

static void reset_delta(element *e)
{
    bool y = e->label == FOURCC('y', 'a', 'x', 'i');
    e->mouse_at = (int16_t)rd16(LM_MouseLocation + (y ? 0 : 2));
}

/* ISpElement_NewVirtualFromNeeds(count, needs, *elements, flags) */
static void isp_NewVirtualFromNeeds(ppc_cpu *c)
{
    uint32_t count = R(0);
    gaddr needs = R(1), out = R(2);
    read_pads();
    for (uint32_t i = 0; i < count; i++) {
        gaddr n = needs + i * NEED_SIZE;
        element *e = new_element(rd32(n + 68), rd32(n + 72));
        if (!e) {
            RETURN(-108);
            return;
        }
        int len = rd8(n) < 63 ? rd8(n) : 63;
        char name[64];
        for (int k = 0; k <= len; k++)
            e->name[k] = rd8(n + (gaddr)k);
        e->name[0] = (uint8_t)len;
        for (int k = 0; k < len; k++)
            name[k] = (char)tolower(e->name[1 + k]);
        name[len] = 0;
        e->player = rd8(n + 66);
        e->group = rd8(n + 67);
        map_need(e, name);
        reset_delta(e);
        e->last = state_of(e);
        if (out)
            wr32(out + i * 4, e->ref);
    }
    RETURN(0);
}

/* ISpElement_NewVirtual(buffSize, *element, flags): one the game sets up
 * itself, which nothing moves. */
static void isp_NewVirtual(ppc_cpu *c)
{
    element *e = new_element(kButton, kLabelNone);
    if (R(1))
        wr32(R(1), e ? e->ref : 0);
    RETURN(e ? 0 : -108);
}

/* A device's own elements, made when first asked for. */
static void device_elements(int d)
{
    if (devices[d].listed)
        return;
    devices[d].listed = true;
    element *e;
    if (d == DEV_KEYBOARD) {
        static const struct { uint8_t key; const char *name; uint32_t label; } keys[] = {
            { K_UP, "Up Arrow", 0 }, { K_DOWN, "Down Arrow", 0 }, { K_LEFT, "Left Arrow", 0 },
            { K_RIGHT, "Right Arrow", 0 }, { K_SPACE, "Space", FOURCC('f', 'i', 'r', 'e') },
            { K_RETURN, "Return", FOURCC('s', 't', 'r', 't') }, { K_ESCAPE, "Escape", FOURCC('q', 'u', 'i', 't') },
            { K_TAB, "Tab", 0 }, { K_SHIFT, "Shift", 0 }, { K_OPTION, "Option", 0 }, { K_CONTROL, "Control", 0 },
            { K_COMMAND, "Command", 0 },
        };
        for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++)
            if ((e = new_element(kButton, keys[i].label ? keys[i].label : kLabelNone)))
                set_name(e, keys[i].name), e->key = keys[i].key, e->device = d;
        /* The letters, by their key codes on a US keyboard. */
        static const char letters[] = "ASDFHGZXCV\0BQWERYT";
        for (int code = 0; code < (int)sizeof letters - 1; code++) {
            if (!letters[code] || !(e = new_element(kButton, kLabelNone)))
                continue;
            char s[2] = { letters[code], 0 };
            set_name(e, s), e->key = (uint8_t)code, e->device = d;
        }
    } else if (d == DEV_MOUSE) {
        if ((e = new_element(kButton, FOURCC('f', 'i', 'r', 'e'))))
            set_name(e, "Mouse Button"), e->mouse = true, e->device = d;
        if ((e = new_element(kDelta, FOURCC('x', 'a', 'x', 'i'))))
            set_name(e, "X Movement"), e->mouse = true, e->device = d, reset_delta(e);
        if ((e = new_element(kDelta, FOURCC('y', 'a', 'x', 'i'))))
            set_name(e, "Y Movement"), e->mouse = true, e->device = d, reset_delta(e);
    } else {
        static const struct { uint32_t mask; const char *name; uint32_t label; } buttons[] = {
            { MACHINE_PAD_A, "A", FOURCC('f', 'i', 'r', 'e') }, { MACHINE_PAD_B, "B", FOURCC('j', 'u', 'm', 'p') },
            { MACHINE_PAD_X, "X", 0 }, { MACHINE_PAD_Y, "Y", 0 }, { MACHINE_PAD_L1, "Left Shoulder", 0 },
            { MACHINE_PAD_R1, "Right Shoulder", 0 }, { MACHINE_PAD_L2, "Left Trigger", 0 },
            { MACHINE_PAD_R2, "Right Trigger", 0 }, { MACHINE_PAD_L3, "Left Stick Button", 0 },
            { MACHINE_PAD_R3, "Right Stick Button", 0 }, { MACHINE_PAD_MENU, "Menu", FOURCC('s', 't', 'r', 't') },
            { MACHINE_PAD_OPTIONS, "Options", FOURCC('q', 'u', 'i', 't') },
        };
        for (unsigned i = 0; i < sizeof buttons / sizeof buttons[0]; i++)
            if ((e = new_element(kButton, buttons[i].label ? buttons[i].label : kLabelNone)))
                set_name(e, buttons[i].name), e->pad = buttons[i].mask, e->device = d;
        if ((e = new_element(kDPad, FOURCC('p', 'a', 'd', ' '))))
            set_name(e, "Direction Pad"), e->device = d;
        static const struct { int axis; const char *name; uint32_t label; } axes[] = {
            { MACHINE_PAD_LX, "Left Stick X", FOURCC('x', 'a', 'x', 'i') },
            { MACHINE_PAD_LY, "Left Stick Y", FOURCC('y', 'a', 'x', 'i') },
            { MACHINE_PAD_RX, "Right Stick X", FOURCC('r', 'x', 'a', 'x') },
            { MACHINE_PAD_RY, "Right Stick Y", FOURCC('r', 'y', 'a', 'x') },
        };
        for (unsigned i = 0; i < sizeof axes / sizeof axes[0]; i++)
            if ((e = new_element(kAxis, axes[i].label)))
                set_name(e, axes[i].name), e->stick = (int8_t)axes[i].axis, e->device = d;
    }
    for (int i = 0; i < nelements; i++)
        if (elements[i].device == d)
            elements[i].last = state_of(&elements[i]);
}

/* ---- States and events ---- */

static void isp_GetSimpleState(ppc_cpu *c)
{
    read_pads();
    element *e = find(R(0));
    uint32_t v = 0;
    if (e) {
        v = state_of(e);
        if (e->kind == kDelta)
            delta_state(e, true);
    }
    if (R(1))
        wr32(R(1), v);
    RETURN(e ? 0 : kISpElementNotInListErr);
}

/* ISpElement_GetComplexState(element, bufSize, *state): a movement's two
 * axes and direction, else the simple state. */
static void isp_GetComplexState(ppc_cpu *c)
{
    read_pads();
    element *e = find(R(0));
    gaddr out = R(2);
    if (!e) {
        RETURN(kISpElementNotInListErr);
        return;
    }
    if (e->kind == kMovement) {
        if (R(1) < 12) {
            RETURN(kISpBufferToSmallErr);
            return;
        }
        uint32_t x = axis_state(e, MACHINE_PAD_LX, K_LEFT, K_RIGHT, MACHINE_PAD_LEFT, MACHINE_PAD_RIGHT);
        uint32_t y = axis_state(e, MACHINE_PAD_LY, K_DOWN, K_UP, MACHINE_PAD_DOWN, MACHINE_PAD_UP);
        wr32(out, suspended ? (uint32_t)AXIS_MID : x);
        wr32(out + 4, suspended ? (uint32_t)AXIS_MID : y);
        wr32(out + 8, state_of(e));
        RETURN(0);
        return;
    }
    if (R(1) < 4) {
        RETURN(kISpBufferToSmallErr);
        return;
    }
    wr32(out, state_of(e));
    if (e->kind == kDelta)
        delta_state(e, true);
    RETURN(0);
}

/* An event if the element's state changed since it was last looked at (a
 * delta: if it moved). */
static bool next_event(element *e, gaddr ev)
{
    uint32_t now = state_of(e);
    if (e->kind == kDelta) {
        if (!now)
            return false;
        delta_state(e, true);
    } else if (now == e->last) {
        return false;
    }
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
    read_pads();
    element *e = find(R(0));
    bool got = e && next_event(e, R(2));
    if (R(3))
        wr8(R(3), got);
    RETURN(e ? 0 : kISpElementNotInListErr);
}

static void isp_Flush(ppc_cpu *c)
{
    read_pads();
    element *e = find(R(0));
    if (e) {
        e->last = state_of(e);
        if (e->kind == kDelta)
            delta_state(e, true);
    }
    RETURN(0);
}

/* ISpElement_GetInfo(element, *info) */
static void isp_GetInfo(ppc_cpu *c)
{
    element *e = find(R(0));
    gaddr info = R(1);
    if (!e) {
        RETURN(kISpElementNotInListErr);
        return;
    }
    if (info) {
        for (int i = 0; i < INFO_SIZE; i++)
            wr8(info + (gaddr)i, 0);
        wr32(info, e->label);
        wr32(info + 4, e->kind);
        for (int i = 0; i <= e->name[0]; i++)
            wr8(info + 8 + (gaddr)i, e->name[i]);
    }
    RETURN(0);
}

/* ISpElement_GetDevice(element, *device): a virtual element has none. */
static void isp_GetDevice(ppc_cpu *c)
{
    element *e = find(R(0));
    if (R(1))
        wr32(R(1), e && e->device >= 0 ? device_ref(e->device) : 0);
    RETURN(e ? 0 : kISpElementNotInListErr);
}

/* ISpElement_GetGroup(element, *group) */
static void isp_GetGroup(ppc_cpu *c)
{
    element *e = find(R(0));
    if (R(1))
        wr32(R(1), e ? e->group : 0);
    RETURN(e ? 0 : kISpElementNotInListErr);
}

/* ISpElement_GetConfigurationInfo(element, bufSize, *info): nothing to say. */
static void isp_GetConfigurationInfo(ppc_cpu *c)
{
    for (uint32_t i = 0; R(2) && i < R(1) && i < 64; i++)
        wr8(R(2) + i, 0);
    RETURN(find(R(0)) ? 0 : kISpElementNotInListErr);
}

/* ---- Element lists ---- */

#define MAX_LISTS 32
static struct {
    gaddr ref;
    gaddr items[MAX_ELEMENTS];
    int count;
    int next;  /* where the next GetNextEvent starts looking, so every element has its turn */
} lists[MAX_LISTS];
static int nlists;

static int list_of(gaddr ref)
{
    for (int i = 0; i < nlists; i++)
        if (ref && lists[i].ref == ref)
            return i;
    return -1;
}

static int new_list(void)
{
    for (int i = 0; i < nlists; i++)
        if (!lists[i].ref) {
            lists[i].ref = tb_NewPtr(4, true, true);
            lists[i].count = lists[i].next = 0;
            return i;
        }
    if (nlists >= MAX_LISTS)
        return -1;
    int l = nlists++;
    lists[l].ref = tb_NewPtr(4, true, true);
    lists[l].count = lists[l].next = 0;
    return l;
}

static void list_add(int l, gaddr item)
{
    for (int i = 0; i < lists[l].count; i++)
        if (lists[l].items[i] == item)
            return;
    if (lists[l].count < MAX_ELEMENTS)
        lists[l].items[lists[l].count++] = item;
}

/* ISpElementList_New(count, items, *list, flags) */
static void isp_ListNew(ppc_cpu *c)
{
    int l = new_list();
    if (l < 0) {
        RETURN(-108);
        return;
    }
    for (uint32_t i = 0; R(1) && i < R(0); i++)
        list_add(l, rd32(R(1) + i * 4));
    if (R(2))
        wr32(R(2), lists[l].ref);
    RETURN(0);
}

/* ISpElementList_Dispose(list) */
static void isp_ListDispose(ppc_cpu *c)
{
    int l = list_of(R(0));
    if (l >= 0) {
        tb_DisposePtr(lists[l].ref);
        lists[l].ref = 0;
        lists[l].count = 0;
    }
    RETURN(0);
}

/* ISpElementList_AddElements(list, refCon, count, items) */
static void isp_ListAdd(ppc_cpu *c)
{
    int l = list_of(R(0));
    for (uint32_t i = 0; l >= 0 && i < R(2); i++) {
        gaddr item = rd32(R(3) + i * 4);
        element *e = find(item);
        if (e)
            e->refcon = R(1);
        list_add(l, item);
    }
    RETURN(l >= 0 ? 0 : -50);
}

/* ISpElementList_RemoveElements(list, count, items) */
static void isp_ListRemove(ppc_cpu *c)
{
    int l = list_of(R(0));
    for (uint32_t i = 0; l >= 0 && i < R(1); i++) {
        gaddr item = rd32(R(2) + i * 4);
        for (int k = 0; k < lists[l].count; k++)
            if (lists[l].items[k] == item) {
                memmove(&lists[l].items[k], &lists[l].items[k + 1], (size_t)(lists[l].count - k - 1) * sizeof(gaddr));
                lists[l].count--;
                break;
            }
    }
    RETURN(l >= 0 ? 0 : -50);
}

/* ISpElementList_Extract(list, count, *found, items),
 * ExtractByKind(list, kind, count, *found, items) and
 * ExtractByLabel(list, label, ...): how many match, and as many of them
 * as there is room for. */
static void list_extract(ppc_cpu *c, int by, uint32_t what, uint32_t room, gaddr found, gaddr out)
{
    int l = list_of(R(0));
    uint32_t n = 0;
    for (int i = 0; l >= 0 && i < lists[l].count; i++) {
        element *e = find(lists[l].items[i]);
        if (by && (!e || (by == 1 ? e->kind : e->label) != what))
            continue;
        if (out && n < room)
            wr32(out + n * 4, lists[l].items[i]);
        n++;
    }
    if (found)
        wr32(found, n);
    RETURN(l < 0 ? -50 : n > room && out ? kISpBufferToSmallErr : 0);
}
static void isp_ListExtract(ppc_cpu *c) { list_extract(c, 0, 0, R(1), R(2), R(3)); }
static void isp_ListExtractByKind(ppc_cpu *c) { list_extract(c, 1, R(1), R(2), R(3), R(4)); }
static void isp_ListExtractByLabel(ppc_cpu *c) { list_extract(c, 2, R(1), R(2), R(3), R(4)); }

/* ISpElementList_GetNextEvent(list, bufSize, *event, *wasEvent) */
static void isp_ListGetNextEvent(ppc_cpu *c)
{
    read_pads();
    int l = list_of(R(0));
    bool got = false;
    for (int k = 0; l >= 0 && k < lists[l].count && !got; k++) {
        int i = (lists[l].next + k) % lists[l].count;
        element *e = find(lists[l].items[i]);
        if (e && (got = next_event(e, R(2))))
            lists[l].next = (i + 1) % lists[l].count;
    }
    if (R(3))
        wr8(R(3), got);
    RETURN(l >= 0 ? 0 : -50);
}

static void isp_ListFlush(ppc_cpu *c)
{
    read_pads();
    int l = list_of(R(0));
    for (int i = 0; l >= 0 && i < lists[l].count; i++) {
        element *e = find(lists[l].items[i]);
        if (e) {
            e->last = state_of(e);
            if (e->kind == kDelta)
                delta_state(e, true);
        }
    }
    RETURN(0);
}

/* ISpGetGlobalElementList(*list): every device's elements. */
static gaddr global_list;
static void isp_GetGlobalElementList(ppc_cpu *c)
{
    read_pads();
    int l = list_of(global_list);
    if (l < 0 && (l = new_list()) >= 0)
        global_list = lists[l].ref;
    for (int d = 0; l >= 0 && d < NDEVICES; d++) {
        if (!device_present(d))
            continue;
        device_elements(d);
        for (int i = 0; i < nelements; i++)
            if (elements[i].device == d)
                list_add(l, elements[i].ref);
    }
    if (R(0))
        wr32(R(0), l >= 0 ? lists[l].ref : 0);
    RETURN(l >= 0 ? 0 : -108);
}

/* ISpDevice_GetElementList(device, *list) */
static void isp_DeviceGetElementList(ppc_cpu *c)
{
    read_pads();
    int d = device_of(R(0));
    int l = d >= 0 ? new_list() : -1;
    if (l >= 0) {
        device_elements(d);
        for (int i = 0; i < nelements; i++)
            if (elements[i].device == d)
                list_add(l, elements[i].ref);
    }
    if (R(1))
        wr32(R(1), l >= 0 ? lists[l].ref : 0);
    RETURN(d < 0 ? -50 : l < 0 ? -108 : 0);
}

/* ---- Devices ---- */

/* ISpDevices_Extract(count, *found, devices) and ExtractByClass(class,
 * count, *found, devices), ExtractByIdentifier(identifier, ...). */
static void devices_extract(ppc_cpu *c, bool by_class, uint32_t cls, uint32_t room, gaddr found, gaddr out)
{
    read_pads();
    uint32_t n = 0;
    for (int d = 0; d < NDEVICES; d++) {
        if (!device_present(d) || (by_class && device_class(d) != cls))
            continue;
        if (out && n < room)
            wr32(out + n * 4, device_ref(d));
        n++;
    }
    if (found)
        wr32(found, n);
    RETURN(n > room && out ? kISpBufferToSmallErr : 0);
}
static void isp_DevicesExtract(ppc_cpu *c) { devices_extract(c, false, 0, R(0), R(1), R(2)); }
static void isp_DevicesExtractByClass(ppc_cpu *c) { devices_extract(c, true, R(0), R(1), R(2), R(3)); }
static void isp_DevicesExtractByIdentifier(ppc_cpu *c) { devices_extract(c, true, R(0), R(1), R(2), R(3)); }

static void write_pstring(gaddr at, const char *utf8)
{
    uint8_t s[256];
    size_t n = utf8_to_macroman(utf8, s, sizeof s);
    if (n > 63)
        n = 63;
    wr8(at, (uint8_t)n);
    for (size_t i = 0; i < n; i++)
        wr8(at + 1 + (gaddr)i, s[i]);
}

/* ISpDevice_GetDefinition(device, bufSize, *definition) */
static void isp_DeviceGetDefinition(ppc_cpu *c)
{
    read_pads();
    int d = device_of(R(0));
    gaddr def = R(2);
    if (d < 0) {
        RETURN(-50);
        return;
    }
    if (R(1) < DEFINITION_SIZE) {
        RETURN(kISpBufferToSmallErr);
        return;
    }
    for (int i = 0; i < DEFINITION_SIZE; i++)
        wr8(def + (gaddr)i, 0);
    write_pstring(def, d == DEV_KEYBOARD ? "Keyboard" : d == DEV_MOUSE ? "Mouse"
                       : pads[d - DEV_PAD].name[0] ? pads[d - DEV_PAD].name : "Game Controller");
    write_pstring(def + 64, d < DEV_PAD ? "Apple Computer, Inc." : "");
    wr32(def + 128, 0x01008000);
    wr32(def + 132, device_class(d));
    wr32(def + 136, device_class(d));
    wr32(def + 140, (uint32_t)d + 1);
    RETURN(0);
}

/* ISpDevices_Activate(count, devices) and Deactivate: a deactivated
 * device's elements are idle. */
static void set_active(ppc_cpu *c, bool on)
{
    for (uint32_t i = 0; R(1) && i < R(0); i++) {
        int d = device_of(rd32(R(1) + i * 4));
        if (d >= 0)
            devices[d].inactive = !on;
    }
    RETURN(0);
}
static void isp_DevicesActivate(ppc_cpu *c) { set_active(c, true); }
static void isp_DevicesDeactivate(ppc_cpu *c) { set_active(c, false); }

/* ISpDevices_ActivateClass(class) and DeactivateClass */
static void set_class_active(ppc_cpu *c, bool on)
{
    for (int d = 0; d < NDEVICES; d++)
        if (device_class(d) == R(0))
            devices[d].inactive = !on;
    RETURN(0);
}
static void isp_DevicesActivateClass(ppc_cpu *c) { set_class_active(c, true); }
static void isp_DevicesDeactivateClass(ppc_cpu *c) { set_class_active(c, false); }

/* ---- The rest ---- */

static void isp_noErr(ppc_cpu *c) { RETURN(0); }

/* 1.7, a NumVersion */
static void isp_GetVersion(ppc_cpu *c)
{
    ppc_struct_result(c, R(0), 0x01708000, 4);
    c->pc = c->lr;
}

/* ISpUptime(): an AbsoluteTime, through the result pointer in r3. */
static void isp_Uptime(ppc_cpu *c)
{
    uint64_t us = machine_time_ns() / 1000;
    if (R(0)) {
        wr32(R(0), (uint32_t)(us >> 32));
        wr32(R(0) + 4, (uint32_t)us);
    }
    c->pc = c->lr;
}

/* ISpTimeToMicroseconds(*time, *microseconds): ours count microseconds. */
static void isp_TimeToMicroseconds(ppc_cpu *c)
{
    if (R(0) && R(1)) {
        wr32(R(1), rd32(R(0)));
        wr32(R(1) + 4, rd32(R(0) + 4));
    }
    RETURN(0);
}

/* ISpConfigure(eventProc, *accepted): no dialog; the mapping stands. */
static void isp_Configure(ppc_cpu *c)
{
    if (R(1))
        wr8(R(1), 1);
    RETURN(0);
}

static void isp_Suspend(ppc_cpu *c)
{
    suspended = true;
    RETURN(0);
}

/* Back from a suspension: what changed meanwhile is no event. */
static void isp_Resume(ppc_cpu *c)
{
    suspended = false;
    read_pads();
    for (int i = 0; i < nelements; i++) {
        elements[i].last = state_of(&elements[i]);
        if (elements[i].kind == kDelta)
            delta_state(&elements[i], true);
    }
    RETURN(0);
}

/* ISpPlotAppIconSuite(rect, alignment, transform, iconSuiteResourceId) */
static void isp_PlotAppIconSuite(ppc_cpu *c)
{
    RETURN(tb_PlotIconID(R(0), (int16_t)R(1), (int16_t)R(2), (int16_t)R(3)));
}

/* ISpElement_DisposeVirtual(count, elements): their references stay
 * unique, so nothing to do. */

void inputsprocket_reset(void)
{
    nelements = spare = spare_button = nlists = 0;
    ref_block = device_block = global_list = 0;
    suspended = false;
    memset(lists, 0, sizeof lists);
    memset(devices, 0, sizeof devices);
}

const ppc_export isp_exports[] = {
    { "ISpConfigure", isp_Configure },
    { "ISpDevice_GetDefinition", isp_DeviceGetDefinition },
    { "ISpDevice_GetElementList", isp_DeviceGetElementList },
    { "ISpDevices_Activate", isp_DevicesActivate },
    { "ISpDevices_ActivateClass", isp_DevicesActivateClass },
    { "ISpDevices_Deactivate", isp_DevicesDeactivate },
    { "ISpDevices_DeactivateClass", isp_DevicesDeactivateClass },
    { "ISpDevices_Extract", isp_DevicesExtract },
    { "ISpDevices_ExtractByClass", isp_DevicesExtractByClass },
    { "ISpDevices_ExtractByIdentifier", isp_DevicesExtractByIdentifier },
    { "ISpElementList_AddElements", isp_ListAdd },
    { "ISpElementList_Dispose", isp_ListDispose },
    { "ISpElementList_Extract", isp_ListExtract },
    { "ISpElementList_ExtractByKind", isp_ListExtractByKind },
    { "ISpElementList_ExtractByLabel", isp_ListExtractByLabel },
    { "ISpElementList_Flush", isp_ListFlush },
    { "ISpElementList_GetNextEvent", isp_ListGetNextEvent },
    { "ISpElementList_New", isp_ListNew },
    { "ISpElementList_RemoveElements", isp_ListRemove },
    { "ISpElement_DisposeVirtual", isp_noErr },
    { "ISpElement_Flush", isp_Flush },
    { "ISpElement_GetComplexState", isp_GetComplexState },
    { "ISpElement_GetConfigurationInfo", isp_GetConfigurationInfo },
    { "ISpElement_GetDevice", isp_GetDevice },
    { "ISpElement_GetGroup", isp_GetGroup },
    { "ISpElement_GetInfo", isp_GetInfo },
    { "ISpElement_GetNextEvent", isp_GetNextEvent },
    { "ISpElement_GetSimpleState", isp_GetSimpleState },
    { "ISpElement_NewVirtual", isp_NewVirtual },
    { "ISpElement_NewVirtualFromNeeds", isp_NewVirtualFromNeeds },
    { "ISpGetGlobalElementList", isp_GetGlobalElementList },
    { "ISpGetVersion", isp_GetVersion },
    { "ISpInit", isp_noErr },
    { "ISpPlotAppIconSuite", isp_PlotAppIconSuite },
    { "ISpResume", isp_Resume },
    { "ISpShutdown", isp_noErr },
    { "ISpStartup", isp_noErr },
    { "ISpStop", isp_noErr },
    { "ISpSuspend", isp_Suspend },
    { "ISpTickle", isp_noErr },
    { "ISpTimeToMicroseconds", isp_TimeToMicroseconds },
    { "ISpUptime", isp_Uptime },
};
const unsigned isp_export_count = sizeof isp_exports / sizeof isp_exports[0];
