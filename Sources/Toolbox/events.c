/*
 * Event Manager.
 *
 * The host posts mouse and keyboard input (machine_post_*), possibly from
 * another thread; the guest takes it in priority order: activate events,
 * then queued input, then update events, then null events.
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "machine.h"
#include "qd_internal.h"
#include "tb_internal.h"

enum { nullEvent = 0, mouseDown, mouseUp, keyDown, keyUp, autoKey, updateEvt, diskEvt, activateEvt, osEvt = 15 };
enum { activeFlag = 1, btnState = 0x80, cmdKey = 0x100, shiftKey = 0x200, alphaLock = 0x400, optionKey = 0x800,
       controlKey = 0x1000 };

typedef struct {
    int16_t what;
    uint32_t message;
    uint32_t when;
    uint32_t where;
    uint16_t modifiers;
} event;

#define QSIZE 64
static event queue[QSIZE];
static int qhead, qtail;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t arrived = PTHREAD_COND_INITIALIZER;
static int16_t mouse_h, mouse_v;
static bool button_down;
static uint16_t key_mods;
static uint8_t keymap[16];
static bool host_attached;

/* ---------------------------------------------------------------------- */
/* Host side */

static void enqueue(int16_t what, uint32_t message)
{
    int next = (qtail + 1) % QSIZE;
    if (next == qhead) /* full: drop the oldest */
        qhead = (qhead + 1) % QSIZE;
    queue[qtail] = (event){ what, message, rd32(LM_Ticks), make_pt(mouse_v, mouse_h),
                            (uint16_t)(key_mods | (button_down ? 0 : btnState)) };
    qtail = next;
    pthread_cond_broadcast(&arrived);
}

void machine_attach_host(bool attached) { host_attached = attached; }

/* ---- Scripted input (headless runs) ----
 * Each step is due at an instruction count and is applied between
 * emulation slices or while the application waits for input, even inside
 * a Toolbox loop such as ModalDialog that runs no guest code. */

typedef struct {
    uint64_t at;
    int kind; /* 0 key, 1 mouse, 2 menu */
    int a, b, c;
} script_step;

static script_step *script;
static int nscript, script_next;

void machine_script_add(uint64_t at, int kind, int a, int b, int c)
{
    script = realloc(script, (size_t)(nscript + 1) * sizeof *script);
    script[nscript++] = (script_step){ at, kind, a, b, c };
}

/* The next step not yet applied: its instruction count, or 0. */
uint64_t events_script_next_at(void) { return script_next < nscript ? script[script_next].at : 0; }

void machine_choose_menu(int16_t menu_id, int16_t item);

void events_script_pump(uint64_t now)
{
    while (script_next < nscript && script[script_next].at <= now) {
        script_step s = script[script_next++];
        switch (s.kind) {
        case 0:
            machine_post_key(s.a, 0, (unsigned)s.b, 1, 0);
            machine_post_key(s.a, 0, (unsigned)s.b, 0, 0);
            break;
        case 1: machine_post_mouse(s.a, s.b, s.c); break;
        case 2: machine_choose_menu((int16_t)s.a, (int16_t)s.b); break;
        }
    }
}

/* The low-memory copies of the mouse that applications read directly:
 * MBState (bit 7 set while the button is up), and the mouse location as
 * Mouse, RawMouse and MTemp.  Kept current between emulation slices. */
void events_sync_lowmem(void)
{
    pthread_mutex_lock(&lock);
    uint32_t pt = make_pt(mouse_v, mouse_h);
    bool down = button_down;
    pthread_mutex_unlock(&lock);
    wr8(LM_MBState, down ? 0x00 : 0x80);
    wr32(LM_MTemp, pt);
    wr32(LM_MouseLocation, pt);
    wr32(LM_MouseLocation2, pt);
}
bool events_host_attached(void) { return host_attached; }

/* Suspend and resume (osEvt, suspendResumeMessage): the app moving to the
 * background and back; convert_clipboard says the scrap changed meanwhile. */
void machine_post_suspend_resume(bool resume, bool convert_clipboard)
{
    pthread_mutex_lock(&lock);
    enqueue(osEvt, 0x01000000u | (resume ? 1u : 0u) | (convert_clipboard ? 2u : 0u));
    pthread_mutex_unlock(&lock);
}

/* Wake an app sleeping in WaitNextEvent (e.g. an Apple Event arrived). */
void ev_wake(void)
{
    pthread_mutex_lock(&lock);
    pthread_cond_broadcast(&arrived);
    pthread_mutex_unlock(&lock);
}

bool aem_next_event(uint32_t *cls, uint32_t *id, bool remove);

void machine_post_mouse(int x, int y, int button)
{
    pthread_mutex_lock(&lock);
    mouse_h = (int16_t)x;
    mouse_v = (int16_t)y;
    if (button >= 0 && (button != 0) != button_down) {
        button_down = button != 0;
        enqueue(button_down ? mouseDown : mouseUp, 0);
    }
    pthread_cond_broadcast(&arrived);
    pthread_mutex_unlock(&lock);
}

void machine_post_key(int charcode, int keycode, unsigned modifiers, int down, int repeat)
{
    pthread_mutex_lock(&lock);
    key_mods = (uint16_t)modifiers;
    if (keycode >= 0 && keycode < 128) {
        if (down)
            keymap[keycode / 8] |= (uint8_t)(1u << (keycode % 8));
        else
            keymap[keycode / 8] &= (uint8_t)~(1u << (keycode % 8));
    }
    if (charcode >= 0)
        enqueue(down ? (repeat ? autoKey : keyDown) : keyUp, ((uint32_t)(keycode & 0xFF) << 8) | (uint8_t)charcode);
    pthread_mutex_unlock(&lock);
}

/* ---------------------------------------------------------------------- */
/* Mouse state for tracking loops */

bool ev_still_down(void)
{
    pthread_mutex_lock(&lock);
    bool d = button_down;
    pthread_mutex_unlock(&lock);
    return d;
}

uint32_t ev_mouse_global(void)
{
    pthread_mutex_lock(&lock);
    uint32_t p = make_pt(mouse_v, mouse_h);
    pthread_mutex_unlock(&lock);
    return p;
}

void machine_advance_ticks(void);
void display_sync(void);

/* A click in the menu bar, for a choice made in the host's menus. */
/* A click at a point (for actions from the host's window frames). */
void ev_post_click_at(int x, int y)
{
    pthread_mutex_lock(&lock);
    int16_t h = mouse_h, v = mouse_v;
    bool down = button_down;
    mouse_h = (int16_t)x;
    mouse_v = (int16_t)y;
    button_down = true;
    enqueue(mouseDown, 0);
    button_down = false;
    enqueue(mouseUp, 0);
    mouse_h = h;
    mouse_v = v;
    button_down = down;
    pthread_mutex_unlock(&lock);
}

void ev_post_menubar_click(void)
{
    pthread_mutex_lock(&lock);
    int16_t h = mouse_h, v = mouse_v;
    bool down = button_down;
    mouse_h = 1;
    mouse_v = 1;
    button_down = true;
    enqueue(mouseDown, 0);
    button_down = false;
    enqueue(mouseUp, 0);
    mouse_h = h;
    mouse_v = v;
    button_down = down;
    pthread_mutex_unlock(&lock);
}

/* Let time pass while native code waits on the user. */
void ev_wait_tick(void)
{
    machine_advance_ticks();
    if (!host_attached)
        return;
    pthread_mutex_lock(&lock);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 16000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    pthread_cond_timedwait(&arrived, &lock, &ts);
    pthread_mutex_unlock(&lock);
}

/* ---------------------------------------------------------------------- */
/* Guest side */

static void write_event(gaddr evt, const event *e)
{
    EventRecord_set_what(evt, e->what);
    EventRecord_set_message(evt, (int32_t)e->message);
    EventRecord_set_when(evt, (int32_t)e->when);
    wr32(evt + EventRecord_where_off, e->where);
    EventRecord_set_modifiers(evt, (int16_t)e->modifiers);
}

static event null_event(void)
{
    event e = { nullEvent, 0, rd32(LM_Ticks), make_pt(mouse_v, mouse_h),
                (uint16_t)(key_mods | (button_down ? 0 : btnState)) };
    return e;
}

/* Take (or peek at) the next event matching mask. */
static bool next_event(int16_t mask, gaddr evt, bool remove, bool os_only)
{
    pthread_mutex_lock(&lock);
    event e = null_event();
    bool found = false;
    if (!os_only && (mask & (1 << activateEvt))) {
        gaddr w = rd32(LM_CurDeactive);
        if (w) {
            e.what = activateEvt;
            e.message = w;
            e.modifiers &= (uint16_t)~activeFlag;
            if (remove)
                wr32(LM_CurDeactive, 0);
            found = true;
        } else if ((w = rd32(LM_CurActivate))) {
            e.what = activateEvt;
            e.message = w;
            e.modifiers |= activeFlag;
            if (remove)
                wr32(LM_CurActivate, 0);
            found = true;
        }
    }
    if (!found) {
        for (int i = qhead; i != qtail; i = (i + 1) % QSIZE) {
            if (mask & (1 << queue[i].what)) {
                e = queue[i];
                found = true;
                if (remove) {
                    for (int j = i; j != qhead; j = (j - 1 + QSIZE) % QSIZE)
                        queue[j] = queue[(j - 1 + QSIZE) % QSIZE];
                    qhead = (qhead + 1) % QSIZE;
                }
                break;
            }
        }
    }
    pthread_mutex_unlock(&lock);
    /* High-level events (Apple Events): message = class, where = ID. */
    uint32_t cls, aeid;
    if (!found && !os_only && (mask & 0x0400) && aem_next_event(&cls, &aeid, remove)) {
        e.what = 23;
        e.message = cls;
        e.where = aeid;
        found = true;
    }
    if (!found && !os_only && (mask & (1 << updateEvt))) {
        if (tb_CheckUpdate(evt)) {
            EventRecord_set_when(evt, (int32_t)rd32(LM_Ticks));
            wr32(evt + EventRecord_where_off, e.where);
            EventRecord_set_modifiers(evt, (int16_t)e.modifiers);
            return true;
        }
    }
    write_event(evt, &e);
    return found;
}

/* An update event the application never handles comes back forever, as on
 * a real Mac (input still gets through first).  Headless, that is the
 * application waiting for input too. */
static void note_repeated_update(gaddr evt)
{
    static uint32_t last_msg;
    static int repeats;
    if ((int16_t)rd16(evt) != updateEvt) {
        repeats = 0;
        return;
    }
    uint32_t msg = rd32(evt + EventRecord_message_off);
    repeats = msg == last_msg ? repeats + 1 : 0;
    last_msg = msg;
    if (repeats > 8 && !host_attached)
        machine_idle();
}

int8_t tb_GetNextEvent(int16_t em, gaddr evt)
{
    sound_pump();
    time_pump();
    notify_pump();
    machine_advance_ticks();
    display_sync();
    if (next_event(em, evt, true, false)) {
        LOGF(LOG_TRAPS, "  event %d message %08X\n", (int16_t)rd16(evt), rd32(evt + 2));
        note_repeated_update(evt);
        return 1;
    }
    /* Apps that poll GetNextEvent would spin the host CPU; a null event
     * waits up to a tick for input, as time passed on a real Mac too. */
    if (host_attached) {
        ev_wait_tick();
        return next_event(em, evt, true, false);
    }
    machine_idle();
    return 0;
}

int8_t tb_EventAvail(int16_t em, gaddr evt)
{
    display_sync();
    return next_event(em, evt, false, false);
}

int8_t tb_WaitNextEvent(int16_t mask, gaddr evp, int32_t sleep, gaddr mousergn)
{
    machine_advance_ticks();
    sound_pump();
    time_pump();
    notify_pump();
    display_sync();
    if (next_event(mask, evp, true, false)) {
        note_repeated_update(evp);
        return 1;
    }
    if (host_attached && sleep > 0) {
        /* Sleep until input arrives or the time is up. */
        uint32_t c0, i0;
        bool hle = (mask & 0x0400) && aem_next_event(&c0, &i0, false);
        pthread_mutex_lock(&lock);
        if (qhead == qtail && !hle) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            long long ns = (long long)(sleep > 600 ? 600 : sleep) * 1000000000LL / 60;
            ts.tv_sec += (time_t)(ns / 1000000000LL);
            ts.tv_nsec += (long)(ns % 1000000000LL);
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&arrived, &lock, &ts);
        }
        pthread_mutex_unlock(&lock);
        machine_advance_ticks();
        return next_event(mask, evp, true, false);
    }
    if (!host_attached)
        machine_idle();
    return 0;
}

bool tb_GetOSEvent(int16_t evmask, gaddr eventp) { return next_event(evmask, eventp, true, true); }
bool tb_OSEventAvail(int16_t evmask, gaddr eventp) { return next_event(evmask, eventp, false, true); }

void tb_FlushEvents(int16_t evmask, int16_t stopmask)
{
    pthread_mutex_lock(&lock);
    event keep[QSIZE];
    int n = 0;
    bool stop = false;
    for (int i = qhead; i != qtail; i = (i + 1) % QSIZE) {
        if (!stop && (stopmask & (1 << queue[i].what)))
            stop = true;
        if (stop || !(evmask & (1 << queue[i].what)))
            keep[n++] = queue[i];
    }
    qhead = 0;
    qtail = n;
    memcpy(queue, keep, (size_t)n * sizeof *keep);
    pthread_mutex_unlock(&lock);
}

int16_t tb_PostEvent(int16_t evcode, int32_t evmsg)
{
    pthread_mutex_lock(&lock);
    enqueue(evcode, (uint32_t)evmsg);
    pthread_mutex_unlock(&lock);
    return noErr;
}

void tb_SetEventMask(int16_t evmask) { wr16(LM_SysEvtMask, (uint16_t)evmask); }

int8_t tb_Button(void)
{
    /* Polling a released button is waiting for input, like a null event. */
    bool down = ev_still_down();
    if (!down && !host_attached)
        machine_idle();
    return down;
}
int8_t tb_StillDown(void)
{
    /* True if the button is down and no new mouse event is waiting. */
    return ev_still_down();
}

int8_t tb_WaitMouseUp(void)
{
    bool d = ev_still_down();
    if (!d) {
        pthread_mutex_lock(&lock);
        for (int i = qhead; i != qtail; i = (i + 1) % QSIZE)
            if (queue[i].what == mouseUp) {
                for (int j = i; j != qhead; j = (j - 1 + QSIZE) % QSIZE)
                    queue[j] = queue[(j - 1 + QSIZE) % QSIZE];
                qhead = (qhead + 1) % QSIZE;
                break;
            }
        pthread_mutex_unlock(&lock);
    }
    return d;
}

void tb_GetMouse(gaddr p)
{
    uint32_t g = ev_mouse_global();
    int16_t h = pt_h(g), v = pt_v(g);
    gaddr port = qd_port();
    if (port) {
        pixdst d = qd_port_pixels(port);
        h = (int16_t)(h + d.bounds.left);
        v = (int16_t)(v + d.bounds.top);
    }
    wr16(p, (uint16_t)v);
    wr16(p + 2, (uint16_t)h);
}

void tb_GetKeys(gaddr keys)
{
    pthread_mutex_lock(&lock);
    gput(keys, keymap, 16);
    pthread_mutex_unlock(&lock);
}

int32_t tb_GetDblTime(void) { return (int32_t)rd32(LM_DoubleTime); }
int32_t tb_GetCaretTime(void) { return (int32_t)rd32(LM_CaretTime); }

void tb_SystemTask(void)
{
    machine_advance_ticks();
    sound_pump();
    time_pump();
}
void tb_SystemClick(gaddr evp, gaddr wp) {}
int8_t tb_SystemEvent(gaddr evp) { return 0; }

void events_reset(void)
{
    pthread_mutex_lock(&lock);
    qhead = qtail = 0;
    button_down = false;
    key_mods = 0;
    memset(keymap, 0, sizeof keymap);
    mouse_h = 100;
    mouse_v = 100;
    pthread_mutex_unlock(&lock);
}
