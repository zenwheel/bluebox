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
#include "ppc.h"
#include "qd_internal.h"
#include "hw68k.h"
#include "tb_internal.h"

BB_HOST static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

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

/* Resumed in another process: nothing to do until a host attaches
 * (display_host_attached_after_resume); the windows as last published come
 * back with the snapshot. */
void display_after_resume(void) {}

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
        uint8_t *mask = bb_calloc((size_t)bw * (size_t)bh, 1, BB_BYTES);
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
        bb_free(wins[i].mask);
    memcpy(wins, fresh, (size_t)n * sizeof *fresh);
    if (n < nwins) /* no copies left of the masks just freed */
        memset(wins + n, 0, (size_t)(nwins - n) * sizeof *wins);
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

bool machine_menu_bar_has_command_key(int ch)
{
    if (ch >= 'a' && ch <= 'z')
        ch -= 32;
    bool found = false;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < nmenus_snap && !found; i++)
        for (int k = 0; k < menus[i].nitems && !found; k++) {
            const machine_menu_item *it = &menus[i].items[k];
            int key = it->key >= 'a' && it->key <= 'z' ? it->key - 32 : it->key;
            /* below 0x20 a submenu or a mark, not a key; 8 "no Command" */
            found = key >= 0x20 && key == ch && !it->separator && !(it->modifiers & 8);
        }
    pthread_mutex_unlock(&lock);
    return found;
}

/* The host's menu bar.  A choice is delivered through MenuSelect, after the
 * app has adjusted its menus.  While a host menu is open (tracking), the
 * app is given the menu-bar click right away, so it adjusts its menus
 * before they are shown, and MenuSelect waits for the host menu to close. */
static int32_t pending_choice;
/* Clicks posted for a pending choice, and when the last one was. */
static int choice_clicks;
static uint32_t choice_click_ticks;
BB_HOST static pthread_cond_t track_cond = PTHREAD_COND_INITIALIZER;
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
        /* The app has not got there yet: any choice stays pending, and the
         * click is posted again while it waits.  MacMan's game loop never
         * takes events and flushes them when the game ends, which lost the
         * click, and the item chosen (Set Keys) never came. */
        tracking = false;
        if (pending_choice) {
            choice_clicks = 1;
            choice_click_ticks = rd32(LM_Ticks);
        }
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

/* ---------------------------------------------------------------------- */
/* The desktop as the Window Manager painted it, for a host that shows the
 * whole screen: while the program is in the background (a game that, sent
 * there, hides its play area and leaves the desktop and a small window of
 * its own, as Maelstrom does), what is still desktop is left out and the
 * host's desktop shows instead.  Each byte painted keeps the value painted;
 * a byte still holding it, outside every window, is desktop. */

BB_HOST static pthread_mutex_t desk_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *desk_val, *desk_have; /* per byte of the screen */
static uint32_t desk_size, desk_rb;
static gaddr desk_base;

void display_desk_painted(const hrgn *area)
{
    gaddr base = rd32(LM_ScrnBase);
    uint32_t rb = qd_screen_row_bytes();
    int w, h, depth = qd_screen_depth();
    machine_screen_size(&w, &h);
    uint32_t size = rb * (uint32_t)h;
    pthread_mutex_lock(&desk_lock);
    if (size != desk_size || base != desk_base || rb != desk_rb) {
        bb_free(desk_val);
        bb_free(desk_have);
        desk_val = bb_malloc(size, BB_BYTES);
        desk_have = bb_calloc(size, 1, BB_BYTES);
        desk_size = desk_val && desk_have ? size : 0;
        desk_base = base;
        desk_rb = rb;
    }
    qrect b = hrgn_bbox(area);
    for (int y = b.top > 0 ? b.top : 0; desk_size && y < b.bottom && y < h; y++) {
        int n;
        const int16_t *runs = hrgn_row(area, y, &n);
        for (int i = 0; i + 1 < n; i += 2) {
            int x0 = runs[i] > 0 ? runs[i] : 0, x1 = runs[i + 1] < w ? runs[i + 1] : w;
            if (x1 <= x0)
                continue;
            uint32_t k0 = (uint32_t)y * rb + (uint32_t)(x0 * depth / 8);
            uint32_t k1 = (uint32_t)y * rb + (uint32_t)((x1 * depth + 7) / 8);
            gget(desk_val + k0, base + k0, k1 - k0);
            memset(desk_have + k0, 1, k1 - k0);
        }
    }
    pthread_mutex_unlock(&desk_lock);
}

/* What of the screen is not desktop (width*height bytes, 1 to show), for
 * the area from left, top.  False if the desktop was never painted. */
bool machine_desktop_mask(int left, int top, int width, int height, uint8_t *out)
{
    gaddr base = rd32(LM_ScrnBase);
    int depth = qd_screen_depth();
    memset(out, 1, (size_t)width * (size_t)height);
    pthread_mutex_lock(&desk_lock);
    bool ok = desk_size && base == desk_base && desk_rb == qd_screen_row_bytes();
    for (int r = 0; ok && r < height; r++)
        for (int c = 0; c < width; c++) {
            int x = left + c, y = top + r;
            uint32_t k0 = (uint32_t)y * desk_rb + (uint32_t)(x * depth / 8);
            uint32_t k1 = (uint32_t)y * desk_rb + (uint32_t)(((x + 1) * depth + 7) / 8);
            if (x < 0 || y < 0 || k1 > desk_size)
                continue;
            bool desk = true;
            for (uint32_t k = k0; k < k1 && desk; k++)
                desk = desk_have[k] && rd8(base + k) == desk_val[k];
            if (desk)
                out[(size_t)r * (size_t)width + (size_t)c] = 0;
        }
    pthread_mutex_unlock(&desk_lock);
    if (!ok)
        return false;
    /* The windows are the program's, whatever their pixels. */
    pthread_mutex_lock(&lock);
    for (int i = 0; i < nwins; i++) {
        qrect wb = wins[i].bounds;
        int bw = wb.right - wb.left;
        for (int y = wb.top; y < wb.bottom; y++)
            for (int x = wb.left; x < wb.right; x++) {
                int c = x - left, r = y - top;
                if (c >= 0 && r >= 0 && c < width && r < height && wins[i].mask &&
                    wins[i].mask[(size_t)(y - wb.top) * (size_t)bw + (size_t)(x - wb.left)])
                    out[(size_t)r * (size_t)width + (size_t)c] = 1;
            }
    }
    pthread_mutex_unlock(&lock);
    return true;
}

/* ---------------------------------------------------------------------- */
/* Drawing on the desktop by the system's own code: an INIT's or a desk
 * accessory's resident code in the system heap (the Talking Moose, which
 * draws itself on the screen with stores of its own while it speaks).
 * That is not the program wanting the whole screen: in rootless mode the
 * host shows what it drew floating over the host's desktop, as long as
 * it is there.  Each byte of the screen such code writes keeps what was
 * there before its first store; the overlay is the written bytes while
 * any of them differs from that, and ends when all are back. */

BB_HOST static pthread_mutex_t ov_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *ov_orig, *ov_have; /* per byte of the screen */
static uint32_t ov_size, ov_rb;
static gaddr ov_base;
static int ov_top, ov_bottom, ov_left, ov_right; /* rows, and bytes in a row, written (inclusive) */
static bool ov_on;
static uint32_t ov_version;

/* Whether the store being made comes from code in the system heap. */
static bool system_code_store(void)
{
    cpu68k *c = g_cpu;
    if (!c || g_ppc_mode)
        return false;
    gaddr z = rd32(LM_SysZone), pc = mem_strip(c->op_pc);
    return z && pc >= z && pc < rd32(z);
}

static void overlay_written(gaddr base, uint32_t off, uint32_t len)
{
    uint32_t rb = qd_screen_row_bytes();
    int w, h;
    machine_screen_size(&w, &h);
    uint32_t size = rb * (uint32_t)h;
    pthread_mutex_lock(&ov_lock);
    if (size != ov_size || base != ov_base || rb != ov_rb) {
        bb_free(ov_orig);
        bb_free(ov_have);
        ov_orig = bb_malloc(size, BB_BYTES);
        ov_have = bb_calloc(size, 1, BB_BYTES);
        ov_size = ov_orig && ov_have ? size : 0;
        ov_base = base;
        ov_rb = rb;
        ov_on = false;
    }
    if (off < ov_size) {
        int row = (int)(off / rb), col = (int)(off % rb);
        for (uint32_t k = off; k < off + len && k < ov_size; k++)
            if (!ov_have[k]) {
                ov_have[k] = 1;
                ov_orig[k] = rd8(base + k); /* the store is still to come */
            }
        int last = col + (int)len - 1 < (int)rb ? col + (int)len - 1 : (int)rb - 1;
        if (!ov_on) {
            ov_on = true;
            ov_top = ov_bottom = row;
            ov_left = col;
            ov_right = last;
            ov_version++;
        } else if (row < ov_top || row > ov_bottom || col < ov_left || last > ov_right) {
            if (row < ov_top) ov_top = row;
            if (row > ov_bottom) ov_bottom = row;
            if (col < ov_left) ov_left = col;
            if (last > ov_right) ov_right = last;
            ov_version++;
        }
    }
    pthread_mutex_unlock(&ov_lock);
}

/* Whether a byte the system code wrote still differs; with ov_lock held. */
static bool ov_differs(uint32_t k) { return ov_have[k] && rd8(ov_base + k) != ov_orig[k]; }

uint32_t machine_overlay(int16_t rect[4], bool *on)
{
    pthread_mutex_lock(&ov_lock);
    bool any = false;
    for (int y = ov_top; ov_on && y <= ov_bottom && !any; y++)
        for (int x = ov_left; x <= ov_right && !any; x++)
            any = ov_differs((uint32_t)y * ov_rb + (uint32_t)x);
    if (ov_on && !any) { /* all put back: over */
        for (int y = ov_top; y <= ov_bottom; y++)
            memset(ov_have + (uint32_t)y * ov_rb + (uint32_t)ov_left, 0, (size_t)(ov_right - ov_left + 1));
        ov_on = false;
        ov_version++;
    }
    *on = ov_on;
    int depth = qd_screen_depth();
    rect[0] = (int16_t)ov_top;
    rect[1] = (int16_t)(ov_left * 8 / depth);
    rect[2] = (int16_t)(ov_bottom + 1);
    rect[3] = (int16_t)((ov_right + 1) * 8 / depth);
    uint32_t v = ov_version;
    pthread_mutex_unlock(&ov_lock);
    return v;
}

/* The overlay's pixels in RGBA, clear where the system code didn't write. */
void machine_overlay_rgba(int x, int y, int w, int h, uint8_t *out)
{
    machine_screen_rgba(x, y, w, h, out);
    int depth = qd_screen_depth();
    pthread_mutex_lock(&ov_lock);
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++) {
            int sx = x + c, sy = y + r;
            uint32_t k = (uint32_t)sy * ov_rb + (uint32_t)(sx * depth / 8);
            if (sx < 0 || sy < 0 || k >= ov_size || !ov_have[k])
                out[((size_t)r * (size_t)w + (size_t)c) * 4 + 3] = 0;
        }
    pthread_mutex_unlock(&ov_lock);
}

/* A store the program made into the screen (mem.h's watch). */
static void screen_written(gaddr a, uint32_t len)
{
    gaddr base = rd32(LM_ScrnBase);
    /* The compact Macs' two pages are watched together; only the one the
     * video circuitry shows counts. */
    if (hw_machine()->via_sound) {
        if (a < base) {
            if (!hw_screen_alternate())
                return;
            a += 0x8000;
        } else if (hw_screen_alternate()) {
            return;
        }
    }
    uint32_t off = a - base, rb = qd_screen_row_bytes();
    if (system_code_store()) {
        overlay_written(base, off, len);
        return;
    }
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

/* BLUEBOX_WATCH=addr[,len]: the processors' stores into guest memory there
 * logged with the instruction that made them, for finding what damages a
 * heap between two Toolbox calls (the screen is not watched meanwhile). */
static gaddr debug_watch_lo, debug_watch_len;

static void debug_watch_written(gaddr a, uint32_t len)
{
    static unsigned n;
    if (n++ < 200)
        log_printf("watch: %u-byte store to %08X at 68k pc %08X, PowerPC pc %08X lr %08X\n", len, a, g_cpu ? g_cpu->op_pc : 0,
                   g_ppc.pc, g_ppc.lr);
}

/* The screen's range, watched for the program's own stores to it; and
 * while idle.c is making sure a program waiting changes nothing, every
 * store, to it as well. */
static gaddr screen_watch_lo, screen_watch_len;
/* Saved with the rest: a snapshot taken while idle.c watches has g_watch_fn
 * at any_store, which calls it. */
static void (*all_stores)(gaddr a, uint32_t len);

static void any_store(gaddr a, uint32_t len)
{
    if (a - screen_watch_lo < screen_watch_len)
        screen_written(a, len);
    all_stores(a, len);
}

static void apply_watch(void)
{
    if (all_stores) {
        g_watch_fn = any_store;
        g_watch_lo = 0;
        g_watch_len = 0xFFFFFFFFu;
    } else {
        g_watch_fn = screen_written;
        g_watch_lo = screen_watch_lo;
        g_watch_len = screen_watch_len;
    }
}

static bool debug_watch(void);

/* False if they can't be (BLUEBOX_WATCH has the watch). */
bool display_watch_all_stores(void (*fn)(gaddr a, uint32_t len))
{
    all_stores = fn;
    if (debug_watch())
        return false;
    apply_watch();
    return true;
}

static bool debug_watch(void)
{
    BB_HOST static int on = -1; /* the run's environment */
    if (on < 0) {
        const char *e = getenv("BLUEBOX_WATCH");
        on = e && *e;
        if (on) {
            char *end;
            debug_watch_lo = (gaddr)strtoul(e, &end, 16);
            debug_watch_len = *end == ',' ? (gaddr)strtoul(end + 1, NULL, 0) : 4;
        }
    }
    if (on) {
        g_watch_fn = debug_watch_written;
        g_watch_lo = debug_watch_lo;
        g_watch_len = debug_watch_len;
    }
    return on;
}

static void check_screen_mode(void)
{
    if (debug_watch())
        return;
    if (!screen_reason) {
        if ((int16_t)rd16(LM_MBarHeight) == 0)
            display_want_screen("it hides the menu bar");
        else if (direct_written && !writes_in_a_window())
            display_want_screen("it draws on the screen outside its windows");
    }
    direct_written = false;
    int w, h;
    machine_screen_size(&w, &h);
    screen_watch_lo = rd32(LM_ScrnBase);
    screen_watch_len = screen_reason ? 0 : qd_screen_row_bytes() * (uint32_t)h; /* decided: no more watching */
    if (hw_machine()->via_sound && screen_watch_len) { /* and the alternate page below the main */
        screen_watch_lo -= 0x8000;
        screen_watch_len += 0x8000;
    }
    apply_watch();
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
    pthread_mutex_lock(&ov_lock);
    ov_on = false;
    ov_size = 0;
    ov_version++;
    pthread_mutex_unlock(&ov_lock);
    screen_reason = NULL;
    direct_written = false;
    screen_watch_len = 0;
    all_stores = NULL;
    g_watch_len = 0;
    wins_dirty = menus_dirty = true;
    pending_choice = 0;
    tracking = in_select = track_done = false;
}

/* ---------------------------------------------------------------------- */
/* Transient drawing outside the windows: a pop-up menu.  Rootless hosts
 * show the area above everything while it is up, and take every click. */

BB_HOST static pthread_mutex_t transient_lock = PTHREAD_MUTEX_INITIALIZER;
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

/* A host attached to a resumed machine has seen none of its windows, menu
 * bar, pop-up or overlay, whatever versions the snapshot's host last saw
 * (the counters come back as the snapshot had them): each moves on, so the
 * host takes all of them afresh.  Only then: a headless resume, with no
 * host, ends as an unbroken run does. */
void display_host_attached_after_resume(void)
{
    pthread_mutex_lock(&lock);
    win_version += 0x10000;
    wins_dirty = true; /* and published again */
    menu_version += 0x10000;
    pthread_mutex_unlock(&lock);
    pthread_mutex_lock(&transient_lock);
    transient_version += 0x10000;
    pthread_mutex_unlock(&transient_lock);
    pthread_mutex_lock(&ov_lock);
    ov_version += 0x10000;
    pthread_mutex_unlock(&ov_lock);
}
