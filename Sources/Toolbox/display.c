/*
 * What the host needs to present rootless windows and native menus.
 *
 * The emulator thread publishes snapshots (window list with shape masks,
 * and the menu bar) under a mutex at safe points; the host thread reads
 * them.  The host never walks guest structures itself.
 */
#include <pthread.h>
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
} win_snap;

static win_snap wins[64];
static int nwins;
static uint32_t win_version;
static bool wins_dirty = true;

void display_windows_changed(void) { wins_dirty = true; }

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
        fresh[n++] = (win_snap){ w, b, mask, WindowRecord_hilited(w) != 0 };
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
        out[i] = (machine_window){ wins[i].id, wins[i].bounds.top, wins[i].bounds.left, wins[i].bounds.bottom,
                                   wins[i].bounds.right, wins[i].active };
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

/* A choice made in the host's menu bar, delivered through MenuSelect. */
static int32_t pending_choice;

void machine_choose_menu(int16_t menu_id, int16_t item)
{
    pthread_mutex_lock(&lock);
    pending_choice = ((int32_t)menu_id << 16) | (uint16_t)item;
    pthread_mutex_unlock(&lock);
    /* The app sees a click in its menu bar and calls MenuSelect. */
    void ev_post_menubar_click(void);
    ev_post_menubar_click();
}

int32_t display_take_menu_choice(void)
{
    pthread_mutex_lock(&lock);
    int32_t c = pending_choice;
    pending_choice = 0;
    pthread_mutex_unlock(&lock);
    return c;
}

/* ---------------------------------------------------------------------- */

/* Called by the Event Manager on the emulator thread: a safe point. */
void display_sync(void)
{
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
    wins_dirty = menus_dirty = true;
    pending_choice = 0;
}
