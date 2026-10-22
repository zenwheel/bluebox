/*
 * The menu walker (--walk-menus): a headless run that chooses every menu
 * item it can, to find what breaks beyond a program's first screen.
 *
 * Whenever the program waits for input and a few million instructions
 * have passed since the last step, the walker takes one: a dialog in
 * front is dismissed (Escape, Command-period, Return, Enter; the default
 * button first before anything has been chosen), otherwise the next
 * enabled item of the menu bar is chosen, as
 * the user would.  Items that would end the run or touch the host's disks
 * (Quit, Restart, Shut Down, Erase, Format) are passed over, as are
 * submenus and the Apple menu's desk accessories.  Each step is printed,
 * so a run that ends in a crash, an error or damaged heap (with
 * BLUEBOX_HEAPCHECK) says which item it came from.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "machine.h"
#include "tb_internal.h"

/* Instructions between steps (the program's own work), and from the menu
 * bar's first appearance to the first choice (a program's opening, while
 * it keeps its menus disabled: Glider's title). */
enum { SETTLE = 3000000, WARM_UP = 30000000, WAITS = 18 };

static bool walking;
static int menu_i, item_n;  /* the next item to try */
static int tries;           /* attempts at the dialog in front */
static int waits;           /* steps with every item left disabled */
static gaddr left_open;     /* one that stayed: a modeless window, walked past */
static uint64_t next_at, menus_at;
static unsigned chosen, dialogs, left;
static char last[128];

void machine_set_menu_walk(bool on)
{
    walking = on;
    menu_i = 0;
    item_n = 1;
    tries = waits = 0;
    next_at = menus_at = 0;
    chosen = dialogs = left = 0;
    left_open = 0;
    snprintf(last, sizeof last, "the start");
}

static bool passed_over(const char *text)
{
    static const char *const words[] = { "quit", "exit", "restart", "shut down", "log out", "erase", "format",
                                         "initialize" };
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++)
        if (strcasestr(text, words[i]))
            return true;
    return false;
}

static void key(int ch, int code, unsigned modifiers)
{
    machine_post_key(ch, code, modifiers, 1, 0);
    machine_post_key(ch, code, modifiers, 0, 0);
}

static void say(const char *what)
{
    printf("walk: %s\n", what);
    fflush(stdout);
}

void walker_idle(uint64_t now)
{
    if (!walking || !g_cpu || g_cpu->halted || now < next_at)
        return;
    next_at = now + SETTLE;
    gaddr d = dialog_in_front();
    if (d && d != left_open) {
        if (!tries)
            dialogs++;
        /* Before the walk's first choice, a dialog is the program's own
         * start (a splash, a registration notice, Bolo's game setup), whose
         * Cancel may well be Quit: its default button first.  After, one a
         * menu item opened: Cancel first. */
        static const int starting[5] = { 2, 3, 0, 1, 4 }, opened[5] = { 0, 1, 2, 3, 4 };
        int step = tries < 5 ? (chosen ? opened : starting)[tries] : 5;
        tries++;
        switch (step) {
        case 0: key(0x1B, 0x35, 0); return;             /* Escape */
        case 1: key('.', 0x2F, 0x0100); return;         /* Command-period */
        case 2: key(0x0D, 0x24, 0); return;             /* Return */
        case 3: key(0x03, 0x4C, 0); return;             /* Enter */
        case 4: key('w', 0x0D, 0x0100); return;         /* Command-W, for a modeless one */
        default:
            /* Still there: a modeless window most likely, which the menus
             * get past; a modal one ends the walk at the next step's turn,
             * as nothing else will change. */
            if (d == left_open || (left_open && !chosen)) {
                printf("walk: a dialog stays up after %s\n", last);
                cpu_halt(g_cpu, "menu walk stuck in a dialog after %s", last);
                return;
            }
            printf("walk: a dialog stays open after %s; walking past it\n", last);
            fflush(stdout);
            left_open = d;
            left++;
            break;
        }
    }
    tries = 0;
    if (!menu_bar_count())
        return; /* no menu bar yet: the program is still starting */
    if (!menus_at)
        menus_at = now;
    if (now - menus_at < WARM_UP)
        return;
    /* The next item that can be chosen, from where the walk is.  Items
     * disabled now are not given up on: a program keeps its menus disabled
     * while it shows a screen of its own (Glider's About), so the walk
     * waits for them, nudging the program on (Return, a click, Escape),
     * and ends only when nothing becomes enabled. */
    bool waiting_for = false;
    for (int mi = menu_i, n = item_n; mi < menu_bar_count(); mi++, n = 1) {
        for (;; n++) {
            char text[64];
            bool enabled, submenu;
            if (!menu_bar_item(mi, n, text, sizeof text, &enabled, &submenu))
                break;
            bool candidate = !submenu && !passed_over(text) && !(mi == 0 && n > 1) && text[0] != '-';
            if (!candidate || !enabled) {
                LOGF(LOG_TRAPS, "  walk passes menu %d item %d \"%s\"%s\n", menu_bar_id(mi), n, text,
                     enabled ? "" : " (disabled)");
                waiting_for |= candidate;
                continue;
            }
            int16_t id = menu_bar_id(mi);
            snprintf(last, sizeof last, "menu %d item %d \"%s\"", id, n, text);
            say(last);
            chosen++;
            menu_i = mi;
            item_n = n + 1;
            waits = 0;
            machine_choose_menu(id, (int16_t)n);
            return;
        }
    }
    if (waiting_for && waits < WAITS) {
        int w = 0, h = 0;
        switch (waits++ % 6) {
        case 1: key(0x0D, 0x24, 0); break;
        case 3: /* pressed for a step, as a program polling Button sees it */
        case 4:
            machine_screen_size(&w, &h);
            machine_post_mouse(w / 2, h / 2, waits % 6 == 4);
            break;
        case 5: key(0x1B, 0x35, 0); break;
        default: break;
        }
        return;
    }
    printf("walk: %u items chosen, %u dialogs, %u left open\n", chosen, dialogs, left);
    fflush(stdout);
    walking = false;
    cpu_halt(g_cpu, "menu walk finished");
}
