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
#include "trap.h"

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
/* Moving the cursor themselves (games recentring it to read the mouse's
 * motion): the position last written to low memory, the program's moves
 * counted, and when it last moved it. */
static uint32_t written_pt;
/* Since boot: the first sync writes the mouse without looking for a move;
 * waiting, caught between a program's cursor writes and its CrsrNew. */
static bool synced, waiting;
static uint32_t warps;
static uint64_t last_warp_ns;
static double frac_h, frac_v; /* captured motion's fractions of a pixel */
/* The mouse's own motion not yet reported to an ADB service routine:
 * from the host, never from the program moving the cursor (a program
 * reading ADB packets and recentring the cursor, as Apeiron does, would
 * otherwise see each recentring as the mouse moving back). */
static int motion_h, motion_v;
/* All of the mouse's own motion since boot, for InputSprocket's deltas,
 * and when a program last read it (the host then captures the mouse). */
static int32_t total_h, total_v;
static uint64_t last_relative_ns;
#define LM_CrsrNew 0x08CEu    /* Byte: nonzero when the cursor must be redrawn at RawMouse */
static bool button_down;
static uint32_t presses, key_presses; /* counted, so a quick click is never missed */
static uint16_t key_mods;
static uint8_t keymap[16];
/* Keys pressed since the program last looked at the keyboard (GetKeys or
 * KeyMap): a tap let go of before it looks still shows once, as the
 * key-down event it gets for it says it should. */
static uint8_t keys_tapped[16];

/* The keyboard as the program sees it: what is down, and what was tapped
 * since it last looked; then forget the taps.  The lock is held. */
static void keys_seen(uint8_t out[16])
{
    for (int i = 0; i < 16; i++) {
        out[i] = keymap[i] | keys_tapped[i];
        keys_tapped[i] = 0;
    }
}
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
    int kind; /* 0 key, 1 mouse, 2 menu, 3 key down, 4 key up, 5 screenshot (a: its path's index) */
    int a, b, c;
} script_step;

static script_step *script;
static int nscript, script_next;

void machine_script_add(uint64_t at, int kind, int a, int b, int c)
{
    script = realloc(script, (size_t)(nscript + 1) * sizeof *script);
    script[nscript++] = (script_step){ at, kind, a, b, c };
}

static char **snap_paths;
static int nsnap_paths;

void machine_script_add_snap(uint64_t at, const char *path)
{
    snap_paths = realloc(snap_paths, (size_t)(nsnap_paths + 1) * sizeof *snap_paths);
    snap_paths[nsnap_paths] = strdup(path);
    machine_script_add(at, 5, nsnap_paths++, 0, 0);
}

/* The next step not yet applied: its instruction count, or 0. */
uint64_t events_script_next_at(void) { return script_next < nscript ? script[script_next].at : 0; }

void machine_choose_menu(int16_t menu_id, int16_t item);

void machine_advance_ticks(void);

void events_script_pump(uint64_t now)
{
    if (script_next < nscript && script[script_next].at <= now)
        machine_advance_ticks(); /* the events' time is now, not when Ticks last moved */
    while (script_next < nscript && script[script_next].at <= now) {
        script_step s = script[script_next++];
        switch (s.kind) {
        case 0: /* a: character, b: modifiers, c: key code */
            machine_post_key(s.a, s.c, (unsigned)s.b, 1, 0);
            machine_post_key(s.a, s.c, (unsigned)s.b, 0, 0);
            pthread_mutex_lock(&lock);
            key_mods = 0; /* and lets go of Command (cmd-x) */
            pthread_mutex_unlock(&lock);
            break;
        case 3: machine_post_key(s.a, s.c, (unsigned)s.b, 1, 0); break; /* held down */
        case 4: machine_post_key(s.a, s.c, (unsigned)s.b, 0, 0); break; /* released */
        case 1: machine_post_mouse(s.a, s.b, s.c); break;
        case 2: machine_choose_menu((int16_t)s.a, (int16_t)s.b); break;
        case 5: machine_screenshot(snap_paths[s.a]); break;
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
    /* A program that moved the cursor: a new position in MTemp or
     * RawMouse, and CrsrNew set (as the cursor VBL task then drew it
     * there).  The mouse is there now, and the host's pointer follows. */
    uint32_t mtemp = rd32(LM_MTemp), raw = rd32(LM_MouseLocation);
    bool changed = synced && (mtemp != written_pt || raw != written_pt);
    synced = true;
    /* Caught between its writes and its CrsrNew: the mouse is left alone
     * until the next sync (once only, in case it never sets it); the keys
     * and the button are brought up to date all the same. */
    bool hold_mouse = changed && !rd8(LM_CrsrNew) && !waiting;
    waiting = hold_mouse;
    bool moved = changed && !hold_mouse;
    if (!hold_mouse && rd8(LM_CrsrNew))
        wr8(LM_CrsrNew, 0);
    pthread_mutex_lock(&lock);
    if (moved) {
        uint32_t to = raw != written_pt ? raw : mtemp;
        mouse_v = (int16_t)(to >> 16);
        mouse_h = (int16_t)to;
        frac_h = frac_v = 0;
        warps++;
        last_warp_ns = machine_time_ns();
        LOGF(LOG_TRAPS, "  the program moved the cursor to %d,%d\n", mouse_h, mouse_v);
    }
    uint32_t pt = make_pt(mouse_v, mouse_h);
    if (!hold_mouse)
        written_pt = pt;
    bool down = button_down;
    uint8_t keys[16];
    keys_seen(keys);
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
    if (hold_mouse)
        return;
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
    motion_h += x - mouse_h;
    motion_v += y - mouse_v;
    total_h += x - mouse_h;
    total_v += y - mouse_v;
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

/* Captured mouse motion, in Mac pixels: added to wherever the mouse is
 * now, the program's own moves included, and kept on the screen. */
void machine_post_mouse_delta(double dx, double dy)
{
    int w = 0, h = 0;
    machine_screen_size(&w, &h);
    pthread_mutex_lock(&lock);
    frac_h += dx;
    frac_v += dy;
    int ih = (int)frac_h, iv = (int)frac_v;
    frac_h -= ih;
    frac_v -= iv;
    motion_h += ih; /* all of it, at the screen's edge too, as a mouse reports */
    motion_v += iv;
    total_h += ih;
    total_v += iv;
    int nh = mouse_h + ih, nv = mouse_v + iv;
    nh = nh < 0 ? 0 : nh >= w ? w - 1 : nh;
    nv = nv < 0 ? 0 : nv >= h ? h - 1 : nv;
    mouse_h = (int16_t)nh;
    mouse_v = (int16_t)nv;
    pthread_cond_broadcast(&arrived);
    pthread_mutex_unlock(&lock);
}

/* Up to `limit` of the mouse's unreported motion each way (the rest stays
 * for the next packet); limit 0 takes it all.  False if there is none. */
bool ev_take_motion(int *dh, int *dv, int limit)
{
    pthread_mutex_lock(&lock);
    int h = motion_h, v = motion_v;
    if (limit) {
        h = h < -limit ? -limit : h > limit - 1 ? limit - 1 : h;
        v = v < -limit ? -limit : v > limit - 1 ? limit - 1 : v;
    }
    motion_h -= h;
    motion_v -= v;
    pthread_mutex_unlock(&lock);
    *dh = h;
    *dv = v;
    return h || v;
}

bool ev_motion_pending(void)
{
    pthread_mutex_lock(&lock);
    bool p = motion_h || motion_v;
    pthread_mutex_unlock(&lock);
    return p;
}

/* Where the mouse is, how many times the program has moved it, and
 * whether it is reading the mouse by moving it (it did in the last two
 * seconds): the host then gives it the mouse's motion. */
uint32_t machine_mouse_state(int *x, int *y, bool *relative)
{
    pthread_mutex_lock(&lock);
    *x = mouse_h;
    *y = mouse_v;
    uint32_t n = warps;
    uint64_t now = machine_time_ns();
    *relative = (warps && now - last_warp_ns < 2000000000ull) ||
                (last_relative_ns && now - last_relative_ns < 2000000000ull);
    pthread_mutex_unlock(&lock);
    return n;
}

/* The mouse's own motion since boot and its button, read as a mouse
 * (InputSprocket): the cursor and low memory play no part.  A program
 * reading the motion (capture) has the host capture the mouse for a
 * while, so the motion never stops at the screen's edge. */
void ev_mouse_raw(int32_t *h, int32_t *v, bool *down, bool capture)
{
    if (!host_attached && g_cpu && !g_cpu->halted)
        events_script_pump(g_cpu->icount);
    pthread_mutex_lock(&lock);
    *h = total_h;
    *v = total_v;
    *down = button_down;
    if (capture)
        last_relative_ns = machine_time_ns();
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
        if (down) {
            keymap[keycode / 8] |= (uint8_t)(1u << (keycode % 8));
            keys_tapped[keycode / 8] |= (uint8_t)(1u << (keycode % 8));
        }
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

/* The button itself, as Button reports it (MBState). */
static bool button_now(void)
{
    if (!host_attached && g_cpu) {
        if (g_cpu->halted)
            return false;
        events_script_pump(g_cpu->icount);
    }
    pthread_mutex_lock(&lock);
    bool d = button_down;
    pthread_mutex_unlock(&lock);
    return d;
}

bool ev_still_down(void)
{
    if (!host_attached && g_cpu) {
        if (g_cpu->halted) /* the instruction limit ended the run mid-drag */
            return false;
        events_script_pump(g_cpu->icount); /* scripted input due by now */
    }
    /* The button is down and no mouse event waits: a press already let go
     * of and pressed again (a double click) ends the first one's tracking. */
    pthread_mutex_lock(&lock);
    bool d = button_down;
    for (int i = qhead; d && i != qtail; i = (i + 1) % QSIZE)
        if (queue[i].what == mouseDown || queue[i].what == mouseUp)
            d = false;
    pthread_mutex_unlock(&lock);
    return d;
}

/* The modifier keys and button now, as an event's modifiers field has them. */
uint16_t ev_modifiers(void)
{
    pthread_mutex_lock(&lock);
    uint16_t m = (uint16_t)(key_mods | (button_down ? 0 : btnState));
    pthread_mutex_unlock(&lock);
    return m;
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
/* A tick in a Toolbox tracking loop (menus, controls, dragging): the
 * interrupt-time work that goes on under it on a Mac goes on here too,
 * sound refills among it (SoundTrecker plays on while a menu is down). */
void ev_wait_tick(void)
{
    machine_advance_ticks();
    sound_pump();
    time_pump();
    if (!host_attached) {
        /* Headless, a tick of the instruction clock passes, so scripted
         * input arrives. */
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
    opentransport_poll(); /* network events to the programs' notifiers */
    mactcp_poll();
    serial_poll();
    ctb_poll();
    fm_flush_dirty();
    as_idle();
    appletalk_poll();
    if (!os_only)
        drag_host_pump();
    appearance_poll(); /* the host's highlight and accent colours */
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
    /* A key event handed to the program: KeyMap shows that key now, not
     * at the next emulation slice (Maelstrom reads it to see which key a
     * key-down was for). */
    uint8_t keys[16];
    bool key_event = found && remove && (e.what == keyDown || e.what == autoKey || e.what == keyUp);
    if (key_event)
        keys_seen(keys);
    pthread_mutex_unlock(&lock);
    if (key_event)
        gput(LM_KeyMap, keys, sizeof keys);
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

static bool gne_idled; /* GetNextEvent let time pass for a null event */

int8_t tb_GetNextEvent(int16_t em, gaddr evt)
{
    sound_pump();
    speech_pump();
    time_pump();
    notify_pump();
    machine_advance_ticks();
    display_sync();
    if (next_event(em, evt, true, false)) {
        LOGF(LOG_TRAPS, "  event %d message %08X when %u where %08X modifiers %04X\n", (int16_t)rd16(evt), rd32(evt + 2), rd32(evt + 6), rd32(evt + 10), rd16(evt + 14));
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
    gne_idled = true;
    return 0;
}

int8_t tb_EventAvail(int16_t em, gaddr evt)
{
    display_sync();
    return next_event(em, evt, false, false);
}

/* A program's patch on GetNextEvent, if it has one.  System 7's
 * WaitNextEvent took its events through GetNextEvent, so such a patch sees
 * them all: HyperCard 2.4's PowerPC code activates its card window there,
 * from CurActivate, and without it treats every click as one that brings
 * the window to the front. */
static bool in_gne_patch;

static gaddr gne_patch(void)
{
    gaddr a = rd32(TB_TRAP_TABLE + 0x170u * 4);
    return in_gne_patch || !a || native_index_for(a) >= 0 ? 0 : a;
}

/* The next event for WaitNextEvent: through the patch, or our own. */
static bool wne_event(int16_t mask, gaddr evp, bool *idled)
{
    gaddr patch = gne_patch();
    *idled = false;
    if (!patch)
        return next_event(mask, evp, true, false);
    cpu68k *c = g_cpu;
    cpu_push16(c, 0); /* the Boolean result */
    cpu_push16(c, (uint16_t)mask);
    cpu_push32(c, evp);
    gne_idled = false;
    in_gne_patch = true;
    guest_call(patch);
    in_gne_patch = false;
    bool got = (cpu_pop16(c) >> 8) != 0;
    *idled = gne_idled;
    return got;
}

int8_t tb_WaitNextEvent(int16_t mask, gaddr evp, int32_t sleep, gaddr mousergn)
{
    machine_advance_ticks();
    sound_pump();
    speech_pump();
    time_pump();
    notify_pump();
    display_sync();
    bool idled;
    if (wne_event(mask, evp, &idled)) {
        LOGF(LOG_TRAPS, "  event %d message %08X when %u where %08X modifiers %04X\n", (int16_t)rd16(evp), rd32(evp + 2), rd32(evp + 6), rd32(evp + 10), rd16(evp + 14));
        note_repeated_update(evp);
        return 1;
    }
    if (host_attached && sleep > 0) {
        /* Sleep until input arrives or the time is up.  Interrupt-time
         * work (sound refills, Time Manager and VBL tasks) goes on while
         * the program sleeps, as it did on a Mac: then the wait is cut into
         * slices with the work done between them. */
        uint64_t total_ns = (uint64_t)(sleep > 600 ? 600 : sleep) * 1000000000ULL / 60;
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);
        for (;;) {
            bool busy = sound_busy() || time_busy();
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            uint64_t spent = (uint64_t)(now.tv_sec - start.tv_sec) * 1000000000ULL + (uint64_t)(now.tv_nsec - start.tv_nsec);
            if (spent >= total_ns)
                break;
            uint64_t ns = total_ns - spent;
            if (busy && ns > 5000000)
                ns = 5000000; /* 5 ms */
            uint32_t c0, i0;
            bool hle = (mask & 0x0400) && aem_next_event(&c0, &i0, false);
            pthread_mutex_lock(&lock);
            bool waiting = qhead == qtail && !hle;
            if (waiting) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += (time_t)(ns / 1000000000ULL);
                ts.tv_nsec += (long)(ns % 1000000000ULL);
                if (ts.tv_nsec >= 1000000000) {
                    ts.tv_sec++;
                    ts.tv_nsec -= 1000000000;
                }
                pthread_cond_timedwait(&arrived, &lock, &ts);
                waiting = qhead == qtail;
            }
            pthread_mutex_unlock(&lock);
            if (!waiting || !busy)
                break;
            machine_advance_ticks();
            sound_pump();
            time_pump();
            if (g_cpu->halted)
                return 0;
        }
        machine_advance_ticks();
        return wne_event(mask, evp, &idled);
    }
    if (!host_attached && !idled)
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

/* PPostEvent: the same, answering the queue element (the queue's last). */
int16_t tb_PPostEvent(int16_t evcode, int32_t evmsg, gaddr qelp)
{
    int16_t err = tb_PostEvent(evcode, evmsg);
    if (qelp)
        wr32(qelp, rd32(LM_EventQueue + 6));
    return err;
}

void tb_SetEventMask(int16_t evmask) { wr16(LM_SysEvtMask, (uint16_t)evmask); }

int8_t tb_Button(void)
{
    /* Polling a released button is waiting for input, like a null event.
     * The button as it is, mouse events waiting or not (a program polling
     * Button never takes them from the queue). */
    bool down = button_now();
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
    uint8_t now[16];
    pthread_mutex_lock(&lock);
    keys_seen(now);
    pthread_mutex_unlock(&lock);
    gput(keys, now, 16);
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
    synced = waiting = false;
    pthread_mutex_lock(&lock);
    qhead = qtail = 0;
    qversion++;
    button_down = false;
    last_relative_ns = 0;
    key_mods = 0;
    memset(keymap, 0, sizeof keymap);
    key_last = 0;
    key_new = button_new = false;
    mouse_h = 100;
    mouse_v = 100;
    pthread_mutex_unlock(&lock);
}
