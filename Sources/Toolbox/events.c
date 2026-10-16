/*
 * Event Manager.
 *
 * The host posts mouse and keyboard input (machine_post_*), possibly from
 * another thread; the guest takes it in priority order: activate events,
 * then queued input, then update events, then null events.
 */
#include <pthread.h>
#include <unistd.h>
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
static uint32_t qversion; /* changes with the queue, for its low-memory copy */
static void mirror_queue(void);
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t arrived = PTHREAD_COND_INITIALIZER;
static int16_t mouse_h, mouse_v;
static bool button_down;
static uint32_t presses, key_presses; /* counted, so a quick click is never missed */
static uint16_t key_mods;
static uint8_t keymap[16];
static bool host_attached;
/* For low memory: the last key down (key code and character) and whether
 * a key or the button changed since it was last written. */
static uint16_t key_last;
static bool key_new, button_new;

/* Keyboard and mouse globals missing from lowmem_gen.h. */
#define LM_MBTicks 0x016Eu    /* LONGINT: Ticks at the last button change */
#define LM_KeyLast 0x0184u    /* INTEGER: the last key's code and character */
#define LM_KeyTime 0x0186u    /* LONGINT: Ticks at the last key down */
#define LM_KeyRepTime 0x018Au /* LONGINT: Ticks at the last repeat */
#define LM_HiKeyLast 0x0216u  /* INTEGER: as KeyLast */

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
    qversion++;
    pthread_cond_broadcast(&arrived);
}

void events_start_stall_log(void);
void machine_attach_host(bool attached)
{
    host_attached = attached;
    if (attached)
        events_start_stall_log();
}

/* ---- Scripted input (headless runs) ----
 * Each step is due at an instruction count and is applied between
 * emulation slices or while the application waits for input, even inside
 * a Toolbox loop such as ModalDialog that runs no guest code. */

typedef struct {
    uint64_t at;
    int kind; /* 0 key, 1 mouse, 2 menu, 3 key down, 4 key up */
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
        case 0: /* a: character, b: modifiers, c: key code */
            machine_post_key(s.a, s.c, (unsigned)s.b, 1, 0);
            machine_post_key(s.a, s.c, (unsigned)s.b, 0, 0);
            break;
        case 3: machine_post_key(s.a, s.c, (unsigned)s.b, 1, 0); break; /* held down */
        case 4: machine_post_key(s.a, s.c, (unsigned)s.b, 0, 0); break; /* released */
        case 1: machine_post_mouse(s.a, s.b, s.c); break;
        case 2: machine_choose_menu((int16_t)s.a, (int16_t)s.b); break;
        }
    }
}

/* The low-memory copies of the mouse that applications read directly:
 * MBState (bit 7 set while the button is up), and the mouse location as
 * Mouse, RawMouse and MTemp.  Kept current between emulation slices. */
/* EventQueue in low memory: a copy of the queue as event records
 * (qLink, qType 4, evtQWhat, evtQMessage, evtQWhen, evtQWhere,
 * evtQModifiers), from a buffer of SysEvtCnt + 1 of them at SysEvtBuf,
 * for programs that look at it directly. */
#define LM_SysEvtBuf 0x0146u /* Ptr */
#define LM_SysEvtCnt 0x0154u /* INTEGER: the buffer's records less one */
enum { EVQEL = 22 };
static uint32_t mirrored = (uint32_t)-1;

static void mirror_queue(void)
{
    gaddr buf = rd32(LM_SysEvtBuf);
    if (!buf)
        return;
    pthread_mutex_lock(&lock);
    if (qversion == mirrored) {
        pthread_mutex_unlock(&lock);
        return;
    }
    mirrored = qversion;
    event copy[QSIZE];
    int n = 0;
    for (int i = qhead; i != qtail; i = (i + 1) % QSIZE)
        copy[n++] = queue[i];
    pthread_mutex_unlock(&lock);
    gaddr prev = 0;
    for (int i = 0; i < n; i++) {
        gaddr q = buf + (uint32_t)i * EVQEL;
        wr32(q, 0);
        wr16(q + 4, 4); /* evType */
        wr16(q + 6, (uint16_t)copy[i].what);
        wr32(q + 8, copy[i].message);
        wr32(q + 12, copy[i].when);
        wr32(q + 16, copy[i].where);
        wr16(q + 20, copy[i].modifiers);
        if (prev)
            wr32(prev, q);
        prev = q;
    }
    wr16(LM_EventQueue, 0); /* qFlags */
    wr32(LM_EventQueue + 2, n ? buf : 0);
    wr32(LM_EventQueue + 6, prev);
}

void events_sync_lowmem(void)
{
    mirror_queue();
    pthread_mutex_lock(&lock);
    uint32_t pt = make_pt(mouse_v, mouse_h);
    bool down = button_down;
    uint8_t keys[16];
    memcpy(keys, keymap, sizeof keys);
    bool key_changed = key_new, button_changed = button_new;
    uint16_t last = key_last;
    key_new = button_new = false;
    pthread_mutex_unlock(&lock);
    gput(LM_KeyMap, keys, sizeof keys); /* games read it in place of GetKeys */
    if (key_changed) {
        wr16(LM_KeyLast, last);
        wr16(LM_HiKeyLast, last);
        wr32(LM_KeyTime, rd32(LM_Ticks));
        wr32(LM_KeyRepTime, rd32(LM_Ticks));
    }
    if (button_changed)
        wr32(LM_MBTicks, rd32(LM_Ticks));
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
        if (button_down)
            presses++;
        button_new = true;
        enqueue(button_down ? mouseDown : mouseUp, 0);
    }
    pthread_cond_broadcast(&arrived);
    pthread_mutex_unlock(&lock);
}

/* The host stopped seeing the keyboard (it went to the background): keys
 * held then are released, or the program sees them held from then on. */
void machine_release_keys(void)
{
    pthread_mutex_lock(&lock);
    memset(keymap, 0, sizeof keymap);
    key_mods = 0;
    pthread_mutex_unlock(&lock);
}

void machine_post_key(int charcode, int keycode, unsigned modifiers, int down, int repeat)
{
    pthread_mutex_lock(&lock);
    if (down && !repeat && charcode > 0)
        key_presses++;
    if (down) {
        key_last = (uint16_t)((keycode & 0xFF) << 8 | (charcode & 0xFF));
        key_new = true;
    }
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

uint32_t ev_press_count(void)
{
    pthread_mutex_lock(&lock);
    uint32_t n = presses;
    pthread_mutex_unlock(&lock);
    return n;
}

uint32_t ev_key_count(void)
{
    pthread_mutex_lock(&lock);
    uint32_t n = key_presses;
    pthread_mutex_unlock(&lock);
    return n;
}

void machine_idle(void);
void machine_pass_tick(void);

bool ev_still_down(void)
{
    if (!host_attached && g_cpu) {
        if (g_cpu->halted) /* the instruction limit ended the run mid-drag */
            return false;
        events_script_pump(g_cpu->icount); /* scripted input due by now */
    }
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
    if (!host_attached) {
        /* Headless, a tick of the instruction clock passes, so scripted
         * input arrives: a Toolbox tracking loop runs no guest code. */
        machine_pass_tick();
        return;
    }
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
static void note_poll(void);

static bool next_event(int16_t mask, gaddr evt, bool remove, bool os_only)
{
    note_poll();
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
                    qversion++;
                }
                break;
            }
        }
    }
    pthread_mutex_unlock(&lock);
    mirror_queue();
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
    speech_pump();
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
    speech_pump();
    time_pump();
    notify_pump();
    display_sync();
    if (next_event(mask, evp, true, false)) {
        LOGF(LOG_TRAPS, "  event %d message %08X\n", (int16_t)rd16(evp), rd32(evp + 2));
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
    qversion++;
    pthread_mutex_unlock(&lock);
    mirror_queue();
}

int16_t tb_PostEvent(int16_t evcode, int32_t evmsg)
{
    pthread_mutex_lock(&lock);
    enqueue(evcode, (uint32_t)evmsg);
    pthread_mutex_unlock(&lock);
    mirror_queue();
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
                qversion++;
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

/* ---------------------------------------------------------------------- */
/* Stall log (BLUEBOX_STALL_LOG=file, with a host): when the program has not
 * asked for events or the keyboard for half a second, what the emulator is
 * doing is written every quarter second until it does again (the PC, the
 * trap running, whether in an interrupt task, the instruction count). */

static volatile uint64_t last_poll_ns;
static FILE *stall_log;

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static void note_poll(void) { last_poll_ns = mono_ns(); }

bool time_in_interrupt(void);
const char *trap_name(uint16_t tw);

static void *stall_watch(void *arg)
{
    bool stalled = false;
    uint64_t since = 0;
    for (;;) {
        usleep(250000);
        uint64_t now = mono_ns(), last = last_poll_ns;
        if (!last || !g_cpu)
            continue;
        if (now - last > 500000000u) {
            if (!stalled) {
                stalled = true;
                since = last;
                fprintf(stall_log, "stall: no event or key poll for 0.5 s\n");
            }
            uint16_t tw = trap_current_word();
            fprintf(stall_log, "  +%.2fs pc %08X trap %s%s icount %llu\n", (double)(now - since) / 1e9, g_cpu->pc,
                    tw ? trap_name(tw) : "-", time_in_interrupt() ? " (in an interrupt task)" : "",
                    (unsigned long long)g_cpu->icount);
            fflush(stall_log);
        } else if (stalled) {
            stalled = false;
            fprintf(stall_log, "stall over after %.2fs\n", (double)(last - since) / 1e9);
            fflush(stall_log);
        }
    }
    return arg;
}

void events_start_stall_log(void)
{
    const char *path = getenv("BLUEBOX_STALL_LOG");
    if (!path || stall_log || !(stall_log = fopen(path, "w")))
        return;
    pthread_t t;
    pthread_create(&t, NULL, stall_watch, NULL);
    pthread_detach(t);
}

void tb_GetKeys(gaddr keys)
{
    note_poll();
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
void tb_SystemClick(gaddr evp, gaddr wp)
{
    /* A click on the desktop goes to the desk hook: D0 -1, A0 the event. */
    if (!wp && rd32(LM_DeskHook))
        hook_call(rd32(LM_DeskHook), 0xFFFFFFFFu, evp, g_cpu->a[7]);
}
int8_t tb_SystemEvent(gaddr evp) { return 0; }

void events_reset(void)
{
    gaddr buf = mm_new_ptr_in(rd32(LM_SysZone), QSIZE * EVQEL, true);
    wr32(LM_SysEvtBuf, buf);
    wr16(LM_SysEvtCnt, QSIZE - 1);
    mirrored = (uint32_t)-1;
    pthread_mutex_lock(&lock);
    qhead = qtail = 0;
    qversion++;
    button_down = false;
    key_mods = 0;
    memset(keymap, 0, sizeof keymap);
    key_last = 0;
    key_new = button_new = false;
    mouse_h = 100;
    mouse_v = 100;
    pthread_mutex_unlock(&lock);
}
