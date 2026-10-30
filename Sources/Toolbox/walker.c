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
 *
 * A program with no menu bar is nudged with keys and clicks, then has the
 * buttons of its window pressed (a program that is one window), or with
 * none, the pictures drawn in it clicked; one that draws on meanwhile (a
 * game) is walked too.  Else, half a minute from its start, the walk ends
 * "found no menu bar", or "no menu bar or window" for one with nothing on
 * the screen (a background program).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "machine.h"
#include "qd_internal.h"
#include "tb_internal.h"

/* Time between steps (the program's own work), and from the menu bar's
 * first appearance to the first choice (a program's opening, while it
 * keeps its menus disabled: Glider's title), in the machine's time, which
 * drawing moves on as well as instructions (Diamonds draws a screenful
 * between polls of the keyboard). */
enum { WAITS = 18, BUTTON_WAITS = 100 };
static const uint64_t SETTLE = 300000000, WARM_UP = 3000000000; /* ns */
/* The least time from the start before a program is found to have no
 * menu bar: one still starting behind a window of its own (MegaSeg's
 * launch after its notice, a map generated behind a progress bar) may
 * take longer than its nudging. */
static const uint64_t PATIENCE = 30000000000; /* ns */

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
static unsigned targets_clicked; /* by the screen walk (below) */
static char last[128];

/* With no menu bar, the buttons of the window in front pressed in turn
 * (a program that is one window: Disinfectant 1.6, an Installer, a
 * licence to agree to), told apart by their titles and places. */
enum { NWINDOWS = 8 };
static struct {
    uint32_t sig;
    uint64_t clicked;
} pressed_in[NWINDOWS];
static int npressed_in;
static unsigned pressed;  /* buttons pressed so */
static unsigned pictured; /* pictures clicked so */
static int picture_waits; /* steps since the last, for what it opens */
static int button_waits;  /* steps waiting for a dimmed one */
/* The screen of a program with no menu bar at the last step, from halfway
 * through its first nudging on, and whether it changed from one step to
 * the next since: to tell one that goes on drawing (a game, or one
 * answering its keys and clicks on a screen of its own) from one stuck on
 * a still screen. */
static uint8_t *screen_then;
static int screen_w, screen_h;
static bool screen_moved;
static gaddr front_then; /* the window in front at the last step */
/* The pictures drawn into windows (DrawPicture, CopyBits from a picture
 * drawn off screen), by port and local rectangle: a window with no
 * buttons whose buttons are pictures (AutoEye's "Manual" and "Automatic")
 * has them clicked in turn. */
enum { NSPOTS = 128 };
static struct {
    gaddr port;
    qrect r;
    bool clicked;
} spots[NSPOTS];
static int nspots;

static void screen_walk_reset(void);

void machine_set_menu_walk(bool on)
{
    walking = on;
    menu_i = 0;
    item_n = 1;
    waits = nopen = 0;
    next_at = menus_at = started_at = 0;
    chosen = dialogs = left = pressed = pictured = 0;
    left_open = 0;
    npressed_in = 0;
    nspots = picture_waits = 0;
    button_waits = 0;
    free(screen_then);
    screen_then = NULL;
    screen_moved = false;
    front_then = 0;
    screen_walk_reset();
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

/* Whether the walk has done anything yet (a menu item chosen, a button
 * pressed): a dialog before is the program's own start. */
static bool begun(void) { return chosen || pressed || pictured || targets_clicked; }

/* A button that would turn the program down at its start (a licence's
 * Decline), as Cancel would. */
static bool declines(const char *text)
{
    return strcasestr(text, "cancel") || strcasestr(text, "decline") || strcasestr(text, "disagree");
}

/* Whether window w has a button of its own that quits (dimmed or not):
 * its Cancel then only cancels (HDBackup's, while it asks for a disk with
 * its Quit dimmed), and is pressed at the start as any other. */
static bool has_quit(gaddr w, bool dialog)
{
    char text[64];
    int x, y, b;
    for (int i = 1; i <= 64 && (b = dialog ? dialog_button(w, i, text, sizeof text, &x, &y)
                                           : window_button(w, i, text, sizeof text, &x, &y)) >= 0; i++)
        if (b && (strcasestr(text, "quit") || strcasestr(text, "exit")))
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

/* ---- The screen walk ----
 * A program with no menu bar that is neither a window of buttons nor a
 * game drawing on, and still stuck on its screen after its nudging (a
 * game's title of its own, with buttons it draws and tests the mouse
 * against itself: Bugs!'s, Ultris's), is walked by what is on its screen.
 * Its click targets are the rectangles and regions it tests the mouse
 * against (PtInRect, PtInRgn, as it handles a click or follows the
 * mouse), the controls and items of its window, and the pictures, icons
 * and short texts it draws on the screen.  Its screens are told apart by a
 * coarse picture of the screen, each with targets of its own, clicked in
 * turn (those labelled Quit or Exit passed over, as the menus' are), then
 * a few keys.  A step is productive when the screen changes or the
 * program runs code it had not run (a Toolbox call from a new place).  The
 * walk ends when the screen it is on has nothing left to try, or after
 * its time. */
enum { THUMB_W = 32, THUMB_H = 24, THUMB_SIZE = THUMB_W * THUMB_H * 3, NSCREENS = 48, NTARGETS = 1024 };
enum { NHASH = 4096, NCALLS = 1 << 16, SCREEN_STEPS = 300 };
static const uint64_t SCREEN_TIME = 90000000000; /* ns */
/* Kinds of target, in the order they are tried. */
enum { T_CONTROL, T_HIT, T_HOVER, T_SHAPE, T_DRAWN, T_TEXT, T_KINDS };
static const char *const target_kinds[T_KINDS] = { "control", "hit test", "hover", "button", "picture", "text" };
static struct {
    qrect r;        /* on the screen */
    int16_t x, y;   /* where a click goes */
    int16_t screen; /* the screen it was seen on; -1: drawn since the last step, -2: none (a duplicate) */
    uint8_t kind;
    bool tried;
    char label[32];
} targets[NTARGETS];
static int ntargets;
static int nnotes, last_note = -1; /* what is drawn off the screen (below) */
static int16_t target_hash[NHASH]; /* hit tests by rectangle and screen: index + 1, 0 empty */
static uint8_t (*thumbs)[THUMB_SIZE]; /* each screen's picture */
static uint8_t keys_tried[NSCREENS];
/* Targets clicked on each screen by kind: no more than PER_KIND pictures
 * or texts (a game's pieces, its score). */
enum { PER_KIND = 16 };
static uint8_t clicked_on[NSCREENS][T_KINDS];
static uint8_t thumb_then[THUMB_SIZE];
static int nscreens, on_screen = -1, acted_on = -1, settling, screen_steps;
static bool screening;
/* The screen watched a few steps before the walk's first, untouched: one
 * that changes by itself (a box blinking, a title's animation) is not
 * moved on by a step for changing; only code run tells. */
enum { WATCH_STEPS = 8 };
static int watched;
static bool self_moving;
static unsigned keys_given, productive;
static uint64_t screening_since;
/* The places Toolbox calls came from, a set: a new one is new code run. */
static uint32_t *call_sites;
static unsigned ncall_sites, calls_then;

/* The keys given on each screen, after its targets; the letters only on
 * a screen nothing has moved on yet (a title asking "Mouse or keyboard?
 * M/K", a game polling the keyboard for its own keys).  Not Q. */
static const struct {
    int ch, code;
    const char *name;
} screen_keys[] = { { 0x0D, 0x24, "Return" },    { ' ', 0x31, "space" }, { 0x1B, 0x35, "Escape" },
                    { 0x1F, 0x7D, "down arrow" }, { '1', 0x12, "1" },     { 'a', 0x00, "a" },
                    { 'b', 0x0B, "b" },           { 'c', 0x08, "c" },     { 'd', 0x02, "d" },
                    { 'e', 0x0E, "e" },           { 'f', 0x03, "f" },     { 'g', 0x05, "g" },
                    { 'h', 0x04, "h" },           { 'i', 0x22, "i" },     { 'j', 0x26, "j" },
                    { 'k', 0x28, "k" },           { 'l', 0x25, "l" },     { 'm', 0x2E, "m" },
                    { 'n', 0x2D, "n" },           { 'o', 0x1F, "o" },     { 'p', 0x23, "p" },
                    { 'r', 0x0F, "r" },           { 's', 0x01, "s" },     { 't', 0x11, "t" },
                    { 'u', 0x20, "u" },           { 'v', 0x09, "v" },     { 'w', 0x0D, "w" },
                    { 'x', 0x07, "x" },           { 'y', 0x10, "y" },     { 'z', 0x06, "z" } };
enum { NKEYS = sizeof screen_keys / sizeof screen_keys[0], NAMED_KEYS = 5 };
static bool moved_from[NSCREENS]; /* a step on the screen was productive */
/* The mouse swept over a screen once its targets are tried, for what it
 * does there: a picture drawn as it passes (a button lit), the cursor
 * changed, code run that had not run: targets at where it is (a program
 * that tests the mouse itself: Evolutionary War's planets). */
enum { SWEEP_W = 16, SWEEP_H = 12, SWEEP_GAP = 200000 /* instructions */ };
static bool swept[NSCREENS];
static int sweep_steps; /* steps the sweep under way has left */
static uint32_t usual_cursor;

/* Steps not to take (--walk-avoid), as "X,Y" or "key NAME": kept from one
 * walk to the next. */
enum { NAVOID = 16 };
static char avoided[NAVOID][40];
static int navoided;

void machine_walk_avoid(const char *step)
{
    if (!step)
        navoided = 0;
    else if (navoided < NAVOID)
        snprintf(avoided[navoided++], sizeof avoided[0], "%s", step);
}

static bool avoid(const char *step)
{
    for (int i = 0; i < navoided; i++)
        if (!strcmp(avoided[i], step))
            return true;
    return false;
}

static void screen_walk_reset(void)
{
    ntargets = nscreens = screen_steps = settling = nnotes = sweep_steps = 0;
    usual_cursor = 0;
    last_note = -1;
    on_screen = acted_on = -1;
    screening = self_moving = false;
    watched = 0;
    targets_clicked = keys_given = productive = 0;
    screening_since = 0;
    memset(target_hash, 0, sizeof target_hash);
    free(thumbs);
    thumbs = NULL;
    free(call_sites);
    call_sites = NULL;
    ncall_sites = calls_then = 0;
}

/* Whether what the program tests and draws is looked at: walking, with
 * no menu bar (from the start, before the walk's first step). */
static bool watching(void) { return walking && !menu_bar_count(); }

static void add_target(int kind, qrect r, int x, int y, int screen, const char *label);

/* Something the program did as the mouse was swept over it. */
static void hover_here(void)
{
    if (!sweep_steps || on_screen < 0)
        return;
    uint32_t g = ev_mouse_global();
    int x = pt_h(g), y = pt_v(g);
    add_target(T_HOVER, (qrect){ (int16_t)(y - 8), (int16_t)(x - 8), (int16_t)(y + 8), (int16_t)(x + 8) }, x, y,
               on_screen, NULL);
}

void walker_note_cursor(gaddr data)
{
    if (!watching() || !data)
        return;
    uint32_t h = 2166136261u;
    for (int i = 0; i < 32; i++)
        h = (h ^ rd8(data + (gaddr)i)) * 16777619u;
    if (!sweep_steps)
        usual_cursor = h;
    else if (h != usual_cursor)
        hover_here();
}

void walker_note_call(uint32_t pc, uint32_t what)
{
    /* The routine called with the place it was called from: PowerPC
     * code may call all of them through one routine of its own. */
    pc ^= what * 2654435761u;
    if (!walking || !pc)
        return;
    if (!call_sites && !(call_sites = calloc(NCALLS, sizeof *call_sites)))
        return;
    if (ncall_sites >= NCALLS / 2)
        return;
    for (uint32_t h = (pc * 2654435761u) >> 16;; h = (h + 1) & (NCALLS - 1)) {
        if (call_sites[h] == pc)
            return;
        if (!call_sites[h]) {
            call_sites[h] = pc;
            ncall_sites++;
            hover_here();
            return;
        }
    }
}

/* Local coordinates in a port that draws on the screen, to the screen's:
 * the amounts to add. */
static bool port_on_screen(gaddr port, int *dh, int *dv)
{
    if (!port)
        return false;
    pixdst d = qd_port_pixels(port);
    if (d.base != rd32(LM_ScrnBase))
        return false;
    *dh = -d.bounds.left;
    *dv = -d.bounds.top;
    return true;
}

static uint32_t rect_key(qrect r, int screen)
{
    uint32_t h = 2166136261u;
    int16_t v[5] = { r.top, r.left, r.bottom, r.right, (int16_t)screen };
    for (int k = 0; k < 5; k++)
        h = (h ^ (uint16_t)v[k]) * 16777619u;
    return h;
}

static bool same_rect(qrect a, qrect b)
{
    return a.top == b.top && a.left == b.left && a.bottom == b.bottom && a.right == b.right;
}

static int find_target(qrect r, int screen)
{
    for (uint32_t h = rect_key(r, screen) & (NHASH - 1);; h = (h + 1) & (NHASH - 1)) {
        int i = target_hash[h] - 1;
        if (i < 0)
            return -1;
        if (targets[i].screen == screen && same_rect(targets[i].r, r))
            return i;
    }
}

static void hash_target(int i)
{
    uint32_t h = rect_key(targets[i].r, targets[i].screen) & (NHASH - 1);
    while (target_hash[h])
        h = (h + 1) & (NHASH - 1);
    target_hash[h] = (int16_t)(i + 1);
}

/* A target seen, on the screen at r (its click at x, y), unless it is one
 * already, off the screen, too small or too big to be a button (a whole
 * window, the screen's background). */
static void add_target(int kind, qrect r, int x, int y, int screen, const char *label)
{
    int w = 0, h = 0;
    machine_screen_size(&w, &h);
    if (r.bottom - r.top < 4 || r.right - r.left < 4 || x < 0 || y < 0 || x >= w || y >= h ||
        4L * (r.bottom - r.top) * (r.right - r.left) > (long)w * h)
        return;
    int i = find_target(r, screen);
    if (i >= 0) {
        if (label && *label && !targets[i].label[0])
            snprintf(targets[i].label, sizeof targets[i].label, "%s", label);
        return;
    }
    if (ntargets == NTARGETS || ntargets >= NHASH / 2)
        return;
    i = ntargets++;
    targets[i].r = r;
    targets[i].x = (int16_t)x;
    targets[i].y = (int16_t)y;
    targets[i].screen = (int16_t)screen;
    targets[i].kind = (uint8_t)kind;
    targets[i].tried = false;
    snprintf(targets[i].label, sizeof targets[i].label, "%s", label ? label : "");
    hash_target(i);
}

void walker_note_hit(gaddr port, int top, int left, int bottom, int right, int x, int y, uint32_t pt)
{
    if (!watching())
        return;
    /* Only tests of where the mouse is (as GetMouse gives it in the
     * port, or on the screen): what the program's buttons are, not where
     * its sprites meet. */
    uint32_t g = ev_mouse_global();
    int dh = 0, dv = 0;
    if (pt != g) {
        if (!port)
            return;
        pixdst d = qd_port_pixels(port);
        if (pt_h(pt) != pt_h(g) + d.bounds.left || pt_v(pt) != pt_v(g) + d.bounds.top)
            return;
        dh = -d.bounds.left;
        dv = -d.bounds.top;
    }
    qrect r = { (int16_t)(top + dv), (int16_t)(left + dh), (int16_t)(bottom + dv), (int16_t)(right + dh) };
    add_target(T_HIT, r, x + dh, y + dv, screening ? on_screen : -1, NULL);
}

/* What is drawn off the screen (into a GWorld, a buffer of the program's
 * own), by the address of the pixels and the place among them: found on
 * the screen when copied there (CopyBits, through one buffer to another
 * too), where most games draw their screens. */
enum { NNOTES = 512 };
static struct {
    gaddr base;
    qrect r;
    uint8_t kind;
    char label[32];
} notes[NNOTES];

/* Text drawn on from the last (a letter at a time, a word at a time, as
 * DrawChar and DrawString go): one label, *r and label grown by it. */
static bool runs_on(qrect *r, char *label, size_t size, qrect s, const char *more)
{
    if (r->top != s.top || r->bottom != s.bottom || s.left < r->right || s.left - r->right > 12 ||
        strlen(label) + strlen(more) + 2 > size)
        return false;
    if (s.left > r->right)
        strcat(label, " ");
    strcat(label, more);
    r->right = s.right;
    return true;
}

/* Something drawn at r among the pixels at base (r relative to their top
 * left), a picture or text (a label): a target if those are the screen's,
 * else noted. */
static void note_at(gaddr base, qrect r, int kind, const char *label)
{
    if (base == rd32(LM_ScrnBase)) {
        int last_i = ntargets - 1;
        if (kind == T_TEXT && last_i >= 0 && targets[last_i].kind == T_TEXT && targets[last_i].screen == -1 &&
            runs_on(&targets[last_i].r, targets[last_i].label, sizeof targets[last_i].label, r, label)) {
            /* Not hashed by its new rectangle: found again at the step. */
            targets[last_i].x = (int16_t)((targets[last_i].r.left + targets[last_i].r.right) / 2);
            return;
        }
        add_target(kind, r, (r.left + r.right) / 2, (r.top + r.bottom) / 2, -1, label);
        return;
    }
    if (kind == T_TEXT && last_note >= 0 && notes[last_note].base == base && notes[last_note].kind == T_TEXT &&
        runs_on(&notes[last_note].r, notes[last_note].label, sizeof notes[last_note].label, r, label))
        return;
    for (int i = 0; i < nnotes; i++)
        if (notes[i].base == base && same_rect(notes[i].r, r)) {
            snprintf(notes[i].label, sizeof notes[i].label, "%s", label ? label : "");
            notes[i].kind = (uint8_t)kind;
            last_note = i;
            return;
        }
    int i = nnotes < NNOTES ? nnotes++ : (last_note + 1) % NNOTES;
    notes[i].base = base;
    notes[i].r = r;
    notes[i].kind = (uint8_t)kind;
    snprintf(notes[i].label, sizeof notes[i].label, "%s", label ? label : "");
    last_note = i;
}

/* Notes among the pixels at base drawn over by something bigger at r
 * (a new screen drawn into the buffer): forgotten. */
static void drawn_over(gaddr base, qrect r)
{
    int k = 0;
    for (int i = 0; i < nnotes; i++) {
        qrect n = notes[i].r;
        bool over = notes[i].base == base && n.top >= r.top && n.left >= r.left && n.bottom <= r.bottom &&
                    n.right <= r.right && !same_rect(n, r);
        if (!over)
            notes[k++] = notes[i];
    }
    if (k != nnotes)
        last_note = -1;
    nnotes = k;
}

/* r in the port's local coordinates, to its pixels'. */
static void note_in_port(gaddr port, qrect r, int kind, const char *label)
{
    if (!port)
        return;
    pixdst d = qd_port_pixels(port);
    if (!d.base)
        return;
    note_at(d.base,
            (qrect){ (int16_t)(r.top - d.bounds.top), (int16_t)(r.left - d.bounds.left),
                     (int16_t)(r.bottom - d.bounds.top), (int16_t)(r.right - d.bounds.left) },
            kind, label);
}

bool walker_watching(void) { return watching(); }

void walker_note_copy(gaddr src, qrect src_bounds, qrect src_r, gaddr dst, qrect dst_bounds, qrect dst_r)
{
    if (!watching() || !nnotes || !src || src == dst)
        return;
    int sl = src_r.left - src_bounds.left, st = src_r.top - src_bounds.top;
    int sw = src_r.right - src_r.left, sh = src_r.bottom - src_r.top;
    int dl = dst_r.left - dst_bounds.left, dt = dst_r.top - dst_bounds.top;
    int dw = dst_r.right - dst_r.left, dh = dst_r.bottom - dst_r.top;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return;
    if (dst != rd32(LM_ScrnBase))
        drawn_over(dst, (qrect){ (int16_t)dt, (int16_t)dl, (int16_t)(dt + dh), (int16_t)(dl + dw) });
    for (int i = 0, n = nnotes; i < n; i++) {
        if (notes[i].base != src)
            continue;
        qrect r = notes[i].r;
        int cx = (r.left + r.right) / 2 - sl, cy = (r.top + r.bottom) / 2 - st;
        if (cx < 0 || cy < 0 || cx >= sw || cy >= sh)
            continue;
        qrect m = { (int16_t)(dt + (r.top - st) * dh / sh), (int16_t)(dl + (r.left - sl) * dw / sw),
                    (int16_t)(dt + (r.bottom - st) * dh / sh), (int16_t)(dl + (r.right - sl) * dw / sw) };
        char label[32];
        snprintf(label, sizeof label, "%s", notes[i].label);
        note_at(dst, m, notes[i].kind, label);
    }
}

void walker_note_text(gaddr port, int top, int left, int bottom, int right, const uint8_t *text, int len)
{
    if (!watching() || len <= 0 || len > 31)
        return;
    char label[32];
    int n = 0;
    bool any = false;
    for (int i = 0; i < len; i++) {
        label[n++] = text[i] >= ' ' && text[i] < 0x7F ? (char)text[i] : '?';
        any |= text[i] > ' ';
    }
    label[n] = 0;
    if (any)
        note_in_port(port, (qrect){ (int16_t)top, (int16_t)left, (int16_t)bottom, (int16_t)right }, T_TEXT, label);
}

/* The screen, as the average colour of each of a grid of cells. */
static void thumb_now(uint8_t *t)
{
    int w = 0, h = 0;
    machine_screen_size(&w, &h);
    memset(t, 0, THUMB_SIZE);
    uint8_t *px = w > 0 && h > 0 ? malloc((size_t)w * (size_t)h * 4) : NULL;
    if (!px)
        return;
    machine_screen_rgba(0, 0, w, h, px);
    for (int cy = 0; cy < THUMB_H; cy++)
        for (int cx = 0; cx < THUMB_W; cx++) {
            unsigned sum[3] = { 0, 0, 0 }, n = 0;
            for (int y = cy * h / THUMB_H; y < (cy + 1) * h / THUMB_H; y += 2)
                for (int x = cx * w / THUMB_W; x < (cx + 1) * w / THUMB_W; x += 2, n++)
                    for (int c = 0; c < 3; c++)
                        sum[c] += px[((size_t)y * (size_t)w + (size_t)x) * 4 + (size_t)c];
            for (int c = 0; c < 3; c++)
                t[(cy * THUMB_W + cx) * 3 + c] = (uint8_t)(n ? sum[c] / n : 0);
        }
    free(px);
}

/* How many cells differ between two pictures of the screen. */
static int thumb_diff(const uint8_t *a, const uint8_t *b)
{
    int n = 0;
    for (int i = 0; i < THUMB_W * THUMB_H; i++)
        for (int c = 0; c < 3; c++)
            if (abs((int)a[i * 3 + c] - (int)b[i * 3 + c]) > 24) {
                n++;
                break;
            }
    return n;
}

/* The same screen: a button lit as the mouse passes, a caret, a clock
 * changing, but no more than a sixteenth of it. */
static bool same_screen(const uint8_t *a, const uint8_t *b)
{
    return thumb_diff(a, b) * 16 <= THUMB_W * THUMB_H;
}

/* Which screen a picture is, one of those seen or a new one. */
static int screen_of(const uint8_t *t)
{
    if (!thumbs && !(thumbs = malloc(NSCREENS * sizeof *thumbs)))
        return 0;
    int best = -1, best_diff = 0;
    for (int i = 0; i < nscreens; i++) {
        int d = thumb_diff(thumbs[i], t);
        if (best < 0 || d < best_diff)
            best = i, best_diff = d;
    }
    if (best >= 0 && (best_diff * 16 <= THUMB_W * THUMB_H || nscreens == NSCREENS))
        return best;
    memcpy(thumbs[nscreens], t, THUMB_SIZE);
    keys_tried[nscreens] = 0;
    moved_from[nscreens] = false;
    swept[nscreens] = false;
    memset(clicked_on[nscreens], 0, sizeof clicked_on[nscreens]);
    return nscreens++;
}

/* What was drawn since the last step, on the screen it was drawn for
 * (s): a duplicate of one there is dropped. */
static void place_drawn(int s)
{
    memset(target_hash, 0, sizeof target_hash);
    for (int i = 0; i < ntargets; i++)
        if (targets[i].screen >= 0)
            hash_target(i);
    for (int i = 0; i < ntargets; i++)
        if (targets[i].screen == -1) {
            bool dup = find_target(targets[i].r, s) >= 0;
            targets[i].screen = (int16_t)(dup ? -2 : s);
            if (!dup)
                hash_target(i);
        }
}

/* The front window's controls and items (a dialog's, the push buttons
 * too), targets of the screen s. */
static void add_window_targets(int s)
{
    gaddr w = tb_FrontWindow();
    if (!w || !WindowRecord_visible(w))
        return;
    bool dialog = dialog_in_front() == w;
    char text[64];
    qrect r;
    int b;
    for (int i = 1; i <= 64 && (b = dialog ? dialog_target(w, i, text, sizeof text, &r)
                                           : window_control(w, i, text, sizeof text, &r)) >= 0; i++)
        if (b == 2)
            add_target(T_CONTROL, r, (r.left + r.right) / 2, (r.top + r.bottom) / 2, s, text);
    for (int i = 1; dialog && i <= 64; i++) {
        int x, y;
        if ((b = dialog_button(w, i, text, sizeof text, &x, &y)) < 0)
            break;
        if (b == 2)
            add_target(T_CONTROL, (qrect){ (int16_t)(y - 4), (int16_t)(x - 4), (int16_t)(y + 4), (int16_t)(x + 4) }, x,
                       y, s, text);
    }
}

static bool inside(int x, int y, qrect r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

/* Target i's label: its own, or the text drawn inside it. */
static const char *target_label(int i)
{
    if (targets[i].label[0])
        return targets[i].label;
    for (int j = 0; j < ntargets; j++)
        if (targets[j].kind == T_TEXT && targets[j].screen == targets[i].screen &&
            inside(targets[j].x, targets[j].y, targets[i].r))
            return targets[j].label;
    return "";
}

/* The next target of screen s to click: none labelled to quit, nor one
 * inside another already clicked there (a picture's text, a hit test's
 * picture), nor one avoided.  One labelled Cancel or Decline (a licence's)
 * only as a last resort, after the keys. */
static int next_target(int s, bool last_resort)
{
    for (int k = 0; k < T_KINDS; k++)
        for (int i = 0; i < ntargets; i++) {
            if (targets[i].screen != s || targets[i].kind != k || targets[i].tried ||
                ((k == T_HOVER || k >= T_DRAWN) && clicked_on[s][k] >= PER_KIND))
                continue;
            char at[24];
            snprintf(at, sizeof at, "%d,%d", targets[i].x, targets[i].y);
            if (!last_resort && declines(target_label(i)))
                continue;
            bool skip = passed_over(target_label(i)) || avoid(at);
            for (int j = 0; !skip && j < ntargets; j++)
                skip = j != i && targets[j].tried && targets[j].screen == s &&
                       inside(targets[i].x, targets[i].y, targets[j].r);
            if (skip) {
                targets[i].tried = true;
                continue;
            }
            return i;
        }
    return -1;
}

/* The mouse moved over the front window (the screen, with none) in
 * steps a little apart, scripted to come as the program runs. */
static void sweep(void)
{
    int w = 0, h = 0;
    machine_screen_size(&w, &h);
    qrect a = { 20, 0, (int16_t)h, (int16_t)w };
    gaddr fw = tb_FrontWindow();
    if (fw && WindowRecord_visible(fw)) {
        pixdst px = qd_port_pixels(fw);
        qrect pr = qrect_get(fw + GrafPort_portRect_off);
        a = (qrect){ (int16_t)(pr.top - px.bounds.top), (int16_t)(pr.left - px.bounds.left),
                     (int16_t)(pr.bottom - px.bounds.top), (int16_t)(pr.right - px.bounds.left) };
    }
    uint64_t at = g_cpu->icount + SWEEP_GAP;
    for (int row = 0; row < SWEEP_H; row++)
        for (int col = 0; col < SWEEP_W; col++, at += SWEEP_GAP) {
            int c = row & 1 ? SWEEP_W - 1 - col : col;
            int x = a.left + (2 * c + 1) * (a.right - a.left) / (2 * SWEEP_W);
            int y = a.top + (2 * row + 1) * (a.bottom - a.top) / (2 * SWEEP_H);
            machine_script_add(at, 1, x < 0 ? 0 : x >= w ? w - 1 : x, y < 0 ? 0 : y >= h ? h - 1 : y, -1);
        }
    /* Steps of the machine's time, at 100 ns an instruction. */
    sweep_steps = (int)((at - g_cpu->icount) * 100 / SETTLE) + 2;
}

/* Whether screen s has keys left to give, those avoided passed over. */
static bool keys_left(int s)
{
    char name[40];
    for (; keys_tried[s] < (moved_from[s] ? NAMED_KEYS : NKEYS); keys_tried[s]++) {
        snprintf(name, sizeof name, "key %s", screen_keys[keys_tried[s]].name);
        if (!avoid(name))
            return true;
    }
    return false;
}

/* One step of the screen walk; false when it is over. */
static bool screen_step(uint64_t now)
{
    if (sweep_steps && --sweep_steps)
        return true;
    uint8_t t[THUMB_SIZE];
    thumb_now(t);
    if (!screening) {
        screening = true;
        screening_since = now;
        say("no menu bar: walking the screen");
    }
    if (watched < WATCH_STEPS) {
        if (watched++ && !same_screen(t, thumb_then))
            self_moving = true;
        memcpy(thumb_then, t, THUMB_SIZE);
        return true;
    }
    if (!same_screen(t, thumb_then) && settling < 3) {
        /* Still changing since the last step (a screen drawn in, a
         * transition): given a few steps to settle. */
        memcpy(thumb_then, t, THUMB_SIZE);
        settling++;
        return true;
    }
    /* A screen that never settles (a game's play) is not moved on by the
     * step for changing either. */
    bool restless = settling == 3 || self_moving;
    settling = 0;
    memcpy(thumb_then, t, THUMB_SIZE);
    int s = screen_of(t);
    place_drawn(s);
    if (acted_on >= 0 && ((s != acted_on && !restless) || ncall_sites > calls_then)) {
        productive++;
        moved_from[acted_on] = true;
    }
    acted_on = -1;
    on_screen = s;
    add_window_targets(s);
    if (++screen_steps > SCREEN_STEPS || now - screening_since >= SCREEN_TIME)
        return false;
    char what[160];
    int i = next_target(s, false);
    if (i < 0 && !swept[s]) {
        swept[s] = true;
        snprintf(what, sizeof what, "the mouse swept over screen %d", s + 1);
        say(what);
        sweep();
        return true;
    }
    if (i < 0 && !keys_left(s))
        i = next_target(s, true);
    if (i >= 0) {
        targets[i].tried = true;
        targets_clicked++;
        clicked_on[s][targets[i].kind]++;
        const char *label = target_label(i);
        snprintf(last, sizeof last, "target%s%s%s at %d,%d", *label ? " \"" : "", label, *label ? "\"" : "",
                 targets[i].x, targets[i].y);
        snprintf(what, sizeof what, "%s (%s, screen %d)", last, target_kinds[targets[i].kind], s + 1);
        say(what);
        acted_on = s;
        calls_then = ncall_sites;
        click(targets[i].x, targets[i].y);
        return true;
    }
    if (keys_left(s)) {
        int k = keys_tried[s]++;
        keys_given++;
        snprintf(last, sizeof last, "key %s", screen_keys[k].name);
        snprintf(what, sizeof what, "%s (screen %d)", last, s + 1);
        say(what);
        acted_on = s;
        calls_then = ncall_sites;
        /* Held a while, as a user's: a program reading the keyboard
         * less often than another part of it (a timer task) takes its
         * picture of it sees it down (Boingo Electro's). */
        machine_post_key(screen_keys[k].ch, screen_keys[k].code, 0, 1, 0);
        machine_script_add(g_cpu->icount + 2000000, 4, screen_keys[k].ch, 0, screen_keys[k].code);
        return true;
    }
    return false;
}

/* The end of a screen walk: walked if any of its steps did something;
 * false if none did (the walk ends as it would have without it). */
static bool screen_walk_over(void)
{
    if (!productive || (!targets_clicked && nscreens < 2))
        return false;
    printf("walk: screen walk finished: %d screens, %u targets clicked, %u keys, %u steps productive\n", nscreens,
           targets_clicked, keys_given, productive);
    fflush(stdout);
    walking = false;
    cpu_halt(g_cpu, "screen walk finished");
    return true;
}

/* The program nudged on from a screen of its own: Return, a click,
 * Escape, and with no menu bar space, an arrow and a digit between.
 * False when it has been nudged enough. */
static bool nudge(void)
{
    if (waits >= WAITS)
        return false;
    int w = 0, h = 0;
    /* A program with no menus (a game) gets the keys a game takes too:
     * space, the arrows, a digit. */
    bool menuless = !menu_bar_count();
    switch (waits++ % 6) {
    case 0:
        if (menuless)
            key(' ', 0x31, 0);
        break;
    case 2:
        if (menuless)
            key(0x1C + (waits / 6) % 4, (int[]){ 0x7B, 0x7C, 0x7E, 0x7D }[(waits / 6) % 4], 0);
        break;
    case 4:
        if (menuless)
            key('1', 0x12, 0);
        break;
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

static uint8_t *screen_now(void)
{
    machine_screen_size(&screen_w, &screen_h);
    uint8_t *px = malloc((size_t)screen_w * (size_t)screen_h * 4);
    if (px)
        machine_screen_rgba(0, 0, screen_w, screen_h, px);
    return px;
}

/* Whether the screen changed since screen_then over more than a caret's
 * blink or a cursor (a box of 64 by 64 pixels at least); it is then the
 * screen now. */
static bool screen_moved_on(void)
{
    int w = screen_w, h = screen_h;
    uint8_t *now = screen_now();
    bool moved = false;
    if (now && screen_then && w == screen_w && h == screen_h) {
        int top = h, left = w, bottom = -1, right = -1;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                size_t k = ((size_t)y * (size_t)w + (size_t)x) * 4;
                if (memcmp(now + k, screen_then + k, 3)) {
                    if (y < top) top = y;
                    if (y > bottom) bottom = y;
                    if (x < left) left = x;
                    if (x > right) right = x;
                }
            }
        moved = bottom - top >= 64 && right - left >= 64;
    }
    free(screen_then);
    screen_then = now;
    return moved;
}

/* The front window's next button not yet pressed, pressed (true); false
 * when there is none left.  A dialog's are its button items, another
 * window's its push button controls.  Those that would end the run are
 * passed over, and at the start, Cancel and Decline (unless the window
 * has a Quit of its own).  *dimmed says whether one that may be pressed
 * later is dimmed for now. */
static bool press_a_button(bool *dimmed)
{
    gaddr w = tb_FrontWindow();
    if (!w || !WindowRecord_visible(w))
        return false;
    bool dialog = dialog_in_front() == w;
    if (dialog && dialog_modal(w))
        return false; /* ModalDialog's: walked as dialogs are */
    char text[64], what[96];
    int x, y, b;
    uint32_t sig = 2166136261u;
    for (int i = 1; i <= 64 && (b = dialog ? dialog_button(w, i, text, sizeof text, &x, &y)
                                           : window_button(w, i, text, sizeof text, &x, &y)) >= 0; i++)
        if (b) {
            for (const char *t = text; *t; t++)
                sig = (sig ^ (uint8_t)*t) * 16777619u;
            sig = (sig ^ (uint32_t)(x << 16 ^ y)) * 16777619u;
        }
    int at = 0;
    while (at < npressed_in && pressed_in[at].sig != sig)
        at++;
    if (at == npressed_in) {
        if (npressed_in == NWINDOWS)
            memmove(&pressed_in[0], &pressed_in[1], (size_t)--npressed_in * sizeof pressed_in[0]);
        at = npressed_in++;
        pressed_in[at].sig = sig;
        pressed_in[at].clicked = 0;
    }
    bool starting = !begun() && !has_quit(w, dialog);
    for (int i = 1; i <= 64 && (b = dialog ? dialog_button(w, i, text, sizeof text, &x, &y)
                                           : window_button(w, i, text, sizeof text, &x, &y)) >= 0; i++) {
        if (!b || (pressed_in[at].clicked & 1ull << (i - 1)) || passed_over(text) || (starting && declines(text)))
            continue;
        if (b == 1) {
            *dimmed = true;
            continue;
        }
        pressed_in[at].clicked |= 1ull << (i - 1);
        pressed++;
        snprintf(last, sizeof last, "button \"%s\"", text);
        snprintf(what, sizeof what, "%s (no menu bar)", last);
        say(what);
        click(x, y);
        return true;
    }
    return false;
}

/* Something the size of a button drawn wholly inside the port, smaller
 * than a quarter of it: not its background, nor a picture scrolled out of
 * sight. */
static bool button_sized(gaddr port, int top, int left, int bottom, int right)
{
    if (!port || bottom - top < 12 || right - left < 12)
        return false;
    qrect pr = qrect_get(port + GrafPort_portRect_off);
    return top >= pr.top && left >= pr.left && bottom <= pr.bottom && right <= pr.right &&
           4L * (bottom - top) * (right - left) <= (long)(pr.bottom - pr.top) * (pr.right - pr.left);
}

void walker_note_shape(gaddr port, int top, int left, int bottom, int right)
{
    if (watching() && button_sized(port, top, left, bottom, right))
        note_in_port(port, (qrect){ (int16_t)top, (int16_t)left, (int16_t)bottom, (int16_t)right }, T_SHAPE, NULL);
}

void walker_note_drawn(gaddr port, int top, int left, int bottom, int right)
{
    if (port && watching()) {
        pixdst d = qd_port_pixels(port);
        if (d.base && d.base != rd32(LM_ScrnBase))
            drawn_over(d.base, (qrect){ (int16_t)(top - d.bounds.top), (int16_t)(left - d.bounds.left),
                                        (int16_t)(bottom - d.bounds.top), (int16_t)(right - d.bounds.left) });
    }
    if (!walking || !button_sized(port, top, left, bottom, right))
        return;
    if (watching())
        note_in_port(port, (qrect){ (int16_t)top, (int16_t)left, (int16_t)bottom, (int16_t)right }, T_DRAWN, NULL);
    for (int i = 0; i < nspots; i++)
        if (spots[i].port == port && spots[i].r.top == top && spots[i].r.left == left && spots[i].r.bottom == bottom &&
            spots[i].r.right == right)
            return;
    if (nspots == NSPOTS)
        memmove(&spots[0], &spots[1], (size_t)--nspots * sizeof spots[0]);
    spots[nspots].port = port;
    spots[nspots].r = (qrect){ (int16_t)top, (int16_t)left, (int16_t)bottom, (int16_t)right };
    spots[nspots++].clicked = false;
}

/* The front window's next picture not yet clicked, clicked (true). */
static bool click_a_picture(void)
{
    gaddr w = tb_FrontWindow();
    if (!w || !WindowRecord_visible(w))
        return false;
    gaddr d = dialog_in_front();
    if (d == w && dialog_modal(w))
        return false;
    for (int i = 0; i < nspots; i++) {
        qrect r = spots[i].r;
        if (spots[i].port != w || spots[i].clicked)
            continue;
        spots[i].clicked = true;
        pictured++;
        picture_waits = 0;
        pixdst px = qd_port_pixels(w);
        int x = (r.left + r.right) / 2 - px.bounds.left, y = (r.top + r.bottom) / 2 - px.bounds.top;
        char what[96];
        snprintf(last, sizeof last, "picture at %d,%d", x, y);
        snprintf(what, sizeof what, "%s (no menu bar)", last);
        say(what);
        click(x, y);
        return true;
    }
    return false;
}

/* The end of a walk that found no menu bar, as the program was. */
static void no_menu_bar(const char *why)
{
    printf("walk: %s\n", why);
    printf("walk: %u items chosen, %u buttons pressed, %u dialogs, %u left open\n", chosen, pressed, dialogs, left);
    fflush(stdout);
    walking = false;
    cpu_halt(g_cpu, "menu walk finished");
}

/* The end of a walk that found nothing to do with no menu bar: a program
 * with nothing on the screen of its own (a background program, a server,
 * a helper waiting for Apple events, or one that never got as far as its
 * first window), or stuck on a screen nothing moved on. */
static void no_menu_bar_found(void)
{
    bool windowless = !tb_FrontWindow() && !machine_screen_mode_wanted();
    printf("walk: no menu bar appeared%s\n", windowless ? ", nor a window" : "");
    fflush(stdout);
    walking = false;
    cpu_halt(g_cpu, windowless ? "menu walk found no menu bar or window" : "menu walk found no menu bar");
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

/* The machine's time to the walk's next step, 0 when it is due or there
 * is no walk: a program asleep in WaitNextEvent is woken by that step as
 * by any input, so its sleep ends there. */
uint64_t walker_next_in(void)
{
    uint64_t now = machine_time_ns();
    if (!walking || !g_cpu || g_cpu->halted || now >= next_at)
        return 0;
    return next_at - now;
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
        int step = *tries < 5 ? (begun() ? opened : starting)[*tries] : 5;
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
            bool modal = dialog_modal(d), dimmed = false, starting = !begun() && !has_quit(d, true);
            for (int i = 1; modal && i <= 64 && (b = dialog_button(d, i, text, sizeof text, &x, &y)) >= 0; i++) {
                if (!b || (open_dialogs[at].clicked & 1ull << (i - 1)))
                    continue;
                if (passed_over(text)) {
                    open_dialogs[at].quits = true;
                    continue;
                }
                if (starting && declines(text))
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
             * as nothing else will change.  (A modeless one after another
             * at the start is walked past too: the splash, then a
             * program's palettes, Fusion's, or its licence, Terrain
             * Builder's.) */
            if (left_open && !begun() && modal) {
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
        if (now - started_at < WARM_UP)
            return;
        if (screening) {
            if (!screen_step(now) && !screen_walk_over())
                no_menu_bar_found();
            return;
        }
        bool game = machine_screen_mode_wanted() != NULL;
        /* The screen at each step from halfway through the first nudging
         * (a screen drawn late, once, as a notice, is not taken for one
         * that goes on drawing), and between them, whether it changed: a
         * game's title answering a key may come back to where it was
         * (Age of Empires' main menu).  Not a window coming or going,
         * nor a modal dialog drawn anew (MediaPresenter's, put up again
         * and again as it asks for a name). */
        if (screen_then || waits == WAITS / 2) {
            gaddr front = tb_FrontWindow();
            if (screen_moved_on() && front == front_then && !(d && dialog_modal(d)))
                screen_moved = true;
            front_then = front;
        }
        if (nudge())
            return;
        bool dimmed = false;
        if (press_a_button(&dimmed)) {
            waits = WAITS; /* nudged enough: the buttons now, a step apart */
            return;
        }
        /* A button dimmed for now: a licence's Accept while it counts
         * down (Terrain Builder's). */
        if (dimmed && button_waits++ < BUTTON_WAITS)
            return;
        /* A game with no menus, in a window or the whole screen, given
         * keys and clicks: walked if it went on drawing, not one stuck on
         * a still screen.  One still, with no buttons, has the pictures
         * in its window clicked (buttons of its own drawing). */
        bool drew_on = (game || tb_FrontWindow()) && screen_moved;
        if (!drew_on && !pressed && click_a_picture()) {
            waits = WAITS;
            return;
        }
        if (pressed) {
            no_menu_bar("the program is a window, its buttons pressed");
            return;
        }
        if (drew_on) {
            no_menu_bar(game ? "the program has the whole screen, it drew on as keys and clicks were given"
                             : "the program has no menus, it drew on as keys and clicks were given");
            return;
        }
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
        /* Perhaps still starting: its window's buttons and its screen are
         * looked at again at each step until PATIENCE has passed, and it
         * is nudged again (a game's title screens one after another, each
         * waiting for a key: Nanosaur 1.1.7's). */
        if (now - started_at < PATIENCE) {
            waits = 0;
            return;
        }
        /* Its pictures clicked, and what the last one opened given time to
         * draw (a window of its own, with pictures of its own): walked. */
        if (pictured) {
            if (picture_waits++ < WAITS)
                return;
            no_menu_bar("the program is a window, the pictures in it clicked");
            return;
        }
        /* Nothing on the screen of its own: a background program (a
         * server, a helper waiting for Apple events), or one that never got
         * as far as its first window. */
        bool windowless = !tb_FrontWindow() && !game;
        /* One whose SIZE says it runs only in the background (Syslogd)
         * has nothing to show: it ran on, and that was its walk. */
        if (windowless && (g_app_size_flags & 0x0400)) {
            no_menu_bar("the program is background-only, it ran on");
            return;
        }
        /* Else its screen is walked, by what it tests the mouse against
         * and draws. */
        if (!screen_step(now) && !screen_walk_over())
            no_menu_bar_found();
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
