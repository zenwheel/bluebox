/*
 * The Carbon Event Manager (CarbonEvents.h), as CarbonLib 1.1-1.3 gave it
 * on Mac OS 8.6 and 9: events as reference-counted objects with typed
 * parameters, targets (the application, windows, controls, menus, the
 * user focus and the event dispatcher) with chains of handlers, a queue,
 * timers, and the event loop that runs them.
 *
 * The events come from Blue Box's own queue (events.c): ReceiveNextEvent
 * takes them through WaitNextEvent and makes each a Carbon event (keeping
 * the EventRecord it was), and the dispatcher target routes it: a click to
 * the menu bar or to the window under it (kEventWindowClick...Rgn, with
 * the dragging, sizing, zooming and closing done by default), keys to the
 * user focus, updates and activates to their window, high-level events to
 * the Apple Event Manager.  A program that keeps to WaitNextEvent sees its
 * events as before; its timers fire and the events it posts are sent while
 * it waits (carbon_event_loop_turn).
 *
 * Handlers run in order, the most recently installed first, then the
 * target's standard handler, then the parent target's (a control's
 * window's, a window's application's).  Everything here is PowerPC only:
 * CarbonLib had no 68k side.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "carbon_events.h"
#include "ppc_bridge.h"
#include "qd_internal.h"
#include "tb_internal.h"

uint16_t ev_modifiers(void); /* events.c */

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; return; } while (0)

enum {
    eventAlreadyPostedErr = -9860,
    eventHandlerAlreadyInstalledErr = -9866,
    eventInternalErr = -9868,
    eventParameterNotFoundErr = -9870,
    eventLoopTimedOutErr = -9875,
    eventLoopQuitErr = -9876,
    eventNotInQueueErr = -9877,
    eventHotKeyExistsErr = -9878,
    eventHotKeyInvalidErr = -9879,
    errAECoercionFail = -1700,
    errInvalidWindowRef = -5600,
    errWindowNotInModalState = -5622,
    errWindowAlreadyInModalState = -5623,
};

/* Kinds by class. */
enum { kEventMouseDown = 1, kEventMouseUp = 2, kEventMouseMoved = 5 };
enum { kEventRawKeyDown = 1, kEventRawKeyRepeat = 2, kEventRawKeyUp = 3, kEventHotKeyPressed = 5 };
enum { kEventAppActivated = 1, kEventAppDeactivated = 2, kEventAppQuit = 3 };
enum { kEventAppleEvent = 1 };
enum {
    kEventWindowUpdate = 1, kEventWindowDrawContent = 2, kEventWindowActivated = 5, kEventWindowDeactivated = 6,
    kEventWindowBoundsChanged = 27, kEventWindowResizeCompleted = 29,
    kEventWindowClickDragRgn = 32, kEventWindowClickResizeRgn = 33, kEventWindowClickCollapseRgn = 34,
    kEventWindowClickCloseRgn = 35, kEventWindowClickZoomRgn = 36, kEventWindowClickContentRgn = 37,
    kEventWindowClickProxyIconRgn = 38,
    kEventWindowCollapse = 66, kEventWindowCollapsed = 67, kEventWindowExpand = 69, kEventWindowExpanded = 70,
    kEventWindowClose = 72, kEventWindowClosed = 73, kEventWindowZoom = 75, kEventWindowZoomed = 76,
    kEventWindowGetMinimumSize = 81, kEventWindowGetMaximumSize = 82, kEventWindowHandleContentClick = 85,
    kEventWindowFocusAcquired = 200, kEventWindowFocusRelinquish = 201,
};
enum { kEventMenuBeginTracking = 1, kEventMenuEndTracking = 2, kEventMenuEnableItems = 8 };
enum { kEventCommandProcess = 1 };
enum { kEventControlHit = 1 };

/* Parameter names and types. */
enum {
    kEventParamDirectObject = 0x2D2D2D2D, /* '----' */
    kEventParamMouseLocation = 0x6D6C6F63, /* 'mloc' */
    kEventParamKeyModifiers = 0x6B6D6F64,  /* 'kmod' */
    kEventParamMouseButton = 0x6D62746E,   /* 'mbtn' */
    kEventParamClickCount = 0x63636E74,    /* 'ccnt' */
    kEventParamKeyMacCharCodes = 0x6B636872, /* 'kchr' */
    kEventParamKeyCode = 0x6B636F64,       /* 'kcod' */
    kEventParamWindowDefPart = 0x77647063, /* 'wdpc' */
    kEventParamWindowRef = 0x77696E64,     /* 'wind' */
    kEventParamControlRef = 0x6374726C,    /* 'ctrl' */
    kEventParamControlPart = 0x63707274,   /* 'cprt' */
    kEventParamAEEventClass = 0x6576636C,  /* 'evcl' */
    kEventParamAEEventID = 0x65766964,     /* 'evid' */
    kEventParamAttributes = 0x61747472,    /* 'attr' */
    kEventParamOriginalBounds = 0x6F726374, /* 'orct' */
    kEventParamPreviousBounds = 0x70726374, /* 'prct' */
    kEventParamCurrentBounds = 0x63726374, /* 'crct' */
    kEventParamDimensions = 0x64696D73,    /* 'dims' */
    kEventParamEnableMenuForKeyEvent = 0x666F726B, /* 'fork' */
    kEventParamPostTarget = 0x70747267,    /* 'ptrg' */
    typeQDPoint = 0x51447074,
    typeQDRectangle = 0x71647274,
    typeUInt32 = 0x6D61676E,
    typeSInt32 = 0x6C6F6E67,
    typeSInt16 = 0x73686F72,
    typeBoolean = 0x626F6F6C,
    typeChar = 0x54455854,
    typeWildCard = 0x2A2A2A2A,
    typeEventTargetRef = 0x65747267,
    typeWindowRef = 0x77696E64,
    typeControlRef = 0x6374726C,
    typeMenuRef = 0x6D656E75,
    typeHICommand = 0x68636D64,
    typeMouseButton = 0x6D62746E,
    typeWindowDefPartCode = 0x77647074,
    typeControlPartCode = 0x63707274,
    typeEventHotKeyID = 0x686B6964,
    typeType = 0x74797065,
};
enum { kHICommandQuit = 0x71756974, kHICommandFromMenu = 1 };

/* Classic events and their parts. */
enum { nullEvent = 0, mouseDown, mouseUp, keyDown, keyUp, autoKey, updateEvt, diskEvt, activateEvt, osEvt = 15,
       kHighLevelEvent = 23 };
enum { activeFlag = 1, btnState = 0x80, cmdKey = 0x100, shiftKey = 0x200, optionKey = 0x800 };
enum { inDesk = 0, inMenuBar, inSysWindow, inContent, inDrag, inGrow, inGoAway, inZoomIn, inZoomOut,
       inCollapseBox = 11, inProxyIcon = 12 };
enum { wInContent = 1, wInDrag, wInGrow, wInGoAway, wInZoomIn, wInZoomOut, wInCollapseBox = 9, wInProxyIcon };
enum { kWindowBoundsChangeUserResize = 2, kWindowBoundsChangeSizeChanged = 4 };
enum { kMouseTrackingMousePressed = 1, kMouseTrackingMouseReleased, kMouseTrackingMouseExited,
       kMouseTrackingMouseEntered, kMouseTrackingMouseMoved, kMouseTrackingKeyModifiersChanged,
       kMouseTrackingUserCancelled, kMouseTrackingTimedOut };

/* Reference kinds (multiprocessing.c's are from 101). */
enum { REF_EVENT = 1, REF_TARGET, REF_HANDLER, REF_TIMER, REF_CALL, REF_LOOP, REF_QUEUE, REF_HOTKEY, REF_CLASS };

/* ---------------------------------------------------------------------- */
/* Opaque references */

enum { REFS_PER_BLOCK = 1024, MAX_REF_BLOCKS = 256 };

typedef struct {
    int kind;
    void *obj;
} ref_slot;

static gaddr ref_base[MAX_REF_BLOCKS];
static ref_slot *ref_tab[MAX_REF_BLOCKS];
static int ref_blocks;

gaddr ref_new(int kind, void *obj)
{
    for (int b = 0;; b++) {
        if (b == ref_blocks) {
            if (b == MAX_REF_BLOCKS)
                return 0;
            gaddr base = tb_NewPtr(REFS_PER_BLOCK * 4, true, true);
            ref_slot *tab = calloc(REFS_PER_BLOCK, sizeof *tab);
            if (!base || !tab) {
                free(tab);
                return 0;
            }
            ref_base[b] = base;
            ref_tab[b] = tab;
            ref_blocks++;
        }
        for (int i = 0; i < REFS_PER_BLOCK; i++)
            if (!ref_tab[b][i].kind) {
                ref_tab[b][i] = (ref_slot){ kind, obj };
                wr32(ref_base[b] + 4u * (uint32_t)i, (uint32_t)kind);
                return ref_base[b] + 4u * (uint32_t)i;
            }
    }
}

static ref_slot *slot_of(gaddr ref)
{
    for (int b = 0; b < ref_blocks; b++)
        if (ref >= ref_base[b] && ref < ref_base[b] + REFS_PER_BLOCK * 4 && !(ref & 3))
            return &ref_tab[b][(ref - ref_base[b]) / 4];
    return NULL;
}

void *ref_get(gaddr ref, int kind)
{
    ref_slot *s = ref ? slot_of(ref) : NULL;
    return s && s->kind == kind ? s->obj : NULL;
}

int ref_kind(gaddr ref)
{
    ref_slot *s = ref ? slot_of(ref) : NULL;
    return s ? s->kind : 0;
}

void ref_free(gaddr ref)
{
    ref_slot *s = ref ? slot_of(ref) : NULL;
    if (s) {
        *s = (ref_slot){ 0, NULL };
        wr32(ref, 0);
    }
}

/* ---------------------------------------------------------------------- */
/* Events */

typedef struct {
    uint32_t name, type, size;
    uint8_t *data;
} param;

typedef struct cevent {
    gaddr ref;
    uint32_t cls, kind, attrs;
    double when;
    int retain;
    param *params;
    int nparams, cap;
    bool queued;
    int16_t priority;
    struct cevent *qnext;
    bool has_record;   /* the classic event it was made from */
    uint8_t record[16];
} cevent;

static double now_secs(void) { return (double)machine_time_ns() / 1e9; }

static cevent *event_new(uint32_t cls, uint32_t kind, double when)
{
    cevent *e = calloc(1, sizeof *e);
    if (!e)
        return NULL;
    e->ref = ref_new(REF_EVENT, e);
    if (!e->ref) {
        free(e);
        return NULL;
    }
    e->cls = cls;
    e->kind = kind;
    e->when = when ? when : now_secs();
    e->retain = 1;
    return e;
}

static void event_free(cevent *e)
{
    for (int i = 0; i < e->nparams; i++)
        free(e->params[i].data);
    free(e->params);
    ref_free(e->ref);
    free(e);
}

static void event_release(cevent *e)
{
    if (e && --e->retain <= 0 && !e->queued)
        event_free(e);
}

static param *find_param(cevent *e, uint32_t name)
{
    for (int i = 0; i < e->nparams; i++)
        if (e->params[i].name == name)
            return &e->params[i];
    return NULL;
}

static bool set_param(cevent *e, uint32_t name, uint32_t type, uint32_t size, const void *data)
{
    param *p = find_param(e, name);
    if (!p) {
        if (e->nparams == e->cap) {
            int cap = e->cap ? e->cap * 2 : 8;
            param *np = realloc(e->params, sizeof *np * (size_t)cap);
            if (!np)
                return false;
            e->params = np;
            e->cap = cap;
        }
        p = &e->params[e->nparams++];
        p->name = name;
        p->data = NULL;
    }
    free(p->data);
    p->type = type;
    p->size = size;
    p->data = malloc(size ? size : 1);
    if (size && data)
        memcpy(p->data, data, size);
    return true;
}

static void set32(cevent *e, uint32_t name, uint32_t type, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    set_param(e, name, type, 4, b);
}

static void set16(cevent *e, uint32_t name, uint32_t type, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    set_param(e, name, type, 2, b);
}

static bool get32(cevent *e, uint32_t name, uint32_t *v)
{
    param *p = find_param(e, name);
    if (!p || p->size < 4)
        return false;
    *v = (uint32_t)p->data[0] << 24 | (uint32_t)p->data[1] << 16 | (uint32_t)p->data[2] << 8 | p->data[3];
    return true;
}

static bool get16(cevent *e, uint32_t name, uint16_t *v)
{
    param *p = find_param(e, name);
    if (!p || p->size < 2)
        return false;
    *v = (uint16_t)(p->data[0] << 8 | p->data[1]);
    return true;
}

static uint32_t direct_object(cevent *e)
{
    uint32_t v = 0;
    get32(e, kEventParamDirectObject, &v);
    return v;
}

gaddr ce_event_new(uint32_t cls, uint32_t kind)
{
    cevent *e = event_new(cls, kind, 0);
    return e ? e->ref : 0;
}

void ce_event_set(gaddr ref, uint32_t name, uint32_t type, uint32_t size, const void *data)
{
    cevent *e = ref_get(ref, REF_EVENT);
    if (e)
        set_param(e, name, type, size, data);
}

void ce_event_set32(gaddr ref, uint32_t name, uint32_t type, uint32_t value)
{
    cevent *e = ref_get(ref, REF_EVENT);
    if (e)
        set32(e, name, type, value);
}

bool ce_event_get(gaddr ref, uint32_t name, void *out, uint32_t size)
{
    cevent *e = ref_get(ref, REF_EVENT);
    param *p = e ? find_param(e, name) : NULL;
    if (!p)
        return false;
    memcpy(out, p->data, p->size < size ? p->size : size);
    return true;
}

void ce_event_release(gaddr ref) { event_release(ref_get(ref, REF_EVENT)); }

/* ---------------------------------------------------------------------- */
/* Targets and handlers */

enum { T_APPLICATION, T_WINDOW, T_CONTROL, T_MENU, T_DISPATCHER, T_FOCUS };

struct target;

typedef struct handler {
    gaddr ref;
    struct target *target;
    gaddr upp, data;
    uint32_t (*types)[2];
    int ntypes;
    bool dead;
    struct handler *next;
    struct handler *dead_next;
} handler;

typedef struct target {
    gaddr ref;
    int kind;
    gaddr object;
    gaddr owner;      /* a control's window */
    handler *handlers; /* the most recently installed first */
    bool standard;    /* InstallStandardEventHandler */
    bool dead;
    gaddr default_button, cancel_button; /* a window's */
    struct target *next;
    struct target *dead_next;
} target;

static target *targets;
static target *app_target, *dispatcher_target, *focus_target;
static int send_depth;
static bool any_handlers;
static handler *dead_handlers;
static target *dead_targets;

static target *target_new(int kind, gaddr object)
{
    target *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->ref = ref_new(REF_TARGET, t);
    t->kind = kind;
    t->object = object;
    t->next = targets;
    targets = t;
    return t;
}

static target *target_of(int kind, gaddr object, bool make)
{
    for (target *t = targets; t; t = t->next)
        if (t->kind == kind && t->object == object && !t->dead)
            return t;
    return make ? target_new(kind, object) : NULL;
}

static target *application(void)
{
    if (!app_target)
        app_target = target_new(T_APPLICATION, 0);
    return app_target;
}

static void free_handler(handler *h)
{
    ref_free(h->ref);
    free(h->types);
    free(h);
}

/* Dead handlers and targets wait for the sends under way to finish. */
static void collect(void)
{
    if (send_depth)
        return;
    while (dead_handlers) {
        handler *h = dead_handlers;
        dead_handlers = h->dead_next;
        free_handler(h);
    }
    while (dead_targets) {
        target *t = dead_targets;
        dead_targets = t->dead_next;
        ref_free(t->ref);
        free(t);
    }
}

static void unlink_handler(handler *h)
{
    target *t = h->target;
    for (handler **p = &t->handlers; *p; p = &(*p)->next)
        if (*p == h) {
            *p = h->next;
            break;
        }
    h->dead = true;
    if (send_depth) {
        /* A send may be stepping through it: its next stays good. */
        h->dead_next = dead_handlers;
        dead_handlers = h;
        return;
    }
    free_handler(h);
}

static void target_kill(target *t)
{
    if (!t || t->dead)
        return;
    while (t->handlers) {
        handler *h = t->handlers;
        t->handlers = h->next;
        h->dead = true;
        if (send_depth) {
            h->dead_next = dead_handlers;
            dead_handlers = h;
        } else {
            free_handler(h);
        }
    }
    for (target **p = &targets; *p; p = &(*p)->next)
        if (*p == t) {
            *p = t->next;
            break;
        }
    t->dead = true;
    if (t == app_target)
        app_target = NULL;
    if (send_depth) {
        t->dead_next = dead_targets;
        dead_targets = t;
    } else {
        ref_free(t->ref);
        free(t);
    }
}

/* Whether w is a window in the window list. */
static bool window_exists(gaddr w)
{
    if (!w)
        return false;
    for (gaddr p = rd32(LM_WindowList); p; p = WindowRecord_nextWindow(p))
        if (p == w)
            return true;
    return false;
}

static gaddr focus_window_set; /* SetUserFocusWindow's, while it lasts */

/* The user focus: the window set, or the frontmost (not floating). */
static gaddr focus_window(void)
{
    if (focus_window_set && window_exists(focus_window_set) && WindowRecord_visible(focus_window_set))
        return focus_window_set;
    focus_window_set = 0;
    return tb_FrontNonFloatingWindow();
}

/* Where an event sent to t goes next. */
static target *parent_of(target *t)
{
    switch (t->kind) {
    case T_CONTROL: {
        gaddr w = t->owner;
        if (!t->dead && t->object && rd32(t->object))
            w = ControlRecord_contrlOwner(rd32(t->object));
        return w && window_exists(w) ? target_of(T_WINDOW, w, true) : application();
    }
    case T_WINDOW:
    case T_MENU:
        return application();
    default:
        return NULL;
    }
}

gaddr ce_application_target(void) { return application()->ref; }

gaddr ce_window_target(gaddr w)
{
    target *t = w ? target_of(T_WINDOW, w, true) : NULL;
    return t ? t->ref : 0;
}

gaddr ce_control_target(gaddr ctl)
{
    target *t = ctl ? target_of(T_CONTROL, ctl, true) : NULL;
    if (t && rd32(ctl))
        t->owner = ControlRecord_contrlOwner(rd32(ctl));
    return t ? t->ref : 0;
}

gaddr ce_menu_target(gaddr menu)
{
    target *t = menu ? target_of(T_MENU, menu, true) : NULL;
    return t ? t->ref : 0;
}

gaddr ce_focus_target(void)
{
    if (!focus_target)
        focus_target = target_new(T_FOCUS, 0);
    return focus_target->ref;
}

bool ce_active(void) { return any_handlers; }

void carbon_window_disposed(gaddr w)
{
    if (focus_window_set == w)
        focus_window_set = 0;
    target_kill(target_of(T_WINDOW, w, false));
}

void carbon_control_disposed(gaddr ctl) { target_kill(target_of(T_CONTROL, ctl, false)); }
void carbon_menu_disposed(gaddr menu) { target_kill(target_of(T_MENU, menu, false)); }

/* ---------------------------------------------------------------------- */
/* Sending */

/* Where a send has got to: the target, the next handler on it, and
 * whether its standard handler has had its turn.  A handler's
 * EventHandlerCallRef names one of these. */
typedef struct {
    cevent *e;
    target *t;
    handler *next;
    int stage; /* 0: the handlers, 1: the standard handler, 2: on to the parent */
    bool next_called;
} frame;

static int32_t standard(target *t, cevent *e, frame *f);
static int32_t run_chain(frame *f);

static bool handles(const handler *h, const cevent *e)
{
    for (int i = 0; i < h->ntypes; i++)
        if (h->types[i][0] == e->cls && h->types[i][1] == e->kind)
            return true;
    return false;
}

/* EventHandlerUPP: pascal OSStatus (EventHandlerCallRef, EventRef, void *). */
static int32_t call_handler(handler *h, frame *f)
{
    gaddr call = ref_new(REF_CALL, f);
    uint32_t args[3] = { call, f->e->ref, h->data };
    int32_t r = (int32_t)aem_call_proc(h->upp, 0xFF0, args, 3);
    ref_free(call);
    return r;
}

static int32_t run_chain(frame *f)
{
    while (f->t && !g_cpu->halted) {
        target *t = f->t;
        if (f->stage == 0) {
            for (handler *h = f->next; h; h = h->next) {
                if (h->dead || !handles(h, f->e))
                    continue;
                f->next = h->next;
                int32_t r = call_handler(h, f);
                /* Handled, or passed on already with CallNextEventHandler. */
                if (r != eventNotHandledErr || f->next_called)
                    return r;
            }
            f->next = NULL;
            f->stage = 1;
        }
        if (f->stage == 1) {
            f->stage = 2;
            int32_t r = standard(t, f->e, f);
            if (r != eventNotHandledErr || f->next_called)
                return r;
        }
        f->t = parent_of(t);
        f->next = f->t ? f->t->handlers : NULL;
        f->stage = 0;
    }
    return eventNotHandledErr;
}

static int32_t send(cevent *e, target *t)
{
    if (!e || !t)
        return paramErr;
    if (t->kind == T_FOCUS) {
        gaddr w = focus_window();
        t = w ? target_of(T_WINDOW, w, true) : application();
    }
    if (t->kind == T_WINDOW && !window_exists(t->object)) {
        target_kill(t);
        return errInvalidWindowRef;
    }
    e->retain++;
    send_depth++;
    frame f = { e, t, t->handlers, 0, false };
    int32_t r = run_chain(&f);
    send_depth--;
    event_release(e);
    collect();
    return r;
}

int32_t ce_send(gaddr event, gaddr tref)
{
    return send(ref_get(event, REF_EVENT), ref_get(tref, REF_TARGET));
}

static int32_t send_new(cevent *e, target *t)
{
    int32_t r = send(e, t);
    event_release(e);
    return r;
}

static int32_t send_object_event(uint32_t cls, uint32_t kind, uint32_t type, gaddr obj, target *t)
{
    cevent *e = event_new(cls, kind, 0);
    if (!e)
        return eventInternalErr;
    set32(e, kEventParamDirectObject, type, obj);
    return send_new(e, t);
}

int32_t carbon_send_window_event(gaddr w, uint32_t kind)
{
    if (!any_handlers && kind != kEventWindowClose && kind != kEventWindowDrawContent)
        return eventNotHandledErr;
    return send_object_event(kEventClassWindow, kind, typeWindowRef, w, target_of(T_WINDOW, w, true));
}

int32_t carbon_send_control_event(gaddr ctl, uint32_t kind)
{
    if (!any_handlers)
        return eventNotHandledErr;
    gaddr t = ce_control_target(ctl);
    return send_object_event(kEventClassControl, kind, typeControlRef, ctl, ref_get(t, REF_TARGET));
}

int32_t carbon_send_menu_event(gaddr menu, uint32_t kind)
{
    if (!any_handlers)
        return eventNotHandledErr;
    return send_object_event(kEventClassMenu, kind, typeMenuRef, menu, target_of(T_MENU, menu, true));
}

/* kEventCommandProcess with an HICommand {attributes, commandID, menuRef,
 * menuItemIndex}. */
static int32_t send_command(uint32_t id, gaddr menu, uint16_t item, uint32_t mods, target *t)
{
    cevent *e = event_new(kEventClassCommand, kEventCommandProcess, 0);
    if (!e)
        return eventInternalErr;
    uint8_t cmd[14];
    uint32_t attrs = menu ? kHICommandFromMenu : 0;
    uint32_t words[3] = { attrs, id, menu };
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 4; k++)
            cmd[4 * i + k] = (uint8_t)(words[i] >> (24 - 8 * k));
    cmd[12] = (uint8_t)(item >> 8);
    cmd[13] = (uint8_t)item;
    set_param(e, kEventParamDirectObject, typeHICommand, sizeof cmd, cmd);
    set32(e, kEventParamKeyModifiers, typeUInt32, mods);
    return send_new(e, t);
}

int32_t carbon_send_command(uint32_t id, gaddr menu, uint16_t item, uint32_t mods)
{
    ce_focus_target();
    return send_command(id, menu, item, mods, focus_target);
}

/* ---------------------------------------------------------------------- */
/* Calling into the rest of the Toolbox */

/* An export of ours run as a PowerPC caller would (a routine another part
 * of the Toolbox provides by name only). */
static uint32_t call_export(const char *name, const uint32_t *args, int n)
{
    void (*fn)(ppc_cpu *) = cfm_export_fn(name);
    if (!fn)
        return (uint32_t)eventInternalErr;
    ppc_cpu saved = g_ppc;
    for (int i = 0; i < n && i < 8; i++)
        g_ppc.gpr[3 + i] = args[i];
    fn(&g_ppc);
    uint32_t r = g_ppc.gpr[3];
    uint64_t icount = g_ppc.icount;
    bool halted = g_ppc.halted;
    char reason[sizeof g_ppc.halt_reason];
    memcpy(reason, g_ppc.halt_reason, sizeof reason);
    g_ppc = saved;
    g_ppc.icount = icount;
    if (halted)
        ppc_halt(&g_ppc, "%s", reason);
    return r;
}

/* A word of scratch guest memory for out parameters. */
static gaddr scratch(void)
{
    static gaddr p;
    if (!p)
        p = tb_NewPtr(32, true, true);
    return p;
}

static uint32_t control_command(gaddr ctl)
{
    gaddr out = scratch();
    wr32(out, 0);
    uint32_t args[2] = { ctl, out };
    return call_export("GetControlCommandID", args, 2) == 0 ? rd32(out) : 0;
}

static uint32_t menu_command(gaddr menu, int16_t item)
{
    gaddr out = scratch();
    wr32(out, 0);
    return tb_GetMenuItemCommandID(menu, item, out) == 0 ? rd32(out) : 0;
}

/* The current port set to w's for a moment. */
static gaddr port_push(gaddr w)
{
    gaddr p = scratch() + 4;
    tb_GetPort(p);
    gaddr old = rd32(p);
    tb_SetPort(w);
    return old;
}

static uint32_t to_local(gaddr w, uint32_t global)
{
    gaddr old = port_push(w);
    gaddr pt = scratch() + 8;
    wr32(pt, global);
    tb_GlobalToLocal(pt);
    uint32_t local = rd32(pt);
    tb_SetPort(old);
    return local;
}

/* ---------------------------------------------------------------------- */
/* Standard handlers */

/* A window's standard handler and the basic one every window has: the
 * frame's parts tracked and acted on, updates bracketed, activation passed
 * to the controls; and with the standard handler, contents clicked and
 * keys typed going to the controls, and closing disposing of the window. */

static void flash_control(gaddr ctl)
{
    tb_HiliteControl(ctl, 1);
    for (int i = 0; i < 8 && !g_cpu->halted; i++)
        ev_wait_tick();
    tb_HiliteControl(ctl, 0);
}

/* A control's part hit: kEventControlHit, then its command. */
static int32_t control_hit(gaddr ctl, int16_t part, uint32_t mods)
{
    cevent *e = event_new(kEventClassControl, kEventControlHit, 0);
    if (!e)
        return eventInternalErr;
    set32(e, kEventParamDirectObject, typeControlRef, ctl);
    set16(e, kEventParamControlPart, typeControlPartCode, (uint16_t)part);
    set32(e, kEventParamKeyModifiers, typeUInt32, mods);
    int32_t r = send_new(e, ref_get(ce_control_target(ctl), REF_TARGET));
    uint32_t cmd = control_command(ctl);
    if (cmd)
        r = send_command(cmd, 0, 0, mods, ref_get(ce_control_target(ctl), REF_TARGET));
    return r;
}

static bool usable(gaddr ctl)
{
    return ctl && rd32(ctl) && ControlRecord_contrlVis(rd32(ctl)) && ControlRecord_contrlHilite(rd32(ctl)) != 255;
}

static int32_t window_key(target *t, cevent *e)
{
    gaddr w = t->object;
    uint32_t mods = 0, code = 0;
    get32(e, kEventParamKeyModifiers, &mods);
    get32(e, kEventParamKeyCode, &code);
    param *p = find_param(e, kEventParamKeyMacCharCodes);
    uint8_t ch = p && p->size ? p->data[0] : 0;
    if (e->kind == kEventRawKeyUp)
        return eventNotHandledErr;
    if (!(mods & cmdKey) && (ch == 0x0D || ch == 0x03) && usable(t->default_button)) {
        flash_control(t->default_button);
        control_hit(t->default_button, 10, mods); /* kControlButtonPart */
        return noErr;
    }
    if ((ch == 0x1B || ((mods & cmdKey) && ch == '.')) && usable(t->cancel_button)) {
        flash_control(t->cancel_button);
        control_hit(t->cancel_button, 10, mods);
        return noErr;
    }
    if (mods & cmdKey)
        return eventNotHandledErr;
    gaddr out = scratch() + 12;
    wr32(out, 0);
    if (tb_GetKeyboardFocus(w, out) == 0 && rd32(out)) {
        tb_HandleControlKey(rd32(out), (int16_t)code, ch, (int16_t)mods);
        return noErr;
    }
    return eventNotHandledErr;
}

/* Where an event's mouse location is, globally. */
static uint32_t mouse_where(cevent *e)
{
    uint32_t where = 0;
    get32(e, kEventParamMouseLocation, &where);
    return where;
}

static void activate_controls(gaddr w, bool on)
{
    gaddr out = scratch() + 16;
    wr32(out, 0);
    if (tb_GetRootControl(w, out) == 0 && rd32(out)) {
        if (on)
            tb_ActivateControl(rd32(out));
        else
            tb_DeactivateControl(rd32(out));
        return;
    }
    for (gaddr c = WindowRecord_controlList(w); c && rd32(c); c = ControlRecord_nextControl(rd32(c)))
        tb_HiliteControl(c, on ? 0 : 255);
}

/* The size limits a program gives (kEventWindowGetMinimumSize and
 * GetMaximumSize), or the defaults. */
static uint32_t window_limit(gaddr w, uint32_t kind, uint32_t dflt)
{
    cevent *e = event_new(kEventClassWindow, kind, 0);
    if (!e)
        return dflt;
    set32(e, kEventParamDirectObject, typeWindowRef, w);
    uint32_t v = dflt;
    e->retain++;
    if (send(e, target_of(T_WINDOW, w, true)) == noErr)
        get32(e, kEventParamDimensions, &v);
    event_release(e);
    event_release(e);
    return v;
}

/* The content's bounds, globally. */
static qrect content_bounds(gaddr w)
{
    gaddr rgn = WindowRecord_contRgn(w);
    return rgn && rd32(rgn) ? qrect_get(rd32(rgn) + 2) : (qrect){ 0, 0, 0, 0 };
}

static void bounds_changed(gaddr w, qrect before)
{
    cevent *e = event_new(kEventClassWindow, kEventWindowBoundsChanged, 0);
    if (!e)
        return;
    qrect now = content_bounds(w);
    uint8_t b[8];
    qrect rs[2] = { before, now };
    set32(e, kEventParamDirectObject, typeWindowRef, w);
    set32(e, kEventParamAttributes, typeUInt32, kWindowBoundsChangeUserResize | kWindowBoundsChangeSizeChanged);
    uint32_t names[3] = { kEventParamOriginalBounds, kEventParamPreviousBounds, kEventParamCurrentBounds };
    for (int i = 0; i < 3; i++) {
        qrect q = rs[i == 2];
        int16_t v[4] = { q.top, q.left, q.bottom, q.right };
        for (int k = 0; k < 4; k++)
            b[2 * k] = (uint8_t)((uint16_t)v[k] >> 8), b[2 * k + 1] = (uint8_t)v[k];
        set_param(e, names[i], typeQDRectangle, 8, b);
    }
    send_new(e, target_of(T_WINDOW, w, true));
}

static int32_t window_basic(target *t, cevent *e)
{
    gaddr w = t->object;
    if (e->cls == kEventClassKeyboard)
        return t->standard ? window_key(t, e) : eventNotHandledErr;
    if (e->cls != kEventClassWindow)
        return eventNotHandledErr;
    uint32_t where = mouse_where(e), mods = 0;
    get32(e, kEventParamKeyModifiers, &mods);
    uint16_t part = 0;
    get16(e, kEventParamWindowDefPart, &part);
    switch (e->kind) {
    case kEventWindowClickDragRgn: {
        gaddr gray = rd32(LM_GrayRgn);
        gaddr r = scratch() + 16;
        if (gray && rd32(gray))
            gmemcpy(r, rd32(gray) + 2, 8);
        else
            qrect_put(r, (qrect){ 0, 0, 0x7FFF, 0x7FFF });
        tb_DragWindow(w, where, r);
        return noErr;
    }
    case kEventWindowClickCloseRgn:
        if (tb_TrackGoAway(w, where))
            send_object_event(kEventClassWindow, kEventWindowClose, typeWindowRef, w, t);
        return noErr;
    case kEventWindowClickZoomRgn: {
        int16_t classic = part == wInZoomIn ? inZoomIn : inZoomOut;
        if (tb_TrackBox(w, where, classic)) {
            cevent *z = event_new(kEventClassWindow, kEventWindowZoom, 0);
            if (z) {
                set32(z, kEventParamDirectObject, typeWindowRef, w);
                set16(z, kEventParamWindowDefPart, typeWindowDefPartCode, part);
                send_new(z, t);
            }
        }
        return noErr;
    }
    case kEventWindowZoom: {
        gaddr old = port_push(w);
        tb_ZoomWindow(w, part == wInZoomIn ? inZoomIn : inZoomOut, false);
        tb_SetPort(old);
        send_object_event(kEventClassWindow, kEventWindowZoomed, typeWindowRef, w, t);
        return noErr;
    }
    case kEventWindowClickCollapseRgn:
        if (tb_TrackBox(w, where, inCollapseBox))
            tb_CollapseWindow(w, !wm_is_collapsed(w));
        return noErr;
    case kEventWindowClickResizeRgn: {
        gaddr limits = scratch() + 24;
        uint32_t lo = window_limit(w, kEventWindowGetMinimumSize, 0x00400040u);
        uint32_t hi = window_limit(w, kEventWindowGetMaximumSize, 0x7FFF7FFFu);
        qrect_put(limits, (qrect){ (int16_t)(lo >> 16), (int16_t)lo, (int16_t)(hi >> 16), (int16_t)hi });
        qrect before = content_bounds(w);
        int32_t size = tb_GrowWindow(w, where, limits);
        if (size) {
            tb_SizeWindow(w, (int16_t)size, (int16_t)(size >> 16), true);
            bounds_changed(w, before);
        }
        send_object_event(kEventClassWindow, kEventWindowResizeCompleted, typeWindowRef, w, t);
        return noErr;
    }
    case kEventWindowUpdate: {
        gaddr old = port_push(w);
        tb_BeginUpdate(w);
        send_object_event(kEventClassWindow, kEventWindowDrawContent, typeWindowRef, w, t);
        tb_EndUpdate(w);
        tb_SetPort(old);
        return noErr;
    }
    case kEventWindowActivated:
    case kEventWindowDeactivated:
        if (t->standard)
            activate_controls(w, e->kind == kEventWindowActivated);
        return t->standard ? noErr : eventNotHandledErr;
    }
    if (!t->standard)
        return eventNotHandledErr;
    switch (e->kind) {
    case kEventWindowDrawContent:
        tb_UpdateControls(w, GrafPort_visRgn(w));
        return noErr;
    case kEventWindowClose:
        tb_DisposeWindow(w);
        carbon_window_disposed(w);
        return noErr;
    case kEventWindowClickContentRgn: {
        if (w != tb_FrontNonFloatingWindow() && WindowRecord_windowKind(w) >= 0) {
            tb_SelectWindow(w);
            return noErr;
        }
        uint32_t local = to_local(w, where);
        gaddr partp = scratch() + 20;
        wr16(partp, 0);
        gaddr old = port_push(w);
        gaddr ctl = tb_FindControlUnderMouse(local, w, partp);
        int32_t r = eventNotHandledErr;
        if (ctl && (int16_t)rd16(partp)) {
            int16_t hit = tb_HandleControlClick(ctl, local, (int16_t)mods, 0xFFFFFFFFu);
            if (hit)
                control_hit(ctl, hit, mods);
            r = noErr;
        }
        tb_SetPort(old);
        if (r == eventNotHandledErr)
            r = send_object_event(kEventClassWindow, kEventWindowHandleContentClick, typeWindowRef, w, t);
        return noErr;
    }
    }
    return eventNotHandledErr;
}

/* The application's standard handler: quitting, Apple events. */
static bool quit_app;
static int quit_requests;
static int app_loops; /* RunApplicationEventLoop's depth */

static void quit_application_loop(void)
{
    quit_app = true;
    quit_requests++;
}

static int32_t app_standard(cevent *e)
{
    if (e->cls == kEventClassCommand && e->kind == kEventCommandProcess) {
        param *p = find_param(e, kEventParamDirectObject);
        uint32_t id = p && p->size >= 8 ? (uint32_t)p->data[4] << 24 | p->data[5] << 16 | p->data[6] << 8 | p->data[7] : 0;
        if (id != kHICommandQuit)
            return eventNotHandledErr;
        /* As a quit Apple event, to the program's handler if it has one. */
        gaddr h = scratch() + 24, refcon = scratch() + 28;
        if (tb_AEGetEventHandler(0x61657674, 0x71756974, h, refcon, false) == 0)
            machine_send_quit();
        else
            quit_application_loop();
        return noErr;
    }
    if (e->cls == kEventClassAppleEvent && e->kind == kEventAppleEvent && e->has_record) {
        gaddr rec = scratch();
        gput(rec, e->record, 16);
        uint32_t cls = 0, id = 0;
        get32(e, kEventParamAEEventClass, &cls);
        get32(e, kEventParamAEEventID, &id);
        int16_t err = tb_AEProcessAppleEvent(rec);
        /* With RunApplicationEventLoop, a quit the program doesn't handle
         * itself ends the loop. */
        if (cls == 0x61657674 && id == 0x71756974 && err && app_loops)
            quit_application_loop();
        return noErr;
    }
    return eventNotHandledErr;
}

static int32_t dispatch(cevent *e);

static int32_t standard(target *t, cevent *e, frame *f)
{
    switch (t->kind) {
    case T_WINDOW:
        return window_basic(t, e);
    case T_APPLICATION:
        return t->standard || app_loops ? app_standard(e) : eventNotHandledErr;
    case T_DISPATCHER:
        return dispatch(e);
    }
    return eventNotHandledErr;
}

/* ---------------------------------------------------------------------- */
/* The dispatcher: classic events' routes */

typedef struct {
    gaddr window;
    bool quit;
} modal_level;

static modal_level modal[16];
static int nmodal;

typedef struct hotkey {
    gaddr ref;
    uint32_t code, mods, sig, id;
    target *t;
    struct hotkey *next;
} hotkey;
static hotkey *hotkeys;

static gaddr modal_window(void)
{
    for (int i = nmodal - 1; i >= 0; i--)
        if (window_exists(modal[i].window))
            return modal[i].window;
    return 0;
}

/* kEventMenuEnableItems to each menu in the bar, for programs that
 * enable their items as menus open. */
static void menus_opening(bool for_key)
{
    gaddr ml = rd32(LM_MenuList);
    if (!any_handlers || !ml || !rd32(ml))
        return;
    uint16_t last = rd16(rd32(ml));
    for (uint16_t off = 6; off <= last && off < 6 * 64; off += 6) {
        gaddr menu = rd32(rd32(ml) + off);
        cevent *e = event_new(kEventClassMenu, kEventMenuEnableItems, 0);
        if (!e)
            return;
        set32(e, kEventParamDirectObject, typeMenuRef, menu);
        uint8_t b = for_key;
        set_param(e, kEventParamEnableMenuForKeyEvent, typeBoolean, 1, &b);
        send_new(e, target_of(T_MENU, menu, true));
    }
}

/* A menu choice (MenuSelect's or MenuKey's): its command, sent on. */
static void menu_chosen(int32_t choice, uint32_t mods)
{
    int16_t id = (int16_t)(choice >> 16), item = (int16_t)choice;
    if (!id)
        return;
    gaddr menu = tb_GetMenuHandle(id);
    send_command(menu_command(menu, item), menu, (uint16_t)item, mods, ref_get(ce_focus_target(), REF_TARGET));
    tb_HiliteMenu(0);
}

static uint32_t click_part(int16_t part)
{
    switch (part) {
    case inContent:
        return kEventWindowClickContentRgn;
    case inDrag:
        return kEventWindowClickDragRgn;
    case inGrow:
        return kEventWindowClickResizeRgn;
    case inGoAway:
        return kEventWindowClickCloseRgn;
    case inZoomIn:
    case inZoomOut:
        return kEventWindowClickZoomRgn;
    case inCollapseBox:
        return kEventWindowClickCollapseRgn;
    case inProxyIcon:
        return kEventWindowClickProxyIconRgn;
    }
    return 0;
}

static uint16_t def_part(int16_t part)
{
    static const uint16_t map[] = { 0, 0, 0, wInContent, wInDrag, wInGrow, wInGoAway, wInZoomIn, wInZoomOut, 0, 0,
                                    wInCollapseBox, wInProxyIcon };
    return part >= 0 && part < (int16_t)(sizeof map / sizeof map[0]) ? map[part] : 0;
}

static bool to_record(cevent *e, uint8_t r[16]);

/* The event as an EventRecord, in scratch memory: its own, or one made
 * from its parameters (an event a program made and sent). */
static gaddr event_record(cevent *e)
{
    gaddr rec = scratch();
    uint8_t r[16];
    to_record(e, r);
    gput(rec, r, 16);
    return rec;
}

static int32_t route_mouse_down(cevent *e)
{
    uint32_t where = mouse_where(e), mods = 0;
    get32(e, kEventParamKeyModifiers, &mods);
    gaddr wp = scratch() + 28;
    wr32(wp, 0);
    int16_t part = tb_FindWindow(where, wp);
    gaddr w = rd32(wp);
    gaddr m = modal_window();
    if (m && part != inDesk && w != m) {
        tb_SysBeep(1);
        return noErr;
    }
    if (part == inMenuBar) {
        menus_opening(false);
        menu_chosen(tb_MenuSelect(where), mods);
        tb_HiliteMenu(0);
        return noErr;
    }
    if (part == inSysWindow) {
        tb_SystemClick(event_record(e), w);
        return noErr;
    }
    if (!w)
        return eventNotHandledErr;
    target *t = target_of(T_WINDOW, w, true);
    /* The window's handlers see the click first, then its parts. */
    cevent *md = event_new(kEventClassMouse, kEventMouseDown, e->when);
    int32_t r = eventNotHandledErr;
    if (md) {
        for (int i = 0; i < e->nparams; i++)
            set_param(md, e->params[i].name, e->params[i].type, e->params[i].size, e->params[i].data);
        set32(md, kEventParamWindowRef, typeWindowRef, w);
        /* Not the dispatcher again: the window's own chain, without its
         * standard handler (which takes window events, not mouse ones). */
        r = send_new(md, t);
    }
    if (r != eventNotHandledErr)
        return r;
    uint32_t kind = click_part(part);
    if (!kind)
        return eventNotHandledErr;
    cevent *ce = event_new(kEventClassWindow, kind, e->when);
    if (!ce)
        return eventInternalErr;
    set32(ce, kEventParamDirectObject, typeWindowRef, w);
    set32(ce, kEventParamMouseLocation, typeQDPoint, where);
    set32(ce, kEventParamKeyModifiers, typeUInt32, mods);
    set16(ce, kEventParamWindowDefPart, typeWindowDefPartCode, def_part(part));
    return send_new(ce, t);
}

static int32_t route_key(cevent *e)
{
    uint32_t mods = 0, code = 0;
    get32(e, kEventParamKeyModifiers, &mods);
    get32(e, kEventParamKeyCode, &code);
    if (e->kind == kEventRawKeyDown)
        for (hotkey *h = hotkeys; h; h = h->next)
            if (h->code == code && (h->mods & 0xFF00) == (mods & 0xFF00)) {
                cevent *k = event_new(kEventClassKeyboard, kEventHotKeyPressed, 0);
                if (k) {
                    uint8_t id[8] = { (uint8_t)(h->sig >> 24), (uint8_t)(h->sig >> 16), (uint8_t)(h->sig >> 8),
                                      (uint8_t)h->sig, (uint8_t)(h->id >> 24), (uint8_t)(h->id >> 16),
                                      (uint8_t)(h->id >> 8), (uint8_t)h->id };
                    set_param(k, kEventParamDirectObject, typeEventHotKeyID, 8, id);
                    send_new(k, h->t);
                }
                return noErr;
            }
    int32_t r = send(e, ref_get(ce_focus_target(), REF_TARGET));
    if (r != eventNotHandledErr || e->kind == kEventRawKeyUp || !(mods & cmdKey))
        return r;
    if (modal_window())
        return noErr;
    menus_opening(true);
    int32_t choice = (int32_t)tb_MenuEvent(event_record(e));
    if (!choice)
        return eventNotHandledErr;
    menu_chosen(choice, mods);
    return noErr;
}

/* The dispatcher's own handling, after any handlers on it. */
static int32_t dispatch(cevent *e)
{
    switch (e->cls) {
    case kEventClassMouse:
        if (e->kind == kEventMouseDown)
            return route_mouse_down(e);
        {
            gaddr w = focus_window();
            return send(e, w ? target_of(T_WINDOW, w, true) : application());
        }
    case kEventClassKeyboard:
        return route_key(e);
    case kEventClassWindow:
        return send(e, target_of(T_WINDOW, direct_object(e), true));
    case kEventClassControl:
        return send(e, ref_get(ce_control_target(direct_object(e)), REF_TARGET));
    case kEventClassMenu:
        return send(e, target_of(T_MENU, direct_object(e), true));
    case kEventClassApplication:
    case kEventClassAppleEvent:
        return send(e, application());
    }
    uint32_t to = 0;
    if (get32(e, kEventParamPostTarget, &to) && ref_get(to, REF_TARGET))
        return send(e, ref_get(to, REF_TARGET));
    return send(e, ref_get(ce_focus_target(), REF_TARGET));
}

static target *dispatcher(void)
{
    if (!dispatcher_target)
        dispatcher_target = target_new(T_DISPATCHER, 0);
    return dispatcher_target;
}

/* ---------------------------------------------------------------------- */
/* Classic events as Carbon events */

static uint32_t last_click_when, last_click_where, click_count;
static double last_user_time;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static cevent *from_record(gaddr rec)
{
    uint8_t r[16];
    gget(r, rec, 16);
    int16_t what = (int16_t)(r[0] << 8 | r[1]);
    uint32_t msg = be32(r + 2), when = be32(r + 6), where = be32(r + 10);
    uint16_t mods = (uint16_t)(r[14] << 8 | r[15]);
    uint32_t cls, kind;
    switch (what) {
    case mouseDown:
    case mouseUp:
        cls = kEventClassMouse, kind = what == mouseDown ? kEventMouseDown : kEventMouseUp;
        break;
    case keyDown:
    case autoKey:
    case keyUp:
        cls = kEventClassKeyboard;
        kind = what == keyDown ? kEventRawKeyDown : what == autoKey ? kEventRawKeyRepeat : kEventRawKeyUp;
        break;
    case updateEvt:
        cls = kEventClassWindow, kind = kEventWindowUpdate;
        break;
    case activateEvt:
        cls = kEventClassWindow, kind = mods & activeFlag ? kEventWindowActivated : kEventWindowDeactivated;
        break;
    case osEvt:
        if ((msg >> 24) == 0xFA) { /* mouse moved */
            cls = kEventClassMouse, kind = kEventMouseMoved;
        } else if ((msg >> 24) == 0x01) { /* suspend or resume */
            cls = kEventClassApplication, kind = msg & 1 ? kEventAppActivated : kEventAppDeactivated;
        } else {
            return NULL;
        }
        break;
    case kHighLevelEvent:
        cls = kEventClassAppleEvent, kind = kEventAppleEvent;
        break;
    default:
        return NULL;
    }
    /* EventRecord's when is in ticks: the event's time, in seconds. */
    double secs = now_secs() - (double)(uint32_t)(tb_TickCount() - when) / 60.0;
    cevent *e = event_new(cls, kind, secs > 0 ? secs : now_secs());
    if (!e)
        return NULL;
    e->has_record = true;
    memcpy(e->record, r, 16);
    switch (what) {
    case mouseDown:
    case mouseUp:
        if (what == mouseDown) {
            int dv = (int16_t)(where >> 16) - (int16_t)(last_click_where >> 16);
            int dh = (int16_t)where - (int16_t)last_click_where;
            if (click_count && when - last_click_when <= rd32(LM_DoubleTime) && abs(dv) <= 4 && abs(dh) <= 4)
                click_count++;
            else
                click_count = 1;
            last_click_when = when;
            last_click_where = where;
        }
        set32(e, kEventParamMouseLocation, typeQDPoint, where);
        set32(e, kEventParamKeyModifiers, typeUInt32, mods & 0xFF00u);
        set16(e, kEventParamMouseButton, typeMouseButton, 1);
        set32(e, kEventParamClickCount, typeUInt32, click_count ? click_count : 1);
        last_user_time = e->when;
        break;
    case keyDown:
    case autoKey:
    case keyUp: {
        uint8_t ch = (uint8_t)msg;
        set_param(e, kEventParamKeyMacCharCodes, typeChar, 1, &ch);
        set32(e, kEventParamKeyCode, typeUInt32, (msg >> 8) & 0xFF);
        set32(e, kEventParamKeyModifiers, typeUInt32, mods & 0xFF00u);
        last_user_time = e->when;
        break;
    }
    case updateEvt:
    case activateEvt:
        set32(e, kEventParamDirectObject, typeWindowRef, msg);
        break;
    case osEvt:
        set32(e, kEventParamMouseLocation, typeQDPoint, where);
        set32(e, kEventParamKeyModifiers, typeUInt32, mods & 0xFF00u);
        break;
    case kHighLevelEvent:
        set32(e, kEventParamAEEventClass, typeType, msg);
        set32(e, kEventParamAEEventID, typeType, where);
        break;
    }
    return e;
}

/* A Carbon event's classic form: its own, or one made from its class. */
static bool to_record(cevent *e, uint8_t r[16])
{
    if (e->has_record) {
        memcpy(r, e->record, 16);
        return true;
    }
    memset(r, 0, 16);
    uint32_t when = tb_TickCount(), where = 0, mods = 0, msg = 0;
    uint16_t what = nullEvent;
    if (e->cls == kEventClassMouse && (e->kind == kEventMouseDown || e->kind == kEventMouseUp)) {
        what = e->kind == kEventMouseDown ? mouseDown : mouseUp;
        get32(e, kEventParamMouseLocation, &where);
        get32(e, kEventParamKeyModifiers, &mods);
    } else if (e->cls == kEventClassKeyboard && e->kind >= kEventRawKeyDown && e->kind <= kEventRawKeyUp) {
        what = e->kind == kEventRawKeyDown ? keyDown : e->kind == kEventRawKeyRepeat ? autoKey : keyUp;
        uint32_t code = 0;
        get32(e, kEventParamKeyCode, &code);
        get32(e, kEventParamKeyModifiers, &mods);
        param *p = find_param(e, kEventParamKeyMacCharCodes);
        msg = (code & 0xFF) << 8 | (p && p->size ? p->data[0] : 0);
    } else if (e->cls == kEventClassWindow && (e->kind == kEventWindowUpdate || e->kind == kEventWindowActivated ||
                                               e->kind == kEventWindowDeactivated)) {
        what = e->kind == kEventWindowUpdate ? updateEvt : activateEvt;
        msg = direct_object(e);
        if (e->kind == kEventWindowActivated)
            mods |= activeFlag;
    } else {
        return false;
    }
    r[0] = (uint8_t)(what >> 8), r[1] = (uint8_t)what;
    for (int k = 0; k < 4; k++) {
        r[2 + k] = (uint8_t)(msg >> (24 - 8 * k));
        r[6 + k] = (uint8_t)(when >> (24 - 8 * k));
        r[10 + k] = (uint8_t)(where >> (24 - 8 * k));
    }
    r[14] = (uint8_t)(mods >> 8), r[15] = (uint8_t)mods;
    return true;
}

/* The classic event mask an event type list can match. */
static uint16_t classic_mask(uint32_t n, gaddr list)
{
    if (!n || !list)
        return 0xFFFF;
    uint16_t mask = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cls = rd32(list + 8 * i), kind = rd32(list + 8 * i + 4);
        switch (cls) {
        case kEventClassMouse:
            mask |= kind == kEventMouseDown ? 1 << mouseDown : kind == kEventMouseUp ? 1 << mouseUp : 1 << osEvt;
            break;
        case kEventClassKeyboard:
            mask |= kind == kEventRawKeyDown ? 1 << keyDown : kind == kEventRawKeyRepeat ? 1 << autoKey
                  : kind == kEventRawKeyUp ? 1 << keyUp : 0;
            break;
        case kEventClassWindow:
            mask |= kind == kEventWindowUpdate ? 1 << updateEvt
                  : kind == kEventWindowActivated || kind == kEventWindowDeactivated ? 1 << activateEvt : 0;
            break;
        case kEventClassApplication:
            mask |= 1 << osEvt;
            break;
        case kEventClassAppleEvent:
            mask |= 0x0400; /* highLevelEventMask */
            break;
        }
    }
    return mask;
}

static bool in_list(cevent *e, uint32_t n, gaddr list)
{
    if (!n || !list)
        return true;
    for (uint32_t i = 0; i < n; i++)
        if (rd32(list + 8 * i) == e->cls && rd32(list + 8 * i + 4) == e->kind)
            return true;
    return false;
}

/* ---------------------------------------------------------------------- */
/* The queue */

static cevent *queue_head;

/* Into the queue after those of its priority or higher. */
static int32_t post(cevent *e, int16_t priority, bool front)
{
    if (e->queued)
        return eventAlreadyPostedErr;
    e->queued = true;
    e->priority = priority;
    e->retain++;
    cevent **p = &queue_head;
    while (*p && (front ? (*p)->priority > priority : (*p)->priority >= priority))
        p = &(*p)->qnext;
    e->qnext = *p;
    *p = e;
    return noErr;
}

static bool unqueue(cevent *e)
{
    for (cevent **p = &queue_head; *p; p = &(*p)->qnext)
        if (*p == e) {
            *p = e->qnext;
            e->qnext = NULL;
            e->queued = false;
            return true;
        }
    return false;
}

/* Out of the queue, its reference with it. */
static void remove_queued(cevent *e)
{
    if (unqueue(e))
        event_release(e);
}

/* ---------------------------------------------------------------------- */
/* Timers */

typedef struct ltimer {
    gaddr ref;
    double fire, interval; /* fire: INFINITY when not to fire */
    gaddr upp, data;
    bool busy, dead;
    struct ltimer *next;
} ltimer;

static ltimer *timers;

static void fire_timers(void)
{
    if (!timers)
        return;
    double now = now_secs();
    for (ltimer *t = timers; t; t = t->next) {
        if (t->dead || t->busy || t->fire > now)
            continue;
        if (t->interval > 0) {
            t->fire += t->interval;
            if (t->fire <= now)
                t->fire = now + t->interval;
        } else {
            t->fire = INFINITY;
        }
        /* EventLoopTimerUPP: pascal void (EventLoopTimerRef, void *). */
        t->busy = true;
        uint32_t args[2] = { t->ref, t->data };
        aem_call_proc(t->upp, 0x3C0, args, 2);
        t->busy = false;
        if (g_cpu->halted)
            break;
    }
    /* Those removed while firing. */
    for (ltimer **p = &timers; *p;) {
        ltimer *t = *p;
        if (t->dead && !t->busy) {
            *p = t->next;
            ref_free(t->ref);
            free(t);
        } else {
            p = &t->next;
        }
    }
}

static double next_fire(void)
{
    double next = INFINITY;
    for (ltimer *t = timers; t; t = t->next)
        if (!t->dead && t->fire < next)
            next = t->fire;
    return next;
}

/* ---------------------------------------------------------------------- */
/* The event loop */

static int receiving; /* ReceiveNextEvent is waiting: posted events stay for it */

int32_t carbon_event_loop_turn(int32_t sleep)
{
    if (!timers && !queue_head && !mp_have_tasks())
        return sleep;
    static int depth;
    if (depth > 4)
        return sleep;
    depth++;
    fire_timers();
    mp_run_tasks();
    if (!receiving) {
        /* Events posted to the queue, sent while the program waits. */
        while (queue_head && !g_cpu->halted) {
            cevent *e = queue_head;
            unqueue(e);
            send(e, dispatcher());
            event_release(e);
        }
    }
    depth--;
    double next = next_fire();
    if (next != INFINITY) {
        double ticks = (next - now_secs()) * 60.0;
        if (ticks < sleep)
            sleep = ticks > 0 ? (int32_t)ceil(ticks) : 0;
    }
    if (mp_have_tasks() && sleep > 1)
        sleep = 1; /* tasks get their turns often */
    return sleep;
}

/* ReceiveNextEvent's work: the queue first, then the classic queue, until
 * an event matches or the time runs out. */
static int32_t receive(uint32_t n, gaddr list, double timeout, bool pull, cevent **out)
{
    *out = NULL;
    double deadline = timeout < 0 ? INFINITY : now_secs() + timeout;
    gaddr rec = tb_NewPtr(16, false, true);
    if (!rec)
        return eventInternalErr;
    uint16_t mask = classic_mask(n, list);
    int32_t err = eventLoopTimedOutErr;
    receiving++;
    for (;;) {
        if (quit_requests) {
            quit_requests--;
            err = eventLoopQuitErr;
            break;
        }
        if (g_cpu->halted) {
            err = eventLoopQuitErr;
            break;
        }
        fire_timers();
        mp_run_tasks();
        cevent *e = queue_head;
        while (e && !in_list(e, n, list))
            e = e->qnext;
        if (e) {
            if (pull)
                unqueue(e); /* the queue's reference is the caller's */
            *out = e;
            err = noErr;
            break;
        }
        double now = now_secs(), until = deadline < next_fire() ? deadline : next_fire();
        double ticks = until == INFINITY ? 60 : (until - now) * 60.0;
        int32_t sleep = ticks <= 0 ? 0 : ticks > 60 ? 60 : (int32_t)ceil(ticks);
        if (mask && tb_WaitNextEvent((int16_t)mask, rec, sleep, 0) && (int16_t)rd16(rec) != nullEvent) {
            e = from_record(rec);
            if (e) {
                if (!in_list(e, n, list)) {
                    post(e, 1, false); /* another caller's */
                    event_release(e);
                    continue;
                }
                if (!pull) {
                    post(e, 1, true);
                    event_release(e);
                }
                *out = e;
                err = noErr;
                break;
            }
        } else if (!mask) {
            ev_wait_tick();
        }
        if (timeout == 0 || now_secs() >= deadline) {
            err = eventLoopTimedOutErr;
            break;
        }
    }
    receiving--;
    tb_DisposePtr(rec);
    return err;
}

/* Run the loop: events received and dispatched until quit or timeout. */
static int32_t run_loop(double timeout, bool *quit)
{
    double deadline = timeout < 0 ? INFINITY : now_secs() + timeout;
    for (;;) {
        double left = deadline == INFINITY ? -1 : deadline - now_secs();
        if (deadline != INFINITY && left < 0)
            left = 0;
        cevent *e;
        int32_t err = receive(0, 0, left, true, &e);
        if (err == eventLoopQuitErr) {
            if (!quit || *quit || g_cpu->halted)
                return eventLoopQuitErr;
            continue; /* another loop's quit: this one goes on */
        }
        if (err == eventLoopTimedOutErr)
            return err;
        if (err)
            return err;
        send(e, dispatcher());
        event_release(e);
        if (quit && *quit)
            return eventLoopQuitErr;
    }
}

/* ---------------------------------------------------------------------- */
/* Exports: events */

static double fpr1(ppc_cpu *c) { return ppc_fpr(c, 1); }

/* A wait the machine's stopping cut short: the call is left unfinished
 * (its own address in pc, so going on runs it again), not answered with a
 * result the program would act on. */
#define RETURN_WAIT(v) do { if (g_cpu->halted) return; RETURN(v); } while (0)

/* CreateEvent(allocator, classID, kind, when, flags, &event): when in f1. */
static void ce_CreateEvent(ppc_cpu *c)
{
    gaddr out = R(6);
    cevent *e = event_new(R(1), R(2), fpr1(c));
    if (!e)
        RETURN(memFullErr);
    e->attrs = R(5);
    if (out)
        wr32(out, e->ref);
    RETURN(noErr);
}

static void ce_CopyEvent(ppc_cpu *c)
{
    cevent *o = ref_get(R(0), REF_EVENT);
    cevent *e = o ? event_new(o->cls, o->kind, o->when) : NULL;
    if (!e)
        RETURN(0);
    e->attrs = o->attrs;
    e->has_record = o->has_record;
    memcpy(e->record, o->record, 16);
    for (int i = 0; i < o->nparams; i++)
        set_param(e, o->params[i].name, o->params[i].type, o->params[i].size, o->params[i].data);
    RETURN(e->ref);
}

static void ce_RetainEvent(ppc_cpu *c)
{
    cevent *e = ref_get(R(0), REF_EVENT);
    if (e)
        e->retain++;
    RETURN(R(0));
}

static void ce_GetEventRetainCount(ppc_cpu *c)
{
    cevent *e = ref_get(R(0), REF_EVENT);
    RETURN(e ? e->retain : 0);
}

static void ce_ReleaseEvent(ppc_cpu *c)
{
    event_release(ref_get(R(0), REF_EVENT));
    c->pc = c->lr;
}

/* SetEventParameter(event, name, type, size, data) */
static void ce_SetEventParameter(ppc_cpu *c)
{
    cevent *e = ref_get(R(0), REF_EVENT);
    if (!e || (R(3) && !R(4)))
        RETURN(paramErr);
    uint8_t *buf = malloc(R(3) ? R(3) : 1);
    if (!buf)
        RETURN(memFullErr);
    if (R(3))
        gget(buf, R(4), R(3));
    bool ok = set_param(e, R(1), R(2), R(3), buf);
    free(buf);
    RETURN(ok ? noErr : memFullErr);
}

static bool integer_type(uint32_t t)
{
    return t == typeUInt32 || t == typeSInt32 || t == typeSInt16 || t == typeBoolean;
}

static int64_t integer_value(const param *p)
{
    if (p->type == typeSInt16 && p->size >= 2)
        return (int16_t)(p->data[0] << 8 | p->data[1]);
    if (p->type == typeBoolean && p->size >= 1)
        return p->data[0] != 0;
    if (p->size >= 4) {
        uint32_t v = be32(p->data);
        return p->type == typeSInt32 ? (int64_t)(int32_t)v : (int64_t)v;
    }
    return 0;
}

/* GetEventParameter(event, name, desiredType, &actualType, bufferSize,
 * &actualSize, data): the bytes as they are, or a number in another
 * numeric type. */
static void ce_GetEventParameter(ppc_cpu *c)
{
    cevent *e = ref_get(R(0), REF_EVENT);
    uint32_t want = R(2), bufsize = R(4);
    gaddr actual_type = R(3), actual_size = R(5), out = R(6);
    if (!e)
        RETURN(paramErr);
    param *p = find_param(e, R(1));
    if (!p)
        RETURN(eventParameterNotFoundErr);
    uint8_t conv[4];
    const uint8_t *data = p->data;
    uint32_t size = p->size, type = p->type;
    if (want != typeWildCard && want != p->type) {
        if (!(integer_type(want) && integer_type(p->type)))
            RETURN(errAECoercionFail);
        int64_t v = integer_value(p);
        size = want == typeSInt16 ? 2 : want == typeBoolean ? 1 : 4;
        for (uint32_t k = 0; k < size; k++)
            conv[k] = (uint8_t)((uint64_t)(want == typeBoolean ? v != 0 : v) >> (8 * (size - 1 - k)));
        data = conv;
        type = want;
    }
    if (actual_type)
        wr32(actual_type, type);
    if (actual_size)
        wr32(actual_size, size);
    if (out)
        gput(out, data, size < bufsize ? size : bufsize);
    RETURN(noErr);
}

static void ce_GetEventClass(ppc_cpu *c)
{
    cevent *e = ref_get(R(0), REF_EVENT);
    RETURN(e ? e->cls : 0);
}

static void ce_GetEventKind(ppc_cpu *c)
{
    cevent *e = ref_get(R(0), REF_EVENT);
    RETURN(e ? e->kind : 0);
}

static void ce_GetEventTime(ppc_cpu *c)
{
    cevent *e = ref_get(R(0), REF_EVENT);
    ppc_set_fpr(c, 1, e ? e->when : 0);
    c->pc = c->lr;
}

static void ce_SetEventTime(ppc_cpu *c)
{
    cevent *e = ref_get(R(0), REF_EVENT);
    if (!e)
        RETURN(paramErr);
    e->when = fpr1(c);
    RETURN(noErr);
}

static void ce_GetCurrentEventTime(ppc_cpu *c)
{
    ppc_set_fpr(c, 1, now_secs());
    c->pc = c->lr;
}

static void ce_GetLastUserEventTime(ppc_cpu *c)
{
    ppc_set_fpr(c, 1, last_user_time);
    c->pc = c->lr;
}

static void ce_ConvertEventRefToEventRecord(ppc_cpu *c)
{
    cevent *e = ref_get(R(0), REF_EVENT);
    uint8_t r[16] = { 0 };
    bool ok = e && to_record(e, r);
    if (!ok) {
        uint32_t when = tb_TickCount();
        for (int k = 0; k < 4; k++)
            r[6 + k] = (uint8_t)(when >> (24 - 8 * k));
    }
    if (R(1))
        gput(R(1), r, 16);
    RETURN(ok);
}

static void ce_IsEventInMask(ppc_cpu *c)
{
    cevent *e = ref_get(R(0), REF_EVENT);
    uint8_t r[16];
    if (!e || !to_record(e, r))
        RETURN(0);
    RETURN(((uint16_t)R(1) & (1u << (r[0] << 8 | r[1]))) != 0);
}

/* A key-down that cancels: Escape, or Command-period. */
static void ce_IsUserCancelEventRef(ppc_cpu *c)
{
    cevent *e = ref_get(R(0), REF_EVENT);
    if (!e || e->cls != kEventClassKeyboard || e->kind != kEventRawKeyDown)
        RETURN(0);
    uint32_t mods = 0;
    get32(e, kEventParamKeyModifiers, &mods);
    param *p = find_param(e, kEventParamKeyMacCharCodes);
    uint8_t ch = p && p->size ? p->data[0] : 0;
    RETURN(ch == 0x1B || ((mods & cmdKey) && ch == '.'));
}

/* ---- The queue ---- */

static gaddr main_queue_ref, main_loop_ref;

static void ce_GetMainEventQueue(ppc_cpu *c)
{
    if (!main_queue_ref)
        main_queue_ref = ref_new(REF_QUEUE, &queue_head);
    RETURN(main_queue_ref);
}

static void ce_GetMainEventLoop(ppc_cpu *c)
{
    if (!main_loop_ref)
        main_loop_ref = ref_new(REF_LOOP, &queue_head);
    RETURN(main_loop_ref);
}

static bool is_queue(gaddr q) { return q && q == main_queue_ref; }

/* PostEventToQueue(queue, event, priority) */
static void ce_PostEventToQueue(ppc_cpu *c)
{
    cevent *e = ref_get(R(1), REF_EVENT);
    if (!is_queue(R(0)) || !e)
        RETURN(paramErr);
    RETURN(post(e, (int16_t)R(2), false));
}

static void ce_FlushEventQueue(ppc_cpu *c)
{
    if (!is_queue(R(0)))
        RETURN(paramErr);
    while (queue_head)
        remove_queued(queue_head);
    tb_FlushEvents((int16_t)0xFFFF, 0);
    RETURN(noErr);
}

static void ce_FlushEventsMatchingListFromQueue(ppc_cpu *c)
{
    if (!is_queue(R(0)))
        RETURN(paramErr);
    for (cevent *e = queue_head, *next; e; e = next) {
        next = e->qnext;
        if (in_list(e, R(1), R(2)))
            remove_queued(e);
    }
    RETURN(noErr);
}

/* EventComparatorUPP: pascal Boolean (EventRef, void *). */
static bool compare(gaddr upp, cevent *e, gaddr data)
{
    uint32_t args[2] = { e->ref, data };
    return (aem_call_proc(upp, 0x3D0, args, 2) & 0xFF) != 0;
}

static void ce_FlushSpecificEventsFromQueue(ppc_cpu *c)
{
    gaddr upp = R(1), data = R(2);
    if (!is_queue(R(0)) || !upp)
        RETURN(paramErr);
    cevent *match[256];
    int n = 0;
    for (cevent *e = queue_head; e && n < 256; e = e->qnext)
        match[n++] = e;
    for (int i = 0; i < n; i++) {
        match[i]->retain++;
        if (match[i]->queued && compare(upp, match[i], data))
            remove_queued(match[i]);
        event_release(match[i]);
    }
    RETURN(noErr);
}

static void ce_FindSpecificEventInQueue(ppc_cpu *c)
{
    gaddr upp = R(1), data = R(2);
    if (!is_queue(R(0)) || !upp)
        RETURN(0);
    for (cevent *e = queue_head; e; e = e->qnext)
        if (compare(upp, e, data))
            RETURN(e->ref);
    RETURN(0);
}

static void ce_GetNumEventsInQueue(ppc_cpu *c)
{
    uint32_t n = 0;
    if (is_queue(R(0)))
        for (cevent *e = queue_head; e; e = e->qnext)
            n++;
    RETURN(n);
}

static void ce_RemoveEventFromQueue(ppc_cpu *c)
{
    cevent *e = ref_get(R(1), REF_EVENT);
    if (!is_queue(R(0)) || !e)
        RETURN(paramErr);
    if (!e->queued)
        RETURN(eventNotInQueueErr);
    remove_queued(e);
    RETURN(noErr);
}

static void ce_IsEventInQueue(ppc_cpu *c)
{
    cevent *e = ref_get(R(1), REF_EVENT);
    RETURN(is_queue(R(0)) && e && e->queued);
}

/* ---- Event loops ---- */

/* ReceiveNextEvent(numTypes, list, timeout, pullEvent, &event): timeout in f1. */
static void ce_ReceiveNextEvent(ppc_cpu *c)
{
    uint32_t n = R(0);
    gaddr list = R(1), out = R(5);
    bool pull = (R(4) & 0xFF) != 0;
    double timeout = fpr1(c);
    cevent *e;
    int32_t err = receive(n, list, timeout, pull, &e);
    if (out)
        wr32(out, err == noErr ? e->ref : 0);
    RETURN_WAIT(err);
}

static void ce_RunCurrentEventLoop(ppc_cpu *c) { RETURN_WAIT(run_loop(fpr1(c), NULL)); }

static void ce_QuitEventLoop(ppc_cpu *c)
{
    quit_requests++;
    RETURN(noErr);
}

static void ce_RunApplicationEventLoop(ppc_cpu *c)
{
    application()->standard = true;
    bool outer = quit_app;
    quit_app = false;
    app_loops++;
    run_loop(-1, &quit_app);
    app_loops--;
    quit_app = outer;
    if (!g_cpu->halted)
        c->pc = c->lr;
}

static void ce_QuitApplicationEventLoop(ppc_cpu *c)
{
    quit_application_loop();
    c->pc = c->lr;
}

/* ---- App-modal windows ---- */

static void ce_BeginAppModalStateForWindow(ppc_cpu *c)
{
    gaddr w = R(0);
    if (!window_exists(w))
        RETURN(errInvalidWindowRef);
    for (int i = 0; i < nmodal; i++)
        if (modal[i].window == w)
            RETURN(errWindowAlreadyInModalState);
    if (nmodal == (int)(sizeof modal / sizeof modal[0]))
        RETURN(eventInternalErr);
    modal[nmodal++] = (modal_level){ w, false };
    RETURN(noErr);
}

static int32_t end_modal(gaddr w)
{
    for (int i = nmodal - 1; i >= 0; i--)
        if (modal[i].window == w) {
            memmove(&modal[i], &modal[i + 1], sizeof modal[0] * (size_t)(nmodal - i - 1));
            nmodal--;
            return noErr;
        }
    return errWindowNotInModalState;
}

static void ce_EndAppModalStateForWindow(ppc_cpu *c) { RETURN(end_modal(R(0))); }

static void ce_RunAppModalLoopForWindow(ppc_cpu *c)
{
    gaddr w = R(0);
    if (!window_exists(w))
        RETURN(errInvalidWindowRef);
    if (nmodal == (int)(sizeof modal / sizeof modal[0]))
        RETURN(eventInternalErr);
    int level = nmodal;
    modal[nmodal++] = (modal_level){ w, false };
    application()->standard = true;
    while (!modal[level].quit && !g_cpu->halted) {
        cevent *e;
        int32_t err = receive(0, 0, -1, true, &e);
        if (err == eventLoopQuitErr) {
            if (g_cpu->halted || quit_app)
                break;
            continue;
        }
        if (err)
            break;
        send(e, dispatcher());
        event_release(e);
    }
    end_modal(w);
    RETURN_WAIT(noErr);
}

static void ce_QuitAppModalLoopForWindow(ppc_cpu *c)
{
    for (int i = nmodal - 1; i >= 0; i--)
        if (modal[i].window == R(0)) {
            modal[i].quit = true;
            quit_requests++;
            RETURN(noErr);
        }
    RETURN(errWindowNotInModalState);
}

/* ---- Targets ---- */

static void ce_GetApplicationEventTarget(ppc_cpu *c) { RETURN(application()->ref); }
static void ce_GetEventDispatcherTarget(ppc_cpu *c) { RETURN(dispatcher()->ref); }
static void ce_GetUserFocusEventTarget(ppc_cpu *c) { RETURN(ce_focus_target()); }
static void ce_GetWindowEventTarget(ppc_cpu *c) { RETURN(ce_window_target(R(0))); }
static void ce_GetControlEventTarget(ppc_cpu *c) { RETURN(ce_control_target(R(0))); }
static void ce_GetMenuEventTarget(ppc_cpu *c) { RETURN(ce_menu_target(R(0))); }

/* InstallEventHandler(target, handler, numTypes, list, userData, &handlerRef) */
static void ce_InstallEventHandler(ppc_cpu *c)
{
    target *t = ref_get(R(0), REF_TARGET);
    gaddr upp = R(1), list = R(3), out = R(5);
    uint32_t n = R(2);
    if (out)
        wr32(out, 0);
    if (!t || !upp || (n && !list) || n > 4096)
        RETURN(paramErr);
    handler *h = calloc(1, sizeof *h);
    if (!h)
        RETURN(memFullErr);
    h->types = calloc(n ? n : 1, sizeof *h->types);
    for (uint32_t i = 0; i < n; i++) {
        h->types[i][0] = rd32(list + 8 * i);
        h->types[i][1] = rd32(list + 8 * i + 4);
    }
    h->ntypes = (int)n;
    h->ref = ref_new(REF_HANDLER, h);
    h->target = t;
    h->upp = upp;
    h->data = R(4);
    h->next = t->handlers;
    t->handlers = h;
    any_handlers = true;
    if (out)
        wr32(out, h->ref);
    RETURN(noErr);
}

void carbon_install_standard_handler(gaddr window)
{
    target *t = target_of(T_WINDOW, window, true);
    if (t) {
        t->standard = true;
        any_handlers = true;
    }
}

static void ce_InstallStandardEventHandler(ppc_cpu *c)
{
    target *t = ref_get(R(0), REF_TARGET);
    if (!t)
        RETURN(paramErr);
    t->standard = true;
    any_handlers = true;
    RETURN(noErr);
}

static void ce_RemoveEventHandler(ppc_cpu *c)
{
    handler *h = ref_get(R(0), REF_HANDLER);
    if (!h)
        RETURN(paramErr);
    ref_free(h->ref);
    h->ref = 0;
    unlink_handler(h);
    RETURN(noErr);
}

static void ce_AddEventTypesToHandler(ppc_cpu *c)
{
    handler *h = ref_get(R(0), REF_HANDLER);
    uint32_t n = R(1);
    gaddr list = R(2);
    if (!h || (n && !list) || n > 4096)
        RETURN(paramErr);
    uint32_t (*types)[2] = realloc(h->types, sizeof *types * (size_t)(h->ntypes + (int)n + 1));
    if (!types)
        RETURN(memFullErr);
    h->types = types;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cls = rd32(list + 8 * i), kind = rd32(list + 8 * i + 4);
        bool have = false;
        for (int k = 0; k < h->ntypes; k++)
            have |= h->types[k][0] == cls && h->types[k][1] == kind;
        if (!have) {
            h->types[h->ntypes][0] = cls;
            h->types[h->ntypes++][1] = kind;
        }
    }
    RETURN(noErr);
}

static void ce_RemoveEventTypesFromHandler(ppc_cpu *c)
{
    handler *h = ref_get(R(0), REF_HANDLER);
    uint32_t n = R(1);
    gaddr list = R(2);
    if (!h || (n && !list))
        RETURN(paramErr);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cls = rd32(list + 8 * i), kind = rd32(list + 8 * i + 4);
        for (int k = 0; k < h->ntypes; k++)
            if (h->types[k][0] == cls && h->types[k][1] == kind) {
                memmove(&h->types[k], &h->types[k + 1], sizeof h->types[0] * (size_t)(h->ntypes - k - 1));
                h->ntypes--;
                break;
            }
    }
    RETURN(noErr);
}

/* CallNextEventHandler(callRef, event): the rest of the chain. */
static void ce_CallNextEventHandler(ppc_cpu *c)
{
    frame *f = ref_get(R(0), REF_CALL);
    if (!f)
        RETURN(paramErr);
    frame g = *f;
    f->next_called = true;
    g.next_called = false;
    RETURN(run_chain(&g));
}

static void ce_SendEventToEventTarget(ppc_cpu *c)
{
    cevent *e = ref_get(R(0), REF_EVENT);
    target *t = ref_get(R(1), REF_TARGET);
    if (!e || !t)
        RETURN(paramErr);
    RETURN(send(e, t));
}

/* ProcessHICommand(&command): kEventCommandProcess to the user focus. */
static void ce_ProcessHICommand(ppc_cpu *c)
{
    gaddr p = R(0);
    if (!p)
        RETURN(paramErr);
    cevent *e = event_new(kEventClassCommand, kEventCommandProcess, 0);
    if (!e)
        RETURN(memFullErr);
    uint8_t cmd[14];
    gget(cmd, p, sizeof cmd);
    set_param(e, kEventParamDirectObject, typeHICommand, sizeof cmd, cmd);
    RETURN(send_new(e, ref_get(ce_focus_target(), REF_TARGET)));
}

/* ---- Toolbox object classes: kept for the definitions that use them ---- */

typedef struct {
    gaddr ref, id, base, upp, data;
    uint32_t (*types)[2];
    uint32_t ntypes;
} object_class;

/* RegisterToolboxObjectClass(classID, baseClass, numEvents, list, handler, data, &classRef) */
static void ce_RegisterToolboxObjectClass(ppc_cpu *c)
{
    gaddr out = R(6);
    if (!R(0) || !R(4) || (R(2) && !R(3)))
        RETURN(paramErr);
    object_class *k = calloc(1, sizeof *k);
    if (!k)
        RETURN(memFullErr);
    k->id = R(0);
    k->base = R(1);
    k->upp = R(4);
    k->data = R(5);
    k->ntypes = R(2) < 4096 ? R(2) : 4096;
    k->types = calloc(k->ntypes ? k->ntypes : 1, sizeof *k->types);
    for (uint32_t i = 0; i < k->ntypes; i++) {
        k->types[i][0] = rd32(R(3) + 8 * i);
        k->types[i][1] = rd32(R(3) + 8 * i + 4);
    }
    k->ref = ref_new(REF_CLASS, k);
    if (out)
        wr32(out, k->ref);
    RETURN(noErr);
}

static void ce_UnregisterToolboxObjectClass(ppc_cpu *c)
{
    object_class *k = ref_get(R(0), REF_CLASS);
    if (!k)
        RETURN(paramErr);
    ref_free(k->ref);
    free(k->types);
    free(k);
    RETURN(noErr);
}

/* ---- User focus, default and cancel buttons ---- */

static void ce_SetUserFocusWindow(ppc_cpu *c)
{
    gaddr w = R(0);
    if (w && w != 0xFFFFFFFFu && !window_exists(w))
        RETURN(errInvalidWindowRef);
    gaddr old = focus_window();
    focus_window_set = w == 0xFFFFFFFFu ? 0 : w; /* kUserFocusAuto */
    gaddr now = focus_window();
    if (old != now) {
        if (old)
            carbon_send_window_event(old, kEventWindowFocusRelinquish);
        if (now)
            carbon_send_window_event(now, kEventWindowFocusAcquired);
    }
    RETURN(noErr);
}

static void ce_GetUserFocusWindow(ppc_cpu *c) { RETURN(focus_window()); }

static void set_button(ppc_cpu *c, bool dflt)
{
    gaddr w = R(0), ctl = R(1);
    if (!window_exists(w))
        RETURN(errInvalidWindowRef);
    target *t = target_of(T_WINDOW, w, true);
    gaddr *slot = dflt ? &t->default_button : &t->cancel_button;
    if (dflt) {
        /* The default ring moves with it (kControlPushButtonDefaultTag). */
        gaddr b = scratch() + 24;
        if (*slot && *slot != ctl && rd32(*slot)) {
            wr8(b, 0);
            tb_SetControlData(*slot, 0, 0x64666C74, 1, b);
        }
        if (ctl && rd32(ctl)) {
            wr8(b, 1);
            tb_SetControlData(ctl, 0, 0x64666C74, 1, b);
        }
    }
    *slot = ctl;
    RETURN(noErr);
}

static void ce_SetWindowDefaultButton(ppc_cpu *c) { set_button(c, true); }
static void ce_SetWindowCancelButton(ppc_cpu *c) { set_button(c, false); }

static void get_button(ppc_cpu *c, bool dflt)
{
    gaddr w = R(0), out = R(1);
    if (!window_exists(w))
        RETURN(errInvalidWindowRef);
    target *t = target_of(T_WINDOW, w, false);
    if (out)
        wr32(out, t ? (dflt ? t->default_button : t->cancel_button) : 0);
    RETURN(noErr);
}

static void ce_GetWindowDefaultButton(ppc_cpu *c) { get_button(c, true); }
static void ce_GetWindowCancelButton(ppc_cpu *c) { get_button(c, false); }

/* ---- Timers ---- */

/* InstallEventLoopTimer(loop, fireDelay, interval, proc, data, &timer): the
 * delays in f1 and f2, the rest after the GPRs they shadow. */
static void ce_InstallEventLoopTimer(ppc_cpu *c)
{
    double delay = ppc_fpr(c, 1), interval = ppc_fpr(c, 2);
    gaddr upp = R(5), data = R(6), out = R(7);
    if (out)
        wr32(out, 0);
    if (!upp)
        RETURN(paramErr);
    ltimer *t = calloc(1, sizeof *t);
    if (!t)
        RETURN(memFullErr);
    t->ref = ref_new(REF_TIMER, t);
    t->fire = delay < 0 ? INFINITY : now_secs() + delay;
    t->interval = interval > 0 ? interval : 0;
    t->upp = upp;
    t->data = data;
    t->next = timers;
    timers = t;
    if (out)
        wr32(out, t->ref);
    RETURN(noErr);
}

static void ce_RemoveEventLoopTimer(ppc_cpu *c)
{
    ltimer *t = ref_get(R(0), REF_TIMER);
    if (!t)
        RETURN(paramErr);
    t->dead = true;
    if (!t->busy) {
        for (ltimer **p = &timers; *p; p = &(*p)->next)
            if (*p == t) {
                *p = t->next;
                break;
            }
        ref_free(t->ref);
        free(t);
    }
    RETURN(noErr);
}

/* SetEventLoopTimerNextFireTime(timer, nextFire): from now, in f1; for
 * ever (a negative time) stops it. */
static void ce_SetEventLoopTimerNextFireTime(ppc_cpu *c)
{
    ltimer *t = ref_get(R(0), REF_TIMER);
    if (!t)
        RETURN(paramErr);
    double next = fpr1(c);
    t->fire = next < 0 ? INFINITY : now_secs() + next;
    RETURN(noErr);
}

/* ---- Mouse tracking ---- */

static uint32_t mouse_local(gaddr port)
{
    gaddr p = scratch() + 28;
    gaddr old = port ? port_push(port) : 0;
    tb_GetMouse(p);
    if (port)
        tb_SetPort(old);
    return rd32(p);
}

/* Wait for the mouse: moved, released, or the time up. */
static int track(gaddr port, gaddr rgn, bool *in, double timeout, uint32_t *pt, uint32_t *mods_out)
{
    double deadline = timeout < 0 ? INFINITY : now_secs() + timeout;
    uint32_t start = mouse_local(port);
    bool was_down = tb_Button() != 0;
    for (;;) {
        uint32_t now_pt = mouse_local(port);
        *pt = now_pt;
        *mods_out = ev_modifiers() & 0xFF00u;
        bool down = tb_Button() != 0;
        if (was_down && !down)
            return kMouseTrackingMouseReleased;
        if (!was_down && down)
            return kMouseTrackingMousePressed;
        if (rgn) {
            bool inside = tb_PtInRgn(now_pt, rgn) != 0;
            if (inside != *in) {
                *in = inside;
                return inside ? kMouseTrackingMouseEntered : kMouseTrackingMouseExited;
            }
        } else if (now_pt != start) {
            return kMouseTrackingMouseMoved;
        }
        if (g_cpu->halted)
            return kMouseTrackingUserCancelled;
        if (now_secs() >= deadline)
            return kMouseTrackingTimedOut;
        ev_wait_tick();
    }
}

static void ce_TrackMouseLocation(ppc_cpu *c)
{
    gaddr port = R(0), outpt = R(1), outres = R(2);
    uint32_t pt, mods;
    int r = track(port, 0, NULL, -1, &pt, &mods);
    if (outpt)
        wr32(outpt, pt);
    if (outres)
        wr16(outres, (uint16_t)r);
    RETURN(noErr);
}

/* TrackMouseLocationWithOptions(port, options, timeout, &pt, &modifiers, &result) */
static void ce_TrackMouseLocationWithOptions(ppc_cpu *c)
{
    gaddr port = R(0), outpt = R(4), outmods = R(5), outres = R(6);
    uint32_t pt, mods;
    int r = track(port, 0, NULL, fpr1(c), &pt, &mods);
    if (outpt)
        wr32(outpt, pt);
    if (outmods)
        wr32(outmods, mods);
    if (outres)
        wr16(outres, (uint16_t)r);
    RETURN(noErr);
}

/* TrackMouseRegion(port, region, &wasInRgn, &result) */
static void ce_TrackMouseRegion(ppc_cpu *c)
{
    gaddr port = R(0), rgn = R(1), inp = R(2), outres = R(3);
    if (!rgn || !inp)
        RETURN(paramErr);
    bool in = rd8(inp) != 0;
    uint32_t pt, mods;
    int r = track(port, rgn, &in, -1, &pt, &mods);
    wr8(inp, in);
    if (outres)
        wr16(outres, (uint16_t)r);
    RETURN(noErr);
}

/* ---- Hot keys: a key-down with these modifiers goes to the target as
 * kEventHotKeyPressed (while this program runs) ---- */

/* RegisterEventHotKey(keyCode, modifiers, hotKeyID {signature, id}, target, options, &ref) */
static void ce_RegisterEventHotKey(ppc_cpu *c)
{
    target *t = ref_get(R(4), REF_TARGET);
    gaddr out = R(6);
    if (!t)
        RETURN(paramErr);
    for (hotkey *h = hotkeys; h; h = h->next)
        if (h->code == R(0) && h->mods == R(1))
            RETURN(eventHotKeyExistsErr);
    hotkey *h = calloc(1, sizeof *h);
    if (!h)
        RETURN(memFullErr);
    *h = (hotkey){ ref_new(REF_HOTKEY, h), R(0), R(1), R(2), R(3), t, hotkeys };
    hotkeys = h;
    any_handlers = true;
    if (out)
        wr32(out, h->ref);
    RETURN(noErr);
}

static void ce_UnregisterEventHotKey(ppc_cpu *c)
{
    hotkey *h = ref_get(R(0), REF_HOTKEY);
    if (!h)
        RETURN(eventHotKeyInvalidErr);
    for (hotkey **p = &hotkeys; *p; p = &(*p)->next)
        if (*p == h) {
            *p = h->next;
            break;
        }
    ref_free(h->ref);
    free(h);
    RETURN(noErr);
}

/* ---- Universal procedure pointers: routine descriptors, as CarbonLib
 * made them on Mac OS 9 ---- */

static gaddr new_upp(gaddr proc, uint32_t procinfo)
{
    if (!proc)
        return 0;
    uint32_t args[3] = { proc, procinfo, 1 }; /* kPowerPCISA */
    return call_export("NewRoutineDescriptor", args, 3);
}

static void dispose_upp(ppc_cpu *c)
{
    if (mm_is_descriptor(R(0)))
        tb_DisposePtr(R(0));
    c->pc = c->lr;
}

static void ce_NewEventHandlerUPP(ppc_cpu *c) { RETURN(new_upp(R(0), 0xFF0)); }
static void ce_NewEventLoopTimerUPP(ppc_cpu *c) { RETURN(new_upp(R(0), 0x3C0)); }
static void ce_NewEventComparatorUPP(ppc_cpu *c) { RETURN(new_upp(R(0), 0x3D0)); }

/* InvokeEventHandlerUPP(callRef, event, userData, upp) */
static void ce_InvokeEventHandlerUPP(ppc_cpu *c)
{
    uint32_t args[3] = { R(0), R(1), R(2) };
    RETURN(aem_call_proc(R(3), 0xFF0, args, 3));
}

static void ce_InvokeEventLoopTimerUPP(ppc_cpu *c)
{
    uint32_t args[2] = { R(0), R(1) };
    aem_call_proc(R(2), 0x3C0, args, 2);
    c->pc = c->lr;
}

static void ce_InvokeEventComparatorUPP(ppc_cpu *c)
{
    uint32_t args[2] = { R(0), R(1) };
    RETURN(aem_call_proc(R(2), 0x3D0, args, 2) & 0xFF);
}

/* ---------------------------------------------------------------------- */
/* CarbonLib's own setting up */

void carbonlib_init(void)
{
    gaddr qd = tb_NewPtr(QDGlobals_SIZEOF, false, true);
    if (qd)
        tb_InitGraf(qd + QDGlobals_thePort_off);
    tb_InitFonts();
    tb_InitWindows();
    tb_InitMenus();
    tb_TEInit();
    tb_InitDialogs(0);
    tb_InitCursor();
}

void carbon_events_reset(void)
{
    while (queue_head) {
        cevent *e = queue_head;
        queue_head = e->qnext;
        e->queued = false;
    }
    while (targets) {
        target *t = targets;
        targets = t->next;
        while (t->handlers) {
            handler *h = t->handlers;
            t->handlers = h->next;
            free(h->types);
            free(h);
        }
        free(t);
    }
    while (timers) {
        ltimer *t = timers;
        timers = t->next;
        free(t);
    }
    while (hotkeys) {
        hotkey *h = hotkeys;
        hotkeys = h->next;
        free(h);
    }
    for (int b = 0; b < ref_blocks; b++)
        free(ref_tab[b]);
    ref_blocks = 0;
    app_target = dispatcher_target = focus_target = NULL;
    dead_handlers = NULL;
    dead_targets = NULL;
    send_depth = 0;
    any_handlers = false;
    focus_window_set = 0;
    quit_app = false;
    quit_requests = 0;
    app_loops = 0;
    nmodal = 0;
    receiving = 0;
    main_queue_ref = main_loop_ref = 0;
    last_click_when = last_click_where = click_count = 0;
    last_user_time = 0;
    mp_reset();
}

const ppc_export ce_exports[] = {
    { "AddEventTypesToHandler", ce_AddEventTypesToHandler },
    { "BeginAppModalStateForWindow", ce_BeginAppModalStateForWindow },
    { "CallNextEventHandler", ce_CallNextEventHandler },
    { "ConvertEventRefToEventRecord", ce_ConvertEventRefToEventRecord },
    { "CopyEvent", ce_CopyEvent },
    { "CreateEvent", ce_CreateEvent },
    { "DisposeEventComparatorUPP", dispose_upp },
    { "DisposeEventHandlerUPP", dispose_upp },
    { "DisposeEventLoopTimerUPP", dispose_upp },
    { "EndAppModalStateForWindow", ce_EndAppModalStateForWindow },
    { "FindSpecificEventInQueue", ce_FindSpecificEventInQueue },
    { "FlushEventQueue", ce_FlushEventQueue },
    { "FlushEventsMatchingListFromQueue", ce_FlushEventsMatchingListFromQueue },
    { "FlushSpecificEventsFromQueue", ce_FlushSpecificEventsFromQueue },
    { "GetApplicationEventTarget", ce_GetApplicationEventTarget },
    { "GetControlEventTarget", ce_GetControlEventTarget },
    { "GetCurrentEventLoop", ce_GetMainEventLoop },
    { "GetCurrentEventQueue", ce_GetMainEventQueue },
    { "GetCurrentEventTime", ce_GetCurrentEventTime },
    { "GetEventClass", ce_GetEventClass },
    { "GetEventDispatcherTarget", ce_GetEventDispatcherTarget },
    { "GetEventKind", ce_GetEventKind },
    { "GetEventParameter", ce_GetEventParameter },
    { "GetEventRetainCount", ce_GetEventRetainCount },
    { "GetEventTime", ce_GetEventTime },
    { "GetLastUserEventTime", ce_GetLastUserEventTime },
    { "GetMainEventLoop", ce_GetMainEventLoop },
    { "GetMainEventQueue", ce_GetMainEventQueue },
    { "GetMenuEventTarget", ce_GetMenuEventTarget },
    { "GetNumEventsInQueue", ce_GetNumEventsInQueue },
    { "GetUserFocusEventTarget", ce_GetUserFocusEventTarget },
    { "GetUserFocusWindow", ce_GetUserFocusWindow },
    { "GetWindowCancelButton", ce_GetWindowCancelButton },
    { "GetWindowDefaultButton", ce_GetWindowDefaultButton },
    { "GetWindowEventTarget", ce_GetWindowEventTarget },
    { "InstallEventHandler", ce_InstallEventHandler },
    { "InstallEventLoopTimer", ce_InstallEventLoopTimer },
    { "InstallStandardEventHandler", ce_InstallStandardEventHandler },
    { "InvokeEventComparatorUPP", ce_InvokeEventComparatorUPP },
    { "InvokeEventHandlerUPP", ce_InvokeEventHandlerUPP },
    { "InvokeEventLoopTimerUPP", ce_InvokeEventLoopTimerUPP },
    { "IsEventInMask", ce_IsEventInMask },
    { "IsEventInQueue", ce_IsEventInQueue },
    { "IsUserCancelEventRef", ce_IsUserCancelEventRef },
    { "MacCreateEvent", ce_CreateEvent },
    { "NewEventComparatorUPP", ce_NewEventComparatorUPP },
    { "NewEventHandlerUPP", ce_NewEventHandlerUPP },
    { "NewEventLoopTimerUPP", ce_NewEventLoopTimerUPP },
    { "PostEventToQueue", ce_PostEventToQueue },
    { "ProcessHICommand", ce_ProcessHICommand },
    { "QuitAppModalLoopForWindow", ce_QuitAppModalLoopForWindow },
    { "QuitApplicationEventLoop", ce_QuitApplicationEventLoop },
    { "QuitEventLoop", ce_QuitEventLoop },
    { "ReceiveNextEvent", ce_ReceiveNextEvent },
    { "RegisterEventHotKey", ce_RegisterEventHotKey },
    { "RegisterToolboxObjectClass", ce_RegisterToolboxObjectClass },
    { "ReleaseEvent", ce_ReleaseEvent },
    { "RemoveEventFromQueue", ce_RemoveEventFromQueue },
    { "RemoveEventHandler", ce_RemoveEventHandler },
    { "RemoveEventLoopTimer", ce_RemoveEventLoopTimer },
    { "RemoveEventTypesFromHandler", ce_RemoveEventTypesFromHandler },
    { "RetainEvent", ce_RetainEvent },
    { "RunAppModalLoopForWindow", ce_RunAppModalLoopForWindow },
    { "RunApplicationEventLoop", ce_RunApplicationEventLoop },
    { "RunCurrentEventLoop", ce_RunCurrentEventLoop },
    { "SendEventToEventTarget", ce_SendEventToEventTarget },
    { "SetEventLoopTimerNextFireTime", ce_SetEventLoopTimerNextFireTime },
    { "SetEventParameter", ce_SetEventParameter },
    { "SetEventTime", ce_SetEventTime },
    { "SetUserFocusWindow", ce_SetUserFocusWindow },
    { "SetWindowCancelButton", ce_SetWindowCancelButton },
    { "SetWindowDefaultButton", ce_SetWindowDefaultButton },
    { "TrackMouseLocation", ce_TrackMouseLocation },
    { "TrackMouseLocationWithOptions", ce_TrackMouseLocationWithOptions },
    { "TrackMouseRegion", ce_TrackMouseRegion },
    { "UnregisterEventHotKey", ce_UnregisterEventHotKey },
    { "UnregisterToolboxObjectClass", ce_UnregisterToolboxObjectClass },
};
const unsigned ce_export_count = sizeof ce_exports / sizeof ce_exports[0];
