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
 * them.  Delta needs follow the mouse's own motion (not the cursor), and
 * once the game turns the mouse on, x and y axes are pushed by it as by a
 * joystick (Apeiron's ship) and the fire button is its button as well.
 * The configuration dialog accepts this mapping.
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
#include <stdio.h>
#include <string.h>

#include "forks.h"
#include "lowmem_gen.h"
#include "machine.h"
#include "ppc_bridge.h"
#include "qd_internal.h"
#include "tb_internal.h"
#include "trap.h"

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
    bool mouse;     /* the mouse button, or a delta or an x or y axis following the mouse */
    uint32_t last;  /* the state last reported */
    int32_t mouse_at; /* a delta or axis: the mouse's motion (ev_mouse_raw) when it was last read */
    int32_t mouse_carry; /* an axis: motion beyond its end, for the next reads */
    uint8_t def_key, def_key2; /* as the need was first mapped (the dialog's Defaults) */
    uint32_t refcon;
} element;

#define MAX_ELEMENTS 256
static element elements[MAX_ELEMENTS];
static int nelements, spare, spare_button;
static gaddr ref_block;
static bool suspended;
/* The game turned the mouse on (ISpDevices_ActivateClass or Activate), as
 * InputSprocket wanted before the mouse drove its axes and fire button;
 * the player can turn it off in the configuration dialog. */
static bool mouse_wanted, mouse_refused;
/* The needs the game made last: what the configuration dialog shows. */
static int config_first, config_count;

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
    case FOURCC('x', 'a', 'x', 'i'): e->stick = MACHINE_PAD_LX; horizontal = true; e->mouse = true; break;
    case FOURCC('y', 'a', 'x', 'i'): e->stick = MACHINE_PAD_LY; horizontal = false; e->mouse = true; break;
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

static gaddr km; /* a KeyMap to read the keys into */

static bool key_down(uint8_t code)
{
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

static bool mouse_down(void)
{
    if (devices[DEV_MOUSE].inactive)
        return false;
    int32_t h, v;
    bool down;
    ev_mouse_raw(&h, &v, &down, false);
    return down;
}

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
    if (key_down(e->key) || pad_button(e, e->pad) || (e->mouse && (e->device >= 0 || (mouse_wanted && !mouse_refused)) && mouse_down()))
        return true;
    if (e->stick >= 0) {
        float v = pad_axis(e, e->stick / 2);
        return (e->stick & 1) ? v > PRESS_PCT / 100.0f : v < -PRESS_PCT / 100.0f;
    }
    return false;
}

/* The mouse's own motion on a delta's axis, as InputSprocket reads it
 * from the mouse: not the cursor, which stops at the screen's edge and
 * which a program may be moving, nor low memory, which a game's loop
 * that never calls the Event Manager never brings up to date. */
static int32_t mouse_motion(const element *e, bool capture)
{
    int32_t h, v;
    bool down;
    ev_mouse_raw(&h, &v, &down, capture);
    return e->label == FOURCC('y', 'a', 'x', 'i') ? v : h;
}

/* A delta's movement since it was last read (Fixed points; up positive). */
static int32_t delta_state(element *e, bool consume)
{
    if (devices[DEV_MOUSE].inactive)
        return 0;
    bool y = e->label == FOURCC('y', 'a', 'x', 'i');
    int32_t at = mouse_motion(e, true);
    int32_t d = y ? e->mouse_at - at : at - e->mouse_at;
    if (consume)
        e->mouse_at = at;
    return d * 65536;
}

/* Pixels of the mouse's motion between two reads that push an axis to
 * its end (Apeiron's ship then moves about a quarter as far as the mouse
 * did: more was touchy), and the most kept for later reads when it moves
 * faster. */
enum { MOUSE_AXIS_FULL = 32, MOUSE_AXIS_CARRY = 4 * MOUSE_AXIS_FULL };

/* An x or y axis need follows the mouse as a joystick would: its motion
 * since the axis was last read sets how far the axis is pushed, and the
 * axis is back in the middle when the mouse stops (Apeiron moves its ship
 * at the axis's speed).  Keys, a pad or a stick held override it. */
static uint32_t mouse_axis_state(element *e)
{
    bool following = mouse_wanted && !mouse_refused && !devices[DEV_MOUSE].inactive;
    int32_t at = mouse_motion(e, following);
    if (!following)
        at = e->mouse_at, e->mouse_carry = 0;
    int32_t d = at - e->mouse_at;
    e->mouse_at = at;
    if (e->label == FOURCC('y', 'a', 'x', 'i'))
        d = -d; /* down the screen is down the axis */
    d += e->mouse_carry;
    int32_t push = d < -MOUSE_AXIS_FULL ? -MOUSE_AXIS_FULL : d > MOUSE_AXIS_FULL ? MOUSE_AXIS_FULL : d;
    int32_t carry = d - push;
    e->mouse_carry = carry < -MOUSE_AXIS_CARRY ? -MOUSE_AXIS_CARRY : carry > MOUSE_AXIS_CARRY ? MOUSE_AXIS_CARRY : carry;
    uint32_t held = axis_state(e, e->stick, e->key, e->key2, e->pad, e->pad2);
    if (held != (uint32_t)AXIS_MID)
        return held;
    return axis_value((float)push / MOUSE_AXIS_FULL);
}

static uint32_t state_of(element *e)
{
    if (suspended)
        return e->kind == kAxis ? (uint32_t)AXIS_MID : 0;
    switch (e->kind) {
    case kDPad: case kMovement: return dpad_state(e);
    case kAxis:
        return e->mouse ? mouse_axis_state(e) : axis_state(e, e->stick, e->key, e->key2, e->pad, e->pad2);
    case kDelta: return (uint32_t)delta_state(e, false);
    default: return button_state(e);
    }
}

/* ---- Making elements ---- */

static void reset_delta(element *e)
{
    e->mouse_at = mouse_motion(e, false);
    e->mouse_carry = 0;
}

static void load_controls(void);

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
        e->def_key = e->key, e->def_key2 = e->key2;
        LOGF(LOG_TRAPS, "  need \"%s\": %c%c%c%c %c%c%c%c, player %d, key %02X, mouse %d\n", name,
             (int)(e->kind >> 24), (int)(e->kind >> 16 & 0xFF), (int)(e->kind >> 8 & 0xFF), (int)(e->kind & 0xFF),
             (int)(e->label >> 24), (int)(e->label >> 16 & 0xFF), (int)(e->label >> 8 & 0xFF),
             (int)(e->label & 0xFF), e->player, e->key, e->mouse);
        reset_delta(e);
        e->last = state_of(e);
        if (out)
            wr32(out + i * 4, e->ref);
    }
    /* A game steered by the mouse fires with its button too (Apeiron). */
    int first = nelements - (int)count;
    config_first = first, config_count = (int)count;
    bool steered = false;
    for (int i = first; i < nelements; i++)
        steered |= elements[i].kind == kDelta || (elements[i].kind == kAxis && elements[i].mouse);
    for (int i = first; steered && i < nelements; i++)
        if (elements[i].kind == kButton && elements[i].key == K_SPACE) {
            elements[i].mouse = true;
            break;
        }
    load_controls();
    for (int i = first; i < nelements; i++)
        elements[i].last = state_of(&elements[i]);
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
        if (d == DEV_MOUSE)
            mouse_wanted = on;
    }
    RETURN(0);
}
static void isp_DevicesActivate(ppc_cpu *c) { set_active(c, true); }
static void isp_DevicesDeactivate(ppc_cpu *c) { set_active(c, false); }

/* ISpDevices_ActivateClass(class) and DeactivateClass */
static void set_class_active(ppc_cpu *c, bool on)
{
    LOGF(LOG_TRAPS, "  class %c%c%c%c %s\n", (int)(R(0) >> 24), (int)(R(0) >> 16 & 0xFF), (int)(R(0) >> 8 & 0xFF),
         (int)(R(0) & 0xFF), on ? "on" : "off");
    for (int d = 0; d < NDEVICES; d++)
        if (device_class(d) == R(0))
            devices[d].inactive = !on;
    if (R(0) == kClassMouse)
        mouse_wanted = on;
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

/* ---- The configuration dialog ---- */

static bool configurable(const element *e) { return e->kind == kButton || e->kind == kAxis; }

/* The player's choices for a program, kept as InputSprocket kept them: a
 * file in the Preferences folder ("<program> Controls"), each need's keys
 * by its place, kind, label and name, and whether the mouse steers. */
static bool controls_path(char *out, size_t size)
{
    gaddr tmp = tb_NewPtr(2 + 4 + 64, false, true);
    if (!tmp)
        return false;
    gaddr vref = tmp, dir = tmp + 2, name = tmp + 6;
    char app[32];
    int n = rd8(LM_CurApName);
    n = n > 22 ? 22 : n;
    for (int i = 0; i < n; i++)
        app[i] = (char)rd8(LM_CurApName + 1 + (gaddr)i);
    app[n] = 0;
    char file[40];
    snprintf(file, sizeof file, "%s Controls", n ? app : "Game");
    pstr_from_c(name, file);
    bool ok = !tb_FindFolder(-32768, (int32_t)fourcc("pref"), 1, vref, dir) &&
              fm_host_path((int16_t)rd16(vref), (int32_t)rd32(dir), name, true, out, size);
    tb_DisposePtr(tmp);
    return ok;
}

static void save_controls(void)
{
    char path[1024];
    if (!controls_path(path, sizeof path))
        return;
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "mouse %d\n", !mouse_refused);
    for (int i = 0; i < config_count; i++) {
        const element *e = &elements[config_first + i];
        if (configurable(e))
            fprintf(f, "need %d %08X %08X %02X %02X %.*s\n", i, e->kind, e->label, e->key, e->key2, e->name[0],
                    (const char *)e->name + 1);
    }
    fclose(f);
}

/* The saved choices, for needs made as they were when they were saved. */
static void load_controls(void)
{
    char path[1024], line[256];
    if (!controls_path(path, sizeof path))
        return;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        int on, i, used;
        unsigned kind, label, key, key2;
        if (sscanf(line, "mouse %d", &on) == 1) {
            mouse_refused = !on;
        } else if (sscanf(line, "need %d %x %x %x %x %n", &i, &kind, &label, &key, &key2, &used) == 5 && i >= 0 &&
                   i < config_count) {
            element *e = &elements[config_first + i];
            char *name = line + used;
            name[strcspn(name, "\r\n")] = 0;
            if (e->kind == kind && e->label == label && strlen(name) == e->name[0] &&
                !memcmp(name, e->name + 1, e->name[0]) && configurable(e))
                e->key = (uint8_t)key, e->key2 = (uint8_t)key2;
        }
    }
    fclose(f);
}

/* A key's name (a US keyboard's), for the dialog. */
static const char *key_name(uint8_t code)
{
    static const char *const names[128] = {
        [0x00] = "A", [0x01] = "S", [0x02] = "D", [0x03] = "F", [0x04] = "H", [0x05] = "G", [0x06] = "Z",
        [0x07] = "X", [0x08] = "C", [0x09] = "V", [0x0B] = "B", [0x0C] = "Q", [0x0D] = "W", [0x0E] = "E",
        [0x0F] = "R", [0x10] = "Y", [0x11] = "T", [0x12] = "1", [0x13] = "2", [0x14] = "3", [0x15] = "4",
        [0x16] = "6", [0x17] = "5", [0x18] = "=", [0x19] = "9", [0x1A] = "7", [0x1B] = "-", [0x1C] = "8",
        [0x1D] = "0", [0x1E] = "]", [0x1F] = "O", [0x20] = "U", [0x21] = "[", [0x22] = "I", [0x23] = "P",
        [0x24] = "Return", [0x25] = "L", [0x26] = "J", [0x27] = "'", [0x28] = "K", [0x29] = ";", [0x2A] = "\\",
        [0x2B] = ",", [0x2C] = "/", [0x2D] = "N", [0x2E] = "M", [0x2F] = ".", [0x30] = "Tab", [0x31] = "Space",
        [0x32] = "`", [0x33] = "Delete", [0x35] = "Escape", [0x37] = "Command", [0x38] = "Shift",
        [0x3A] = "Option", [0x3B] = "Control", [0x41] = "Keypad .", [0x43] = "Keypad *", [0x45] = "Keypad +",
        [0x47] = "Clear", [0x4B] = "Keypad /", [0x4C] = "Enter", [0x4E] = "Keypad -", [0x51] = "Keypad =",
        [0x52] = "Keypad 0", [0x53] = "Keypad 1", [0x54] = "Keypad 2", [0x55] = "Keypad 3", [0x56] = "Keypad 4",
        [0x57] = "Keypad 5", [0x58] = "Keypad 6", [0x59] = "Keypad 7", [0x5B] = "Keypad 8", [0x5C] = "Keypad 9",
        [0x60] = "F5", [0x61] = "F6", [0x62] = "F7", [0x63] = "F3", [0x64] = "F8", [0x65] = "F9",
        [0x67] = "F11", [0x6D] = "F10", [0x6F] = "F12", [0x73] = "Home", [0x74] = "Page Up",
        [0x75] = "Forward Delete", [0x76] = "F4", [0x77] = "End", [0x78] = "F2", [0x79] = "Page Down",
        [0x7A] = "F1", [0x7B] = "Left Arrow", [0x7C] = "Right Arrow", [0x7D] = "Down Arrow", [0x7E] = "Up Arrow",
    };
    if (code == K_NONE)
        return "none";
    return code < 128 && names[code] ? names[code] : "a key";
}

/* Items: OK, Cancel, Defaults, the mouse box, the heading, then a name and
 * a button for each need. */
enum { itemOK = 1, itemCancel, itemDefaults, itemMouse, itemHeading, itemNeeds, CONFIG_ROWS = 14 };
enum { CAPTURED = 999 }; /* ModalDialog's item when a key was taken */

static gaddr config_dialog;
static int capturing = -1, capture_stage; /* the need taking a key, and which of an axis's two */
static uint8_t capture_keys[16];          /* the keys down when it started */

static bool uses_mouse(void)
{
    for (int i = config_first; i < config_first + config_count; i++)
        if (elements[i].mouse || elements[i].kind == kDelta)
            return true;
    return false;
}

static int16_t config_item(gaddr p, qrect r, uint8_t type, const char *text)
{
    size_t n = strlen(text);
    wr32(p, 0);
    qrect_put(p + 4, r);
    wr8(p + 12, type);
    wr8(p + 13, (uint8_t)n);
    gput(p + 14, text, (uint32_t)n);
    return (int16_t)(14 + (((uint32_t)n + 1) & ~1u));
}

static gaddr config_handle(int item)
{
    gaddr scratch = tb_NewPtr(16, false, true);
    tb_GetDialogItem(config_dialog, (int16_t)item, scratch, scratch + 2, scratch + 6);
    gaddr h = rd32(scratch + 2);
    tb_DisposePtr(scratch);
    return h;
}

/* What a need's button says: its keys, the mouse, or what to press. */
static void config_label(const element *e, int row, char *out, size_t size)
{
    bool mouse = e->mouse && mouse_wanted && !mouse_refused;
    if (capturing == row)
        snprintf(out, size, e->kind == kAxis ? (capture_stage ? (e->label == FOURCC('y', 'a', 'x', 'i') ? "Press a key for up" : "Press a key for right") : (e->label == FOURCC('y', 'a', 'x', 'i') ? "Press a key for down" : "Press a key for left")) : "Press a key");
    else if (e->kind == kAxis)
        snprintf(out, size, "%s, %s%s", key_name(e->key), key_name(e->key2), mouse ? ", mouse" : "");
    else if (e->kind == kButton)
        snprintf(out, size, "%s%s", key_name(e->key), mouse ? ", mouse button" : "");
    else if (e->kind == kDelta)
        snprintf(out, size, "Mouse");
    else
        snprintf(out, size, "Arrow keys");
}

static void config_show(void)
{
    char text[64];
    gaddr title = tb_NewPtr(256, false, true);
    int rows = config_count < CONFIG_ROWS ? config_count : CONFIG_ROWS;
    for (int row = 0; row < rows; row++) {
        config_label(&elements[config_first + row], row, text, sizeof text);
        pstr_from_c(title, text);
        gaddr h = config_handle(itemNeeds + row * 2 + 1);
        if (h)
            tb_SetControlTitle(h, title);
    }
    gaddr box = config_handle(itemMouse);
    if (box)
        tb_SetControlValue(box, !mouse_refused);
    tb_DisposePtr(title);
}

/* A key newly down since the capture started (modifiers too, which make
 * no key event): KeyMap's bit order, as GetKeys gives it. */
static int new_key(void)
{
    gaddr km_now = tb_NewPtr(16, true, true);
    tb_GetKeys(km_now);
    int found = -1;
    for (int code = 0; code < 128 && found < 0; code++) {
        bool down = (rd8(km_now + (code >> 3)) >> (code & 7)) & 1, was = (capture_keys[code >> 3] >> (code & 7)) & 1;
        if (down && !was && code != 0x39 /* caps lock */)
            found = code;
    }
    for (int i = 0; i < 16; i++) /* a key let go may be pressed again */
        capture_keys[i] &= rd8(km_now + (gaddr)i);
    tb_DisposePtr(km_now);
    return found;
}

static void start_capture(int row)
{
    capturing = row;
    capture_stage = 0;
    gaddr km_now = tb_NewPtr(16, true, true);
    tb_GetKeys(km_now);
    gget(capture_keys, km_now, 16);
    tb_DisposePtr(km_now);
}

/* The dialog's filter: while a need waits for a key, the next key pressed
 * is its (no key reaches the dialog then, Return and Escape included). */
static void config_filter(cpu68k *c, uint16_t tw)
{
    (void)tw;
    uint32_t ret = cpu_pop32(c);
    gaddr itemp = cpu_pop32(c), evt = cpu_pop32(c), d = cpu_pop32(c);
    (void)d;
    bool r = false;
    if (capturing >= 0) {
        int code = new_key();
        if (code >= 0) {
            element *e = &elements[config_first + capturing];
            if (e->kind == kAxis && capture_stage == 0) {
                e->key = (uint8_t)code;
                capture_stage = 1;
            } else {
                if (e->kind == kAxis)
                    e->key2 = (uint8_t)code;
                else
                    e->key = (uint8_t)code;
                capturing = -1;
            }
            wr16(itemp, CAPTURED);
            r = true;
        } else if (rd16(evt) == 3 || rd16(evt) == 5) { /* keyDown, autoKey: taken above or next time */
            wr16(evt, 0);
        }
    }
    wr16(c->a[7], r ? 0x0100 : 0);
    cpu_push32(c, ret);
}

/* ISpConfigure(eventProc, *accepted): the needs the game made last, each
 * with its keys to change (an axis takes two, its low end first), whether
 * the mouse steers, and Defaults; Cancel puts things back. */
static void isp_Configure(ppc_cpu *c)
{
    gaddr accepted = R(1);
    int rows = config_count < CONFIG_ROWS ? config_count : CONFIG_ROWS;
    bool mouse = uses_mouse();
    int list_bottom = 60 + rows * 24;
    int height = list_bottom + 8 + 20 + 16 + 20 + 14;
    int width = 460;
    int16_t top = (int16_t)((g_screen_height - height) / 3), left = (int16_t)((g_screen_width - width) / 2);
    if (top < 40)
        top = 40;
    gaddr ditl = tb_NewHandle(2 + (itemNeeds + rows * 2) * (14 + 64), false, true);
    gaddr scratch = tb_NewPtr(32, false, true);
    if (!ditl || !scratch) {
        if (accepted)
            wr8(accepted, 1);
        RETURN(0);
        return;
    }
    int16_t box = (int16_t)(list_bottom + 8), bottom = (int16_t)(box + 36);
    gaddr p = rd32(ditl) + 2;
    p += (gaddr)config_item(p, (qrect){ bottom, 370, (int16_t)(bottom + 20), 440 }, 4, "OK");
    p += (gaddr)config_item(p, (qrect){ bottom, 288, (int16_t)(bottom + 20), 358 }, 4, "Cancel");
    p += (gaddr)config_item(p, (qrect){ bottom, 20, (int16_t)(bottom + 20), 100 }, 4, "Defaults");
    p += (gaddr)config_item(p, (qrect){ box, 20, (int16_t)(box + 18), 440 }, mouse ? 5 : 5 | 0x80,
                            mouse ? "Use the mouse to steer and fire" : "");
    p += (gaddr)config_item(p, (qrect){ 14, 20, 50, 440 }, 8 | 0x80,
                            rows ? "Click a control's keys, then press the key to use (an axis takes two)."
                                 : "This game has no controls to configure.");
    for (int row = 0; row < rows; row++) {
        element *e = &elements[config_first + row];
        char name[64];
        int n = e->name[0];
        memcpy(name, e->name + 1, (size_t)n);
        name[n] = 0;
        int16_t y = (int16_t)(60 + row * 24);
        p += (gaddr)config_item(p, (qrect){ (int16_t)(y + 2), 20, (int16_t)(y + 18), 180 }, 8 | 0x80, name);
        p += (gaddr)config_item(p, (qrect){ y, 186, (int16_t)(y + 20), 440 }, configurable(e) ? 4 : 4 | 0x80, "");
    }
    wr16(rd32(ditl), (uint16_t)(itemNeeds + rows * 2 - 1));
    qrect_put(scratch, (qrect){ top, left, (int16_t)(top + height), (int16_t)(left + width) });
    config_dialog = tb_NewDialog(0, scratch, scratch + 8, 1, 1 /* dBoxProc */, 0xFFFFFFFF, 0, 0, ditl);
    if (!config_dialog) {
        tb_DisposePtr(scratch);
        if (accepted)
            wr8(accepted, 1);
        RETURN(0);
        return;
    }
    if (!mouse) {
        gaddr box = config_handle(itemMouse);
        if (box)
            tb_HiliteControl(box, 255);
    }
    tb_SetDialogDefaultItem(config_dialog, itemOK);
    tb_SetDialogCancelItem(config_dialog, itemCancel);
    static gaddr filter_stub;
    if (!filter_stub)
        filter_stub = native_register(config_filter, "ISpConfigureFilter", 0);
    /* What Cancel puts back. */
    uint8_t keys[CONFIG_ROWS][2];
    for (int row = 0; row < rows; row++)
        keys[row][0] = elements[config_first + row].key, keys[row][1] = elements[config_first + row].key2;
    bool was_refused = mouse_refused;
    capturing = -1;
    config_show();
    gaddr hit = scratch + 16;
    int16_t n;
    do {
        tb_ModalDialog(filter_stub, hit);
        n = (int16_t)rd16(hit);
        if (n >= itemNeeds && (n - itemNeeds) % 2 == 1) {
            int row = (n - itemNeeds) / 2;
            if (configurable(&elements[config_first + row]))
                start_capture(row);
        } else if (n == itemMouse && mouse) {
            mouse_refused = !mouse_refused;
        } else if (n == itemDefaults) {
            capturing = -1;
            for (int row = 0; row < rows; row++)
                elements[config_first + row].key = elements[config_first + row].def_key,
                elements[config_first + row].key2 = elements[config_first + row].def_key2;
            mouse_refused = false;
        }
        config_show();
    } while (n != itemOK && n != itemCancel);
    capturing = -1;
    if (n == itemCancel) {
        for (int row = 0; row < rows; row++)
            elements[config_first + row].key = keys[row][0], elements[config_first + row].key2 = keys[row][1];
        mouse_refused = was_refused;
    } else {
        save_controls();
    }
    tb_DisposeDialog(config_dialog);
    config_dialog = 0;
    tb_DisposePtr(scratch);
    if (accepted)
        wr8(accepted, n == itemOK);
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
        if (elements[i].kind == kAxis && elements[i].mouse) /* the mouse's moves meanwhile are none */
            reset_delta(&elements[i]);
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
    ref_block = device_block = global_list = km = 0;
    suspended = mouse_wanted = mouse_refused = false;
    config_first = config_count = 0;
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
