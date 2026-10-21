/*
 * What the host needs to present rootless windows and native menus.
 *
 * The emulator thread publishes snapshots (window list with shape masks,
 * and the menu bar) under a mutex at safe points; the host thread reads
 * them.  The host never walks guest structures itself.
 */
#include <pthread.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "qd_internal.h"
#include "tb_internal.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------------------------------------------------------------------- */
/* Windows */

typedef struct {
    uint32_t id;
    qrect bounds;
    uint8_t *mask; /* one byte per pixel of bounds: 1 inside the structure region */
    bool active;
    uint8_t flags;
    char title[256];
} win_snap;

uint8_t wm_window_flags(gaddr w);

static win_snap wins[64];
static int nwins;
static uint32_t win_version;
static bool wins_dirty = true;

void display_windows_changed(void) { wins_dirty = true; }

static void publish_windows(void);

/* After a Toolbox trap called from the program itself, also a safe point:
 * a program that shows a window and then draws and waits without asking
 * for events (a splash screen polling TickCount, Apeiron's) is seen. */
void display_trap_done(void)
{
    if (wins_dirty) {
        wins_dirty = false;
        publish_windows();
    }
}

static void publish_windows(void)
{
    win_snap fresh[64];
    int n = 0;
    for (gaddr w = rd32(LM_WindowList); w && n < 64; w = WindowRecord_nextWindow(w)) {
        if (!WindowRecord_visible(w))
            continue;
        hrgn s;
        hrgn_init(&s);
        rgn_read(WindowRecord_strucRgn(w), &s);
        qrect b = hrgn_bbox(&s);
        int bw = b.right - b.left, bh = b.bottom - b.top;
        if (bw <= 0 || bh <= 0) {
            hrgn_free(&s);
            continue;
        }
        uint8_t *mask = calloc((size_t)bw * (size_t)bh, 1);
        for (int i = 0; i < s.nbands; i++) {
            const rband *band = &s.bands[i];
            for (int y = band->top; y < band->bottom; y++)
                for (int k = 0; k < band->n; k += 2)
                    memset(mask + (size_t)(y - b.top) * (size_t)bw + (size_t)(band->x[k] - b.left), 1,
                           (size_t)(band->x[k + 1] - band->x[k]));
        }
        hrgn_free(&s);
        fresh[n] = (win_snap){ w, b, mask, WindowRecord_hilited(w) != 0, wm_window_flags(w), { 0 } };
        gaddr th = WindowRecord_titleHandle(w);
        if (th && rd32(th)) {
            uint32_t tl = rd8(rd32(th));
            gget(fresh[n].title, rd32(th) + 1, tl);
            fresh[n].title[tl] = 0;
        }
        n++;
    }
    pthread_mutex_lock(&lock);
    for (int i = 0; i < nwins; i++)
        free(wins[i].mask);
    memcpy(wins, fresh, (size_t)n * sizeof *fresh);
    nwins = n;
    win_version++;
    pthread_mutex_unlock(&lock);
}

uint32_t machine_windows_version(void)
{
    pthread_mutex_lock(&lock);
    uint32_t v = win_version;
    pthread_mutex_unlock(&lock);
    return v;
}

/* Front-to-back list of visible windows; returns the count. */
int machine_windows(machine_window *out, int max)
{
    pthread_mutex_lock(&lock);
    int n = nwins < max ? nwins : max;
    for (int i = 0; i < n; i++)
    {
        out[i] = (machine_window){ wins[i].id, wins[i].bounds.top, wins[i].bounds.left, wins[i].bounds.bottom,
                                   wins[i].bounds.right, wins[i].active, wins[i].flags, { 0 } };
        memcpy(out[i].title, wins[i].title, sizeof out[i].title);
    }
    pthread_mutex_unlock(&lock);
    return n;
}

/* Copy a window's shape mask (width*height bytes); false if it is gone. */
bool machine_window_mask(uint32_t id, uint8_t *out, int width, int height)
{
    pthread_mutex_lock(&lock);
    bool ok = false;
    for (int i = 0; i < nwins; i++) {
        if (wins[i].id != id)
            continue;
        int bw = wins[i].bounds.right - wins[i].bounds.left, bh = wins[i].bounds.bottom - wins[i].bounds.top;
        if (bw == width && bh == height) {
            memcpy(out, wins[i].mask, (size_t)bw * (size_t)bh);
            ok = true;
        }
        break;
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

/* ---------------------------------------------------------------------- */
/* Menus */

static machine_menu menus[32];
static int nmenus_snap;
static uint32_t menu_version;
static bool menus_dirty = true;

void display_menus_changed(void) { menus_dirty = true; }

static void copy_pstr(char *dst, size_t n, gaddr s)
{
    size_t len = rd8(s);
    if (len > n - 1)
        len = n - 1;
    gget(dst, s + 1, (uint32_t)len);
    dst[len] = 0;
}

static void publish_menus(void)
{
    gaddr l = rd32(LM_MenuList);
    machine_menu fresh[32];
    int n = 0;
    if (l && rd32(l)) {
        int count = (int16_t)rd16(rd32(l)) / 6;
        for (int i = 0; i < count && n < 32; i++) {
            gaddr mh = rd32(rd32(l) + 6 + (uint32_t)i * 6);
            if (!mh || !rd32(mh))
                continue;
            gaddr p = rd32(mh);
            machine_menu *m = &fresh[n++];
            memset(m, 0, sizeof *m);
            m->id = MenuInfo_menuID(p);
            uint32_t flags = (uint32_t)MenuInfo_enableFlags(p);
            m->enabled = flags & 1;
            gaddr t = p + MenuInfo_menuData_off;
            copy_pstr(m->title, sizeof m->title, t);
            gaddr it = t + rd8(t) + 1;
            int k = 0;
            while (rd8(it) && k < 64) {
                machine_menu_item *mi = &m->items[k];
                int nl = rd8(it);
                copy_pstr(mi->text, sizeof mi->text, it);
                mi->separator = nl > 0 && rd8(it + 1) == '-';
                mi->key = rd8(it + 1 + nl + 1);
                mi->mark = rd8(it + 1 + nl + 2);
                mi->style = rd8(it + 1 + nl + 3);
                mi->modifiers = menu_item_modifiers(mh, k + 1);
                mi->enabled = (flags & 1) && ((k + 1) > 31 || (flags & (1u << (k + 1))));
                k++;
                it += (uint32_t)nl + 5;
            }
            m->nitems = k;
        }
    }
    pthread_mutex_lock(&lock);
    memcpy(menus, fresh, (size_t)n * sizeof *fresh);
    nmenus_snap = n;
    menu_version++;
    pthread_mutex_unlock(&lock);
}

uint32_t machine_menus_version(void)
{
    pthread_mutex_lock(&lock);
    uint32_t v = menu_version;
    pthread_mutex_unlock(&lock);
    return v;
}

int machine_menus(machine_menu *out, int max)
{
    pthread_mutex_lock(&lock);
    int n = nmenus_snap < max ? nmenus_snap : max;
    memcpy(out, menus, (size_t)n * sizeof *out);
    pthread_mutex_unlock(&lock);
    return n;
}

/* The host's menu bar.  A choice is delivered through MenuSelect, after the
 * app has adjusted its menus.  While a host menu is open (tracking), the
 * app is given the menu-bar click right away, so it adjusts its menus
 * before they are shown, and MenuSelect waits for the host menu to close. */
static int32_t pending_choice;
/* Clicks posted for a pending choice, and when the last one was. */
static int choice_clicks;
static uint32_t choice_click_ticks;
static pthread_cond_t track_cond = PTHREAD_COND_INITIALIZER;
static bool tracking, in_select, track_done;

void ev_post_menubar_click(void);

void machine_choose_menu(int16_t menu_id, int16_t item)
{
    pthread_mutex_lock(&lock);
    pending_choice = ((int32_t)menu_id << 16) | (uint16_t)item;
    bool waiting = tracking;
    if (waiting) {
        track_done = true;
        pthread_cond_broadcast(&track_cond);
    }
    if (!waiting) {
        choice_clicks = 1;
        choice_click_ticks = rd32(LM_Ticks);
    }
    pthread_mutex_unlock(&lock);
    /* Otherwise the app sees a click in its menu bar and calls MenuSelect. */
    if (!waiting)
        ev_post_menubar_click();
}

static void deadline_after_ms(struct timespec *ts, long ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_nsec += ms * 1000000L;
    ts->tv_sec += ts->tv_nsec / 1000000000L;
    ts->tv_nsec %= 1000000000L;
}

/* One tracking session lasts while any host menu is open (moving from menu
 * to menu does not start a new one). */
void machine_menu_tracking_begin(void)
{
    pthread_mutex_lock(&lock);
    bool start = !tracking;
    if (start) {
        tracking = true;
        in_select = false;
        track_done = false;
        pending_choice = 0;
    }
    pthread_mutex_unlock(&lock);
    if (start)
        ev_post_menubar_click();
    /* Give the app a moment to adjust its menus; it may be busy. */
    struct timespec ts;
    deadline_after_ms(&ts, 150);
    pthread_mutex_lock(&lock);
    while (tracking && !in_select)
        if (pthread_cond_timedwait(&track_cond, &lock, &ts))
            break;
    pthread_mutex_unlock(&lock);
}

void machine_menu_tracking_end(void)
{
    pthread_mutex_lock(&lock);
    if (in_select) {
        track_done = true;
        pthread_cond_broadcast(&track_cond);
    } else {
        tracking = false; /* the app has not got there yet: any choice stays pending */
    }
    pthread_mutex_unlock(&lock);
}

int32_t display_take_menu_choice(void)
{
    pthread_mutex_lock(&lock);
    int32_t c = pending_choice;
    pending_choice = 0;
    choice_clicks = 0;
    pthread_mutex_unlock(&lock);
    return c;
}

/* For MenuSelect: if a host menu is open, publish the menus as adjusted and
 * wait for it to close.  Returns false if no host menu is open. */
bool display_menu_tracking_wait(void)
{
    pthread_mutex_lock(&lock);
    bool active = tracking;
    pthread_mutex_unlock(&lock);
    if (!active)
        return false;
    menus_dirty = false;
    publish_menus();
    pthread_mutex_lock(&lock);
    in_select = true;
    pthread_cond_broadcast(&track_cond);
    /* While the menu is down, interrupt-time work goes on in 5 ms slices,
     * as on a Mac: sound refills (SoundTrecker's double buffers), Time
     * Manager and VBL tasks, the network.  A minute with no answer ends it. */
    struct timespec give_up;
    deadline_after_ms(&give_up, 60000);
    while (!track_done) {
        struct timespec ts;
        deadline_after_ms(&ts, 5);
        if (pthread_cond_timedwait(&track_cond, &lock, &ts) == 0 || track_done)
            continue;
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > give_up.tv_sec || (now.tv_sec == give_up.tv_sec && now.tv_nsec >= give_up.tv_nsec))
            break;
        pthread_mutex_unlock(&lock);
        machine_advance_ticks();
        sound_pump();
        time_pump();
        pthread_mutex_lock(&lock);
    }
    tracking = in_select = track_done = false;
    pthread_mutex_unlock(&lock);
    return true;
}

/* ---------------------------------------------------------------------- */
/* The whole screen: a program that wants it (draws on the screen outside
 * its windows itself, hides the menu bar, changes the screen's depth,
 * uses DrawSprocket) is shown in one window of the whole Mac screen
 * instead of rootless windows.  The first reason is kept; the host asks. */

uint32_t qd_screen_row_bytes(void);
int qd_screen_depth(void);

static const char *_Atomic screen_reason;
static bool direct_written;
static int dw_top, dw_left, dw_bottom, dw_right; /* pixels written since the last sync */

void display_want_screen(const char *why)
{
    if (!screen_reason) {
        screen_reason = why;
        LOGF(LOG_UNIMPL, "screen mode: %s\n", why);
    }
}

const char *machine_screen_mode_wanted(void) { return screen_reason; }

/* A store the program made into the screen (mem.h's watch). */
static void screen_written(gaddr a)
{
    uint32_t off = a - g_watch_lo, rb = qd_screen_row_bytes();
    int y = (int)(off / rb), x = (int)((off % rb) * 8 / (uint32_t)qd_screen_depth());
    if (!direct_written) {
        direct_written = true;
        dw_top = dw_bottom = y;
        dw_left = dw_right = x;
        return;
    }
    if (y < dw_top) dw_top = y;
    if (y > dw_bottom) dw_bottom = y;
    if (x < dw_left) dw_left = x;
    if (x > dw_right) dw_right = x;
}

/* Whether one of the program's windows covers what it wrote. */
static bool writes_in_a_window(void)
{
    for (gaddr w = rd32(LM_WindowList); w; w = WindowRecord_nextWindow(w)) {
        gaddr rgn = WindowRecord_strucRgn(w);
        if (!WindowRecord_visible(w) || !rgn || !rd32(rgn))
            continue;
        qrect b = qrect_get(rd32(rgn) + 2);
        if (dw_top >= b.top && dw_bottom < b.bottom && dw_left >= b.left && dw_right < b.right)
            return true;
    }
    return false;
}

void display_note_screen_draw(qrect r)
{
    if (screen_reason)
        return;
    gaddr port = qd_port();
    if (port && (port == rd32(LM_WMgrPort) || port == rd32(LM_WMgrCPort)))
        return; /* the Window Manager's own drawing: the desktop, outlines */
    int w, h;
    machine_screen_size(&w, &h);
    if (r.top < 0) r.top = 0;
    if (r.left < 0) r.left = 0;
    if (r.bottom > h) r.bottom = (int16_t)h;
    if (r.right > w) r.right = (int16_t)w;
    if (r.bottom <= r.top || r.right <= r.left)
        return;
    if ((int)(r.bottom - r.top) * (r.right - r.left) < w * h / 4)
        return; /* small: a zoom rectangle, a cursor's save area */
    int st = dw_top, sl = dw_left, sb = dw_bottom, sr = dw_right;
    dw_top = r.top, dw_left = r.left, dw_bottom = r.bottom - 1, dw_right = r.right - 1;
    bool inside = writes_in_a_window();
    dw_top = st, dw_left = sl, dw_bottom = sb, dw_right = sr;
    if (!inside)
        display_want_screen("it draws on the screen outside its windows");
}

static void check_screen_mode(void)
{
    if (!screen_reason) {
        if ((int16_t)rd16(LM_MBarHeight) == 0)
            display_want_screen("it hides the menu bar");
        else if (direct_written && !writes_in_a_window())
            display_want_screen("it draws on the screen outside its windows");
    }
    direct_written = false;
    int w, h;
    machine_screen_size(&w, &h);
    g_watch_fn = screen_written;
    g_watch_lo = rd32(LM_ScrnBase);
    g_watch_len = screen_reason ? 0 : qd_screen_row_bytes() * (uint32_t)h; /* decided: no more watching */
}

/* Called by the Event Manager on the emulator thread: a safe point. */
void display_sync(void)
{
    check_screen_mode();

    /* A choice still waiting for MenuSelect: the click may only have shown
     * a hidden menu bar (Prince of Persia's), as a first click does on a
     * Mac; click again, a few times at most. */
    pthread_mutex_lock(&lock);
    bool again = pending_choice && !tracking && choice_clicks > 0 && choice_clicks < 4 &&
                 rd32(LM_Ticks) - choice_click_ticks >= 10;
    if (again) {
        choice_clicks++;
        choice_click_ticks = rd32(LM_Ticks);
    } else if (pending_choice && !tracking && choice_clicks >= 4 && rd32(LM_Ticks) - choice_click_ticks >= 10) {
        pending_choice = 0; /* nobody took it */
        choice_clicks = 0;
    }
    pthread_mutex_unlock(&lock);
    if (again)
        ev_post_menubar_click();

    if (wins_dirty) {
        wins_dirty = false;
        publish_windows();
    }
    if (menus_dirty) {
        menus_dirty = false;
        publish_menus();
    }
}

void display_reset(void)
{
    screen_reason = NULL;
    direct_written = false;
    g_watch_len = 0;
    wins_dirty = menus_dirty = true;
    pending_choice = 0;
    tracking = in_select = track_done = false;
}

/* ---------------------------------------------------------------------- */
/* Transient drawing outside the windows: a pop-up menu.  Rootless hosts
 * show the area above everything while it is up, and take every click. */

static pthread_mutex_t transient_lock = PTHREAD_MUTEX_INITIALIZER;
static int16_t transient[4];
static bool transient_on;
static uint32_t transient_version;

void display_set_transient(bool on, int16_t top, int16_t left, int16_t bottom, int16_t right)
{
    pthread_mutex_lock(&transient_lock);
    transient_on = on;
    transient[0] = top;
    transient[1] = left;
    transient[2] = bottom;
    transient[3] = right;
    transient_version++;
    pthread_mutex_unlock(&transient_lock);
}

uint32_t machine_transient(int16_t rect[4], bool *on)
{
    pthread_mutex_lock(&transient_lock);
    memcpy(rect, transient, sizeof transient);
    *on = transient_on;
    uint32_t v = transient_version;
    pthread_mutex_unlock(&transient_lock);
    return v;
}
