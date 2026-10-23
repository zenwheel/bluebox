/*
 * The menu walker (--walk-menus): a headless run that chooses every menu
 * item it can, to find what breaks beyond a program's first screen.
 *
 * Whenever the program waits for input and a few million instructions
 * have passed since the last step, the walker takes one: a dialog in
 * front is dismissed (Escape, Command-period, Return, Enter; the default
 * button first before anything has been chosen; then a click on each of
 * its buttons), otherwise the next enabled item of the menu bar is
 * chosen, as the user would.  Items and buttons that would end the run or
 * touch the host's disks (Quit, Restart, Shut Down, Erase, Format) are
 * passed over, as are submenus and the Apple menu's desk accessories.  Each step is printed,
 * so a run that ends in a crash, an error or damaged heap (with
 * BLUEBOX_HEAPCHECK) says which item it came from.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "machine.h"
#include "tb_internal.h"

/* Time between steps (the program's own work), and from the menu bar's
 * first appearance to the first choice (a program's opening, while it
 * keeps its menus disabled: Glider's title), in the machine's time, which
 * drawing moves on as well as instructions (Diamonds draws a screenful
 * between polls of the keyboard). */
enum { WAITS = 18, BUTTON_WAITS = 100 };
static const uint64_t SETTLE = 300000000, WARM_UP = 3000000000; /* ns */

static bool walking;
static int menu_i, item_n;  /* the next item to try */
static int waits;           /* steps with every item left disabled */
static uint32_t left_open;  /* one that stayed (its signature): a modeless window, walked past */

/* The dialogs being dismissed and how far each has got, told apart by
 * their items, from one menu choice to the next: one a program puts up
 * again (a registration notice after a wrong code, an installer's
 * "Stopping now" after its Resume, a program that is all one dialog after
 * each of its buttons) goes on from where the last left off. */
enum { NDIALOGS = 16 };
static struct {
    uint32_t sig;
    int tries;        /* keys pressed */
    uint64_t clicked; /* buttons clicked, by item */
    int waited;       /* steps waiting for a dimmed button */
    bool quits;       /* a button passed over as Quit */
} open_dialogs[NDIALOGS];
static int nopen;
static uint64_t next_at, menus_at, started_at;
static unsigned chosen, dialogs, left;
static char last[128];

void machine_set_menu_walk(bool on)
{
    walking = on;
    menu_i = 0;
    item_n = 1;
    waits = nopen = 0;
    next_at = menus_at = started_at = 0;
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

/* A click on the screen, let go a while later as scripted input, which a
 * Toolbox loop tracking the press (LClick, TrackControl) sees too. */
static void click(int x, int y)
{
    machine_post_mouse(x, y, 1);
    machine_script_add(g_cpu->icount + 1000000, 1, x, y, 0); /* scripts go by instructions */
}

static void say(const char *what)
{
    printf("walk: %s\n", what);
    fflush(stdout);
}

/* The program nudged on from a screen of its own, every other step:
 * Return, a click, Escape.  False when it has been nudged enough. */
static bool nudge(void)
{
    if (waits >= WAITS)
        return false;
    int w = 0, h = 0;
    switch (waits++ % 6) {
    case 1: key(0x0D, 0x24, 0); break;
    case 3:
        /* A program polling Button finds it down. */
        machine_screen_size(&w, &h);
        click(w / 2, h / 2);
        break;
    case 5: key(0x1B, 0x35, 0); break;
    default: break;
    }
    return true;
}

/* The end of the walk at a dialog that stays.  With a Quit of its own,
 * its other buttons pressed and no menus to walk, it is the program
 * (MacroMind Player), walked; else it wants
 * what the walk can't give, as a registration's serial number. */
static void stays_up(int at)
{
    walking = false;
    if (open_dialogs[at].quits && !menu_bar_count()) {
        printf("walk: the program is a dialog, its buttons pressed\n");
        printf("walk: %u items chosen, %u dialogs, %u left open\n", chosen, dialogs, left);
        fflush(stdout);
        cpu_halt(g_cpu, "menu walk finished");
        return;
    }
    printf("walk: a dialog stays up after %s\n", last);
    fflush(stdout);
    cpu_halt(g_cpu, "menu walk stuck in a dialog after %s", last);
}

void walker_idle(uint64_t icount)
{
    (void)icount;
    uint64_t now = machine_time_ns();
    if (!walking || !g_cpu || g_cpu->halted || now < next_at)
        return;
    next_at = now + SETTLE;
    gaddr d = dialog_in_front();
    uint32_t sig = d ? dialog_signature(d) : 0;
    if (d && sig != left_open) {
        int at = 0;
        while (at < nopen && open_dialogs[at].sig != sig)
            at++;
        if (at == nopen) {
            if (nopen == NDIALOGS)
                memmove(&open_dialogs[0], &open_dialogs[1], (size_t)--nopen * sizeof open_dialogs[0]);
            at = nopen++;
            open_dialogs[at].sig = sig;
            open_dialogs[at].tries = open_dialogs[at].waited = 0;
            open_dialogs[at].clicked = 0;
            open_dialogs[at].quits = false;
            dialogs++;
        }
        int *tries = &open_dialogs[at].tries;
        /* Before the walk's first choice, a dialog is the program's own
         * start (a splash, a registration notice, Bolo's game setup), whose
         * Cancel may well be Quit: its default button first.  After, one a
         * menu item opened: Cancel first. */
        static const int starting[5] = { 2, 3, 0, 1, 4 }, opened[5] = { 0, 1, 2, 3, 4 };
        int step = *tries < 5 ? (chosen ? opened : starting)[*tries] : 5;
        if (step < 5)
            ++*tries;
        switch (step) {
        case 0: key(0x1B, 0x35, 0); return;             /* Escape */
        case 1: key('.', 0x2F, 0x0100); return;         /* Command-period */
        case 2: key(0x0D, 0x24, 0); return;             /* Return */
        case 3: key(0x03, 0x4C, 0); return;             /* Enter */
        case 4: key('w', 0x0D, 0x0100); return;         /* Command-W, for a modeless one */
        default: {
            /* Keys did nothing for a modal dialog: its buttons, a click on
             * each in turn ("Not Yet" with no Cancel item, a nag's "Demo"
             * past a Return that only asks for the code again, the buttons
             * of a program that is all one dialog), but for those that
             * would end the run, and a starting one's Cancel, which may
             * well quit.  (Not a window the program runs itself: an
             * installer's progress, whose Stop would cut it short.) */
            char text[64], what[96];
            int x, y, b;
            bool modal = dialog_modal(d), dimmed = false;
            for (int i = 1; modal && i <= 64 && (b = dialog_button(d, i, text, sizeof text, &x, &y)) >= 0; i++) {
                if (!b || (open_dialogs[at].clicked & 1ull << (i - 1)))
                    continue;
                if (passed_over(text)) {
                    open_dialogs[at].quits = true;
                    continue;
                }
                if (!chosen && strcasestr(text, "cancel"))
                    continue;
                if (b == 1) {
                    dimmed = true;
                    continue;
                }
                open_dialogs[at].clicked |= 1ull << (i - 1);
                snprintf(what, sizeof what, "button \"%s\"", text);
                say(what);
                click(x, y);
                return;
            }
            /* A button dimmed for now: a nag's "Not Yet" counting down. */
            if (dimmed && open_dialogs[at].waited++ < BUTTON_WAITS)
                return;
            /* Still there: a modeless window most likely, which the menus
             * get past; a modal one ends the walk at the next step's turn,
             * as nothing else will change. */
            if (left_open && !chosen) {
                stays_up(at);
                return;
            }
            printf("walk: a dialog stays open after %s; walking past it\n", last);
            fflush(stdout);
            left_open = sig;
            left++;
            break;
        }
        }
    }
    if (!started_at)
        started_at = now;
    if (!menu_bar_count()) {
        /* No menu bar yet: the program is starting, or showing a title of
         * its own that waits for a key or a click (Diamonds). */
        if (now - started_at < WARM_UP || nudge())
            return;
        if (d && sig == left_open) {
            /* The dialog walked past, its buttons pressed, is all there
             * is.  (Without buttons it is more likely a screen of the
             * program's own: Space Gypsy's, all pictures.) */
            int at = 0;
            while (at < nopen && open_dialogs[at].sig != sig)
                at++;
            if (at < nopen && (open_dialogs[at].clicked || open_dialogs[at].quits)) {
                stays_up(at);
                return;
            }
        }
        printf("walk: no menu bar appeared\n");
        fflush(stdout);
        walking = false;
        cpu_halt(g_cpu, "menu walk found no menu bar");
        return;
    }
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
            nopen = 0;
            menu_i = mi;
            item_n = n + 1;
            waits = 0;
            machine_choose_menu(id, (int16_t)n);
            return;
        }
    }
    if (waiting_for && nudge())
        return;
    printf("walk: %u items chosen, %u dialogs, %u left open\n", chosen, dialogs, left);
    fflush(stdout);
    walking = false;
    cpu_halt(g_cpu, "menu walk finished");
}
