/* Public interface to the classic environment, used by the host shells and CLI. */
#ifndef MACHINE_H
#define MACHINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    RUN_LIMIT = 0,   /* instruction limit reached, still runnable */
    RUN_HALTED = 1,  /* the app quit or an error stopped the CPU */
    RUN_DRAINED = 2, /* stopped at a safe point for a snapshot (machine_drain_begin(true)) */
} run_status;

/* Reset everything and set up an empty machine. Returns false on failure. */
bool machine_boot(bool census);
/* Whether Blue Box provides a PowerPC library's export (a routine, or data). */
bool machine_provides_export(const char *name, bool data);
/* Load an application (AppleDouble data-fork path) and prepare to run it. */
bool machine_launch(const char *app_path, char *err, size_t errlen);
/* Whether the run launched is the Finder (file type FNDR). */
bool machine_is_finder(void);
/* Run a fat application's PowerPC code rather than its 68k code. */
void machine_set_prefer_ppc(bool on);
/* The JIT (off by default): translate the program's 68k and PowerPC code
 * to host code; false if this host has no JIT (arm64 only for now).
 * cache_dir, if not NULL, keeps translations between runs (files per
 * application there). */
bool machine_set_jit(bool on, const char *cache_dir);
/* A line per CPU about what the JIT did (empty when it was off). */
void machine_jit_report(char *buf, size_t len);
/* Before launching: a host folder becomes a volume of its own, named name
 * (UTF-8; the folder's name if empty), as the disk an application came on. */
void machine_mount_folder(const char *path, const char *name);
/* An AppleShare volume, mirrored: "tcp:[user[:password]@]host[:port]/Volume"
 * or "atalk:[user[:password]@]Server:AFPServer[@zone]/Volume" (AppleTalk on).
 * Connected when first looked into; false if the specification is wrong. */
bool machine_mount_appleshare(const char *spec, char *err, size_t errlen);
void machine_close_appleshares(void); /* logged out, forgotten */
/* The volumes the program sees (0 the main one), for the host's file
 * panels: the name (UTF-8) and the host folder each lives in. */
int machine_volume_count(void);
bool machine_volume_info(int i, char *name, size_t namesize, char *root, size_t rootsize);
/* A program's own INIT file (an extension or INIT: its 'INIT' resources) to
 * run at startup, before the application's first instruction, as the Start
 * Manager would; the list is kept across boots (startup_reset empties it). */
bool machine_add_init(const char *path);
void startup_reset(void);
int startup_run_init_file(const char *path, char *err, size_t errlen);
/* After Dark graphics modules (afterdark.c).  A module file given to
 * machine_launch is run as the screen saver: Initialize, Blank, then
 * DrawFrame each tick (up to frames_per_tick calls, or until the module has
 * run tick_budget instructions in the tick), and Close when input comes or
 * the seconds are up.  Before machine_launch: */
void machine_screensaver_reset(void);                  /* the options as they start */
void machine_screensaver_set_sound(bool on);          /* off by default */
void machine_screensaver_set_seconds(double seconds); /* 0: until input */
void machine_screensaver_set_pace(int frames_per_tick, uint32_t tick_budget);
/* A PNG of the screen every so many seconds, dir/frame-NNNNN.png. */
void machine_screensaver_set_snapshots(double every, const char *dir);
/* A control's value (0-3, or its resource ID 1000-1003) in place of the
 * one the module's resources hold; false for another index. */
bool machine_screensaver_set_control(int index, int value);
/* What the screen shows when the module starts, as the desktop it saves
 * (Puzzle, Spotlight and Bat Signal work on it): RGBA pixels, scaled to the
 * screen; NULL for Blue Box's own desktop. */
void machine_screensaver_set_backdrop(const uint8_t *rgba, int w, int h);
/* The screen as several displays (After Dark's monitors, the main one
 * first), each a rectangle of the screen: the module gets each in its
 * parameter block's monitor list and blanks them all.  None: one, the
 * whole screen.  False past 16 or for an empty rectangle. */
bool machine_screensaver_add_monitor(int left, int top, int right, int bottom);
/* Setup before it runs, as in After Dark's control panel: the module is
 * told it was chosen (ModuleSelected, if it takes it) and the buttons are
 * pressed in order (a message 8 or more, or a 'bVal' ID 1000 and up). */
bool machine_screensaver_press_button(int button);
/* In a normal run: blank the screen with the module at path after the
 * minutes without input, and wake on input (empty path: none). */
void machine_screensaver_set_idle(const char *path, double minutes);
/* Settings, as After Dark wrote them back into a module's file: the
 * controls given and whatever the module changes in its own resources
 * (at a button, in its setup) are kept in dir, a file a module holding
 * the resources that differ from its own, laid over them when it runs
 * again ("" or NULL: not kept).  The module file isn't written. */
void machine_screensaver_set_settings_dir(const char *dir);
/* The file a module's settings are kept in (false if none is set). */
bool machine_screensaver_settings_path(const char *module, char *out, size_t outsize);
/* The module's resource fork with its settings over it (malloc'd), and
 * one resource stored in its settings (the screen saver's Options sheet). */
uint8_t *machine_screensaver_module_fork(const char *module, size_t *len);
bool machine_screensaver_store_resource(const char *module, uint32_t type, int16_t id, const uint8_t *data,
                                        uint32_t len);
/* Modules in turn, as After Dark's Randomize ran them: the one launched
 * (or the idle one) and each added, every so many seconds, in order or at
 * random (each closed, its settings kept, and the next begun). */
bool machine_screensaver_add_module(const char *path);
void machine_screensaver_set_rotation(double seconds, bool random);
/* What happened: DrawFrame calls, calls by message (8: buttons), and the
 * message the module stopped with ("" if it didn't). */
uint32_t machine_screensaver_frames(void);
uint32_t machine_screensaver_calls(int message);
const char *machine_screensaver_error(void);
/* A raw floppy disk image (a sector dump of 400K/720K/800K/1440K, or Disk
 * Copy 4.2) as the disk in the Sony driver's drive 1: a key disk whose
 * sectors the program's own copy protection reads (sony.c). */
bool machine_set_floppy(const char *path, char *err, size_t errlen);
bool sony_has_disk(void);
uint32_t sony_disk_size(void);
bool sony_sector_tags(uint32_t sector, uint8_t out[12]);
/* The debugger (debugger.c), a MacsBug that is there: DEBUGGER_LOG (the
 * default) logs each Debugger, DebugStr and SysBreak and goes on, as "g"
 * in MacsBug; DEBUGGER_HALT stops the run there; DEBUGGER_QUIET only
 * counts them.  BLUEBOX_DEBUGGER=halt or quiet when none is set.  The
 * run's count, the places it came from, the last message, and a summary
 * line (empty if it was never entered). */
enum { DEBUGGER_LOG, DEBUGGER_HALT, DEBUGGER_QUIET };
void machine_set_debugger(int mode);
uint64_t machine_debugger_entries(void);
unsigned machine_debugger_places(void);
const char *machine_debugger_last(void);
void machine_debugger_summary(char *out, size_t n);
/* Run up to max_instructions. */
run_status machine_run(uint64_t max_instructions);
const char *machine_halt_reason(void);
/* BLUEBOX_68K_TRACE=n: print the last n 68k instructions run (a fault does). */
void machine_dump_68k_trace(void);
uint64_t machine_instruction_count(void);
uint32_t machine_pc(void);

/* Screen size in pixels (call before machine_boot; default 1024x768). */
void machine_set_screen(int width, int height);
/* Open the fonts (FOND/NFNT/FONT) of a resource file below the application.
 * Call after machine_boot and before machine_launch. */
bool machine_add_resource_file(const char *path, char *err, size_t errlen);
/* The screen, for the host to display: its size, and any rectangle as RGBA. */
void machine_screen_size(int *width, int *height);
/* Why the program wants the whole screen shown (it draws outside its
 * windows, hides the menu bar ...), or NULL while rootless suits it. */
const char *machine_screen_mode_wanted(void);
/* The host's double-click, caret-blink and key-repeat times, in ticks,
 * for the low-memory settings at boot (0 keeps a default). */
void machine_set_input_timing(int double_time, int caret_time, int key_thresh, int key_rep);
/* A low-memory setting by name (DoubleTime, SdVolume, DlgFont ...: real
 * settings only, not the machine's own state), applied at each boot;
 * NULL forgets them all. */
bool machine_set_lowmem(const char *name, uint32_t value);
void machine_screen_rgba(int x, int y, int w, int h, uint8_t *out);
/* Whether that part of the screen shows something else since the copy in
 * shadow (machine_screen_shadow_size bytes; any other size counts as
 * changed) was taken, the copy brought up to date: a host refreshing its
 * windows every frame converts the pixels only when they changed. */
size_t machine_screen_shadow_size(int x, int y, int w, int h);
bool machine_screen_changed(int x, int y, int w, int h, uint8_t *shadow, size_t len);
/* Window, control and menu appearance; the colour ones use an 8-bit screen.
 * Call before boot. */
enum { MACHINE_APPEARANCE_BW, MACHINE_APPEARANCE_THEME, MACHINE_APPEARANCE_NATIVE };
void machine_set_appearance(int appearance);
/* The highlight colour (HiliteRGB) from boot on: the host's selection
 * colour, for selected text and list rows to match the host. */
void machine_set_hilite_color(uint16_t r, uint16_t g, uint16_t b);
/* The host's accent colour, for the Appearance Manager's (scroll bar
 * thumbs, focus rings, selected menu items).  Both may be set from any
 * thread while the machine runs: they take effect at its next event call. */
void machine_set_accent_color(uint16_t r, uint16_t g, uint16_t b);
/* Use a Kaleidoscope 2 scheme file (AppleDouble) for windows and controls;
 * implies MACHINE_APPEARANCE_THEME and a 32-bit screen.  Before boot. */
bool machine_load_theme(const char *path, char *err, size_t errlen);
/* A Mac OS 8.5 Appearance theme file's parts, as text on stdout (with the
 * layouts' coordinates if asked), or drawn off-screen into a PNG: a sheet
 * of them all, or one ('layo' part, pw x ph) in each state bit. */
bool machine_theme_dump(const char *path, bool coords, char *err, size_t errlen);
/* A theme's sound set ('tsnd' file), for the Appearance sounds; and the
 * name the loaded theme gives its own. */
bool machine_load_theme_sounds(const char *path, char *err, size_t errlen);
bool machine_theme_sound_set_name(char *out, size_t n);
/* The menu bar's height in the appearance chosen (a theme's own, else 20). */
int machine_menu_bar_height(void);
bool machine_theme_render(const char *path, const char *png, int part, int pw, int ph, char *err, size_t errlen);
/* Screen depth: 1 (black and white, the default), 8 or 32; before boot. */
void machine_set_screen_depth(int depth);
/* The depth the next boot gives the screen (machine_screen_depth is the one it has). */
int machine_screen_depth_setting(void);
/* The screen's depth now (a program may have changed it). */
int machine_screen_depth(void);
/* The processor: 68040 (the default), 68030, 68020 or 68000.  Before
 * machine_boot. */
void machine_set_cpu(int model);
/* Its FPU: 0 (none), 68881, 68882 or 68040 (the 68040's own), where the
 * processor could have it; -1 (the default) gives the model its usual
 * one: the 68040's own, a 68882 with a 68030, none with a 68020 or 68000.
 * Before machine_boot. */
void machine_set_fpu(int fpu);
/* Addressing: 24 or 32 bits (0, the default: 32, but always 24 on a
 * 68000).  Before machine_boot. */
void machine_set_addressing(int bits);
/* What an application asks for: 32 if its SIZE resource says it is
 * 32-bit clean (or it has PowerPC code), else 24. */
int machine_app_addressing(const char *app_path);
/* The machine as booted: processor, FPU (as above), addressing mode, the
 * Gestalt machine type and MMU type standing for them. */
int machine_cpu_model(void);
int machine_fpu_model(void);
int machine_addressing(void);
int machine_gestalt_type(void);
int machine_mmu_type(void);
/* SysEnvirons' machine type and HWCfgFlags for the same machine. */
int machine_env_type(void);
int machine_hwcfg(void);
bool machine_halted(void);
/* With depth alerts on, an alert saying the program needs another screen
 * depth ends the run instead of showing ("asks for depth N"), and
 * machine_wanted_depth() gives N, for the host to start it again at. */
void machine_set_depth_alerts(bool on);
int machine_wanted_depth(void);
/* The depths an alert's text says a program runs at, as bits 1 << depth. */
uint64_t machine_alert_depths(const char *message);
/* Write the screen to a PNG file. */
bool machine_screenshot(const char *path);
bool machine_write_png(const char *path, const uint8_t *rgba, int w, int h);
/* The same keeping the alpha (icons' masks). */
bool machine_write_png_alpha(const char *path, const uint8_t *rgba, int w, int h);

/* ---- Resources rendered for the host (bluebox inspect) ----
 * Both need a booted machine (machine_boot) and leave it usable. */

/* Draw a 'PICT' resource's data with QuickDraw into a 32-bit offscreen
 * the size of its frame: *rgba (malloc'd, w*h*4 bytes, opaque). */
bool machine_picture_rgba(const uint8_t *pict, size_t len, uint8_t **rgba, int *w, int *h);
/* Decode the sampled sound of a 'snd ' resource (format 1 or 2, a standard,
 * extended or compressed header) to 16-bit PCM, interleaved when stereo:
 * *pcm malloc'd; bits is the sound's own sample size (8 or 16). */
bool machine_snd_pcm(const uint8_t *snd, size_t len, int16_t **pcm, uint32_t *frames, int *channels,
                     double *rate, int *bits);
/* The redraw check: repaint everything (every window's content erased and
 * queued for update), and go on running after the instruction limit. */
void machine_redraw_all(void);
void machine_continue(uint64_t more);

/* Set a Gestalt selector; gestalt_override's answers (the run's own,
 * before launch) also stand over a PowerPC program's. */
void gestalt_set(uint32_t selector, uint32_t value);
void gestalt_override(uint32_t selector, uint32_t value);
/* Logging mask (see log.h) and destination. */
void machine_set_log(unsigned mask);

/* Host input (thread-safe).  Coordinates are global screen pixels;
 * button: 1 down, 0 up, -1 unchanged. */
void machine_post_mouse(int x, int y, int button);
/* Mouse motion while the host has captured the mouse, in Mac pixels. */
void machine_post_mouse_delta(double dx, double dy);
/* The host's scroll wheel at (x, y), in steps (lines): dy > 0 up, dx > 0
 * left.  Each is a click in an arrow of the scroll bar there. */
void machine_post_wheel(int x, int y, int dx, int dy);
/* The mouse's position; the count of the program's own moves of it; and
 * whether it is reading the mouse by recentring it (moved it lately). */
uint32_t machine_mouse_state(int *x, int *y, bool *relative);
void machine_release_keys(void); /* all keys up: the host lost the keyboard */
void machine_post_key(int charcode, int keycode, unsigned modifiers, int down, int repeat);
/* Scripted input for headless runs: kind 0 a key (a = character, b =
 * modifiers), 1 the mouse (a, b = position, c = button: 1 down, 0 up,
 * -1 unchanged), 2 a menu choice (a = menu ID, b = item), applied once the
 * instruction count reaches at. */
void machine_script_add(uint64_t at, int kind, int a, int b, int c);
/* A scripted screenshot at an instruction count (snap:F.png, headless). */
void machine_script_add_snap(uint64_t at, const char *path);
/* Tell the Event Manager a host is attached (it may then sleep for input). */
void machine_attach_host(bool attached);

/* ---- Rootless presentation (snapshots published by the emulator thread) ---- */

/* The Mac's whole screen shown in one window: with the native appearance,
 * the Window Manager draws the windows' frames again (at the next event). */
void machine_drop_native_frames(void);
enum { MACHINE_WINDOW_NATIVE = 1, MACHINE_WINDOW_CLOSE = 2, MACHINE_WINDOW_ZOOM = 4, MACHINE_WINDOW_GROW = 8 };

typedef struct {
    uint32_t id;                 /* stable for the window's lifetime */
    int16_t top, left, bottom, right; /* structure bounds, Mac global coordinates */
    bool active;
    uint8_t flags;               /* MACHINE_WINDOW_*: native frame and its controls */
    char title[256];             /* MacRoman */
} machine_window;

/* With the native appearance, the host draws document windows' frames;
 * its title-bar actions reach the app as the classic clicks would (the app
 * calls TrackGoAway, TrackBox, DragWindow or GrowWindow and gets the
 * host's result).  x, y: a point in the window's content (global). */
enum { MACHINE_ACTION_CLOSE = 1, MACHINE_ACTION_ZOOM, MACHINE_ACTION_MOVE, MACHINE_ACTION_RESIZE };
/* MOVE: a, b = the content's new left, top; RESIZE: a, b = new width, height. */
void machine_window_action(uint32_t id, int action, int a, int b, int x, int y);

/* Changes whenever the window list or a window's shape or position changes. */
uint32_t machine_windows_version(void);
/* Visible windows, front to back. */
int machine_windows(machine_window *out, int max);
/* The window's shape: width*height bytes, 1 inside.  False if it changed. */
bool machine_window_mask(uint32_t id, uint8_t *out, int width, int height);
/* What of the screen from left, top is not desktop (width*height bytes, 1
 * to show, 0 where the Window Manager's desktop is still as it painted it,
 * outside every window).  False if no desktop was painted. */
bool machine_desktop_mask(int left, int top, int width, int height, uint8_t *out);

typedef struct {
    char text[64];               /* MacRoman */
    bool enabled, separator;
    uint8_t key, mark, style;
    uint8_t modifiers;           /* Mac OS 8's: 1 Shift, 2 Option, 4 Control, 8 no Command */
} machine_menu_item;

typedef struct {
    int16_t id;
    char title[64];              /* MacRoman */
    bool enabled;
    int nitems;
    machine_menu_item items[64];
} machine_menu;

/* The cursor as the host should show it: hidden (HideCursor, or the mouse
 * in a ShieldCursor rectangle), the arrow or a 16x16 image (data and mask
 * bits, rows of two bytes, the hot spot), and a count of ObscureCursor
 * calls (hide until the mouse moves).  The result changes with it. */
/* Which cursor it is, so the host can show its own: the arrow, one of the
 * System's (CURS 1 to 4), or an image of the application's. */
enum {
    MACHINE_CURSOR_CUSTOM,
    MACHINE_CURSOR_ARROW,
    MACHINE_CURSOR_IBEAM,
    MACHINE_CURSOR_CROSS,
    MACHINE_CURSOR_PLUS,
    MACHINE_CURSOR_WATCH,
};
typedef struct {
    bool hidden, arrow;
    int kind;
    uint8_t data[32], mask[32];
    int16_t hot_h, hot_v;
    uint32_t obscures;
    bool color;          /* a colour cursor: rgba holds its image */
    uint8_t rgba[16 * 16 * 4];
} machine_cursor;
uint32_t machine_cursor_state(machine_cursor *out);

uint32_t machine_menus_version(void);
/* Drawing outside the windows that must show above them (a pop-up menu):
 * its rectangle (top, left, bottom, right) and whether it is up; the
 * result changes whenever it does. */
uint32_t machine_transient(int16_t rect[4], bool *on);
/* What the system's own code (an INIT's, a desk accessory's resident code
 * in the system heap: the Talking Moose) drew on the screen outside the
 * windows, which a rootless host shows floating over its desktop: its
 * rectangle and whether it is there (it goes when what was under it is
 * put back); the result changes when either does.  The pixels come as
 * RGBA, clear where that code didn't draw. */
uint32_t machine_overlay(int16_t rect[4], bool *on);
void machine_overlay_rgba(int x, int y, int w, int h, uint8_t *out);
/* The user typed or clicked in another host application: KeyTime moves,
 * so code waiting for the Mac to be left alone (the Talking Moose) waits
 * on.  For a program run in the background. */
void machine_note_user_activity(void);
int machine_menus(machine_menu *out, int max);
/* Whether the menu bar's menus (as last published) have an item whose key
 * is Command and this character, in either case, enabled or not: the
 * program's own Command-H, which MenuKey finds with Option held too. */
bool machine_menu_bar_has_command_key(int ch);
/* Deliver a choice made in the host's menu bar (item is 1-based). */
void machine_choose_menu(int16_t menu_id, int16_t item);
/* A host menu opened: the app adjusts its menus (waits briefly for that,
 * then the menu snapshot is current).  End when the menu closes, after any
 * choice was delivered. */
void machine_menu_tracking_begin(void);
void machine_menu_tracking_end(void);

/* ---- Alerts (shown by the host) ---- */

enum { MACHINE_ALERT_PLAIN, MACHINE_ALERT_STOP, MACHINE_ALERT_NOTE, MACHINE_ALERT_CAUTION };

typedef struct {
    int16_t item;                /* DITL item number */
    char title[64];              /* MacRoman */
    bool is_default;
} machine_alert_button;

typedef struct {
    int kind;
    char message[1024];          /* MacRoman, CR between paragraphs */
    int16_t default_item;
    int nbuttons;
    machine_alert_button buttons[8];
} machine_alert;

/* Called on the emulator thread; returns the item chosen (or 0 for the
 * default).  The handler blocks until the user answers. */
typedef int (*machine_alert_fn)(const machine_alert *alert, void *ctx);
void machine_set_alert_handler(machine_alert_fn fn, void *ctx);

/* ---- AppleScript's dialogs (shown by the host) ----
 * display dialog and choose from list, from a script our AppleScript runs:
 * the host shows them as native dialogs and gives back the answer; only
 * text goes back and forth.  Without a handler (headless runs) the default
 * button and the default answer are taken. */

enum { MACHINE_SCRIPT_DIALOG, MACHINE_SCRIPT_LIST };

typedef struct {
    int kind;                    /* MACHINE_SCRIPT_DIALOG or MACHINE_SCRIPT_LIST */
    char message[2048];          /* MacRoman, CR between lines; a list's prompt */
    int icon;                    /* MACHINE_ALERT_PLAIN, _STOP, _NOTE or _CAUTION */
    int nbuttons;                /* 1 to 3, left to right (a list: OK and Cancel) */
    char buttons[3][64];         /* MacRoman */
    int default_button;          /* 1-based; 0 for none */
    int cancel_button;           /* 1-based; 0 for none */
    bool has_answer;             /* a text field, answer its default text ... */
    char answer[1024];           /* ... and what was typed, on return (MacRoman) */
    int giving_up;               /* seconds until the dialog answers itself; 0: never */
    int nitems;                  /* a list's items (MacRoman) */
    const char *const *items;
    int selected;                /* a list: the item chosen (0-based), in: the default (-1 none) */
} machine_script_dialog;

/* Called on the emulator thread; returns the button chosen (1-based), or
 * 0 when the dialog gave up.  The handler blocks until the user answers. */
typedef int (*machine_script_dialog_fn)(machine_script_dialog *dialog, void *ctx);
void machine_set_script_dialog_handler(machine_script_dialog_fn fn, void *ctx);

/* ---- Standard File (open and save panels shown by the host) ---- */

typedef struct {
    int16_t top, left, bottom, right;
} mprect;

/* An item the app added to its Standard File dialog (shown by the host
 * next to its panel).  Rectangles are the app's own dialog coordinates. */
enum { MACHINE_ITEM_BUTTON, MACHINE_ITEM_CHECKBOX, MACHINE_ITEM_RADIO, MACHINE_ITEM_TEXT, MACHINE_ITEM_EDIT,
       MACHINE_ITEM_ICON, MACHINE_ITEM_IMAGE };
typedef struct {
    int16_t item;
    int kind;
    char title[256];             /* UTF-8: control title or text */
    int16_t value;
    bool enabled, visible;
    mprect rect;
    uint8_t icon[256];           /* ICON: 32x32, 1 bit (128 bytes); icon_mask: second half */
    bool has_icon;
    /* IMAGE: what the app draws in a user item (its own icons and labels),
     * RGBA, the dialog's white made clear; valid until the next request. */
    const uint8_t *image;
    int image_width, image_height;
} machine_file_item;

typedef struct {
    bool save;
    bool folders;                /* open: choose a folder, not a file */
    char prompt[256];            /* UTF-8; may be empty */
    char default_name[256];      /* UTF-8, for save */
    uint32_t types[16];          /* for open; none means any file */
    int ntypes;
    char directory[1024];        /* where to start; empty for the host's choice */
    int nitems;                  /* the app's own extra items */
    machine_file_item items[16];
} machine_file_request;

/* Called on the emulator thread; blocks until the user answers.  Returns
 * false if cancelled, else the chosen host path. */
/* For each file an open panel would offer: whether the application's file
 * filter hides it (call while the emulator waits in the panel request). */
/* Run the application's dialog hook with a null event (call periodically
 * while the panel is up, as item clicks are). */
void machine_file_idle(void);
bool machine_file_filtered(const char *path);
typedef bool (*machine_file_fn)(const machine_file_request *rq, char *path, size_t pathsize, void *ctx);
void machine_set_file_handler(machine_file_fn fn, void *ctx);
/* While the panel is up (callable from the host's main thread): the user
 * clicked an extra item, or edited one (text is UTF-8, or NULL).  Runs the
 * app's dialog hook; returns 0, or 1 / 2 if the app asks to save (open) or
 * cancel.  machine_file_items then gives the items' new states. */
int machine_file_item_hit(int16_t item, const char *text);
int machine_file_items(machine_file_item *out, int max);
/* Classic file type of a host file (for filtering open panels). */
uint32_t machine_file_type(const char *path);

/* ---- Printing (pages recorded from QuickDraw; the host renders them) ---- */

enum {
    MACHINE_PRINT_RECT, MACHINE_PRINT_OVAL, MACHINE_PRINT_RRECT, MACHINE_PRINT_ARC, MACHINE_PRINT_REGION,
    MACHINE_PRINT_LINE, MACHINE_PRINT_TEXT, MACHINE_PRINT_IMAGE,
};
enum { MACHINE_VERB_FRAME, MACHINE_VERB_PAINT, MACHINE_VERB_ERASE, MACHINE_VERB_INVERT, MACHINE_VERB_FILL };

/* One drawing operation, in device coordinates (72 per inch, 0,0 at the top
 * left of the printable area). */
typedef struct {
    int kind, verb;
    mprect r;                    /* shapes */
    int16_t a, b;                /* round rect: oval width, height; arc: start, angle (degrees) */
    int16_t h0, v0, h1, v1;      /* line ends; text: h0, v0 is the baseline start */
    int16_t pen_w, pen_h;
    uint8_t pattern[8];          /* the pattern drawn with (rows, high bit leftmost) */
    int mode;                    /* QuickDraw transfer mode */
    bool fg_black;               /* foreground colour is dark */
    int nclip;
    mprect *clip;                /* the clip, as rectangles */
    int nrects;
    mprect *rects;               /* region shapes, as rectangles */
    int16_t font_id;             /* text */
    char font_name[64];          /* MacRoman */
    int16_t size, width;         /* point size; the run's width with classic metrics */
    uint8_t face;
    int len;
    uint8_t *text;               /* MacRoman */
    double angle;                /* text: degrees clockwise about rot_h, rot_v (TextBegin) */
    double rot_h, rot_v;
    bool flip_h, flip_v;         /* text: mirrored about the same point */
    int image_w, image_h;        /* images: RGBA pixels shown in r */
    uint8_t *image;
} machine_print_op;

typedef struct {
    int nops;
    machine_print_op *ops;
} machine_print_page;

typedef struct {
    int npages;
    const machine_print_page *pages;
    mprect page;                 /* printable area (rPage) */
    mprect paper;                /* paper, relative to the printable area (rPaper) */
    int copies;
    int vres, hres;              /* the drawing's resolution, dpi (72: points) */
    bool save_pdf;               /* the classic job dialog's Save as PDF: a save panel */
    /* Where the Chooser sends it: the print panel, a host printer by name
     * (no panel), PDF files in a folder (no panel). */
    int destination;             /* MACHINE_PRINT_TO_... */
    char printer[256];           /* MACHINE_PRINT_TO_HOST: the host printer's name (UTF-8) */
    char folder[1024];           /* MACHINE_PRINT_TO_PDF: the folder */
    char title[256];             /* the document: the front window's title (UTF-8), or empty */
} machine_print_job;
enum { MACHINE_PRINT_TO_PANEL, MACHINE_PRINT_TO_HOST, MACHINE_PRINT_TO_PDF };

/* Page setup as the host's page layout panel describes it, in points. */
typedef struct {
    double paper_width, paper_height;
    double left, top, right, bottom;   /* imageable area within the paper */
    bool landscape;
} machine_page_setup;

/* Print the job (show the host's print panel); blocks until done. */
typedef bool (*machine_print_fn)(const machine_print_job *job, void *ctx);
/* Run the host's page setup panel, starting from and updating ps; false if cancelled. */
typedef bool (*machine_page_setup_fn)(machine_page_setup *ps, void *ctx);
void machine_set_print_handlers(machine_print_fn print, machine_page_setup_fn page_setup, void *ctx);
/* ---- The Chooser (chooser.c) ----
 * Its choices are the run's settings: before boot, the user name (UTF-8;
 * the System's 'STR ' -16096), the printer and the folder PDF files go to.
 * A printer is "panel" (the host's print panel, the default), "pdf" (a PDF
 * file in the print folder), "host:NAME" (the host's printer NAME, without
 * a panel), "laserwriter:NAME@ZONE" (a LaserWriter on AppleTalk, over PAP)
 * or "driver:FILE" (a Chooser extension's printer in the System Folder,
 * printed through the panel); false if the spec is none of these. */
void machine_set_user_name(const char *utf8);
bool machine_set_printer(const char *spec);
void machine_set_print_folder(const char *path);
/* The host's side: its printers' names (UTF-8, up to max; the count), and
 * a setting the Chooser changed ("user-name", "printer", "print-folder",
 * or "appletalk": "off" or the mode it is on in), for the host to keep. */
typedef int (*machine_printers_fn)(char (*names)[256], int max, void *ctx);
typedef void (*machine_setting_fn)(const char *key, const char *value, void *ctx);
void machine_set_chooser_host(machine_printers_fn printers, machine_setting_fn changed, void *ctx);
/* Open the Chooser now, as its Apple menu item does; its refnum, or 0.
 * (A desk accessory file whose DRVR is empty stands for the System's desk
 * accessory of that name: `bluebox chooser` runs one with the desk
 * accessory host.) */
int machine_open_chooser(void);

/* A print job as a PostScript program (what a LaserWriter is sent),
 * malloc'd; its length in *len. */
char *machine_postscript(const machine_print_job *job, size_t *len);

/* The host's default paper, used for new print records. */
void machine_set_page_defaults(const machine_page_setup *ps);

/* One of the System file's standard resources (cursors, generic icons
 * ...), or NULL. */
const uint8_t *machine_system_resource(uint32_t type, int16_t id, uint32_t *len);

/* ---- Speech (the Speech Manager's host side) ---- */

enum {
    MACHINE_SPEECH_SPEAK,    /* speak text (UTF-8, with classic embedded commands) */
    MACHINE_SPEECH_STOP,
    MACHINE_SPEECH_PAUSE,
    MACHINE_SPEECH_CONTINUE,
    MACHINE_SPEECH_DISPOSE,  /* the channel is gone */
    MACHINE_SPEECH_PHONEMES, /* text to phonemes, into result */
};
typedef struct {
    int channel, op;
    const char *voice;       /* the classic voice asked for (Fred, Victoria ...); hosts speak with the system voice */
    const char *host_voice;  /* a host voice by name to speak with instead (MacinTalk's robotic voice: Fred), or NULL */
    double rate;             /* words per minute */
    double pitch;            /* base pitch, as SetSpeechPitch's (about 30 to 65) */
    double modulation, volume;
    const char *text;        /* phonemes given as input are words by now */
    char *result;            /* MACHINE_SPEECH_PHONEMES: the phonemes, UTF-8 */
    size_t result_size;
} machine_speech_request;
/* Called on the emulator thread; must not block for the utterance. */
typedef void (*machine_speech_fn)(const machine_speech_request *rq, void *ctx);
void machine_set_speech_handler(machine_speech_fn fn, void *ctx);
/* From the host, on any thread: kind 0 done, 1 word (a = offset in the
 * text, b = length), 2 sync (a = message), 3 phoneme (a = opcode). */
void machine_speech_event(int channel, int kind, int32_t a, int32_t b);
/* The record backend: requests logged to path ("-" for stderr), words,
 * syncs and done delivered at once, no sound.  For tests and probes. */
bool machine_set_speech_record(const char *path);
/* The clock (seconds) the made-up phonemes are timed by; NULL: the host's.
 * For tests. */
void machine_set_speech_clock(double (*fn)(void));

/* ---- Native controls (the native appearance) ---- */

enum {
    MACHINE_CONTROL_BUTTON,
    MACHINE_CONTROL_CHECKBOX,
    MACHINE_CONTROL_RADIO,
    MACHINE_CONTROL_SCROLLBAR,
    MACHINE_CONTROL_POPUP,     /* a pop-up menu's box: title is the current item */
    MACHINE_CONTROL_EDITFRAME, /* the frame round an edit-text item; on: it has the focus */
};
typedef struct {
    int kind;
    int width, height;
    bool pressed, disabled, on;  /* buttons: pressed; check/radio: on */
    bool vertical;               /* scroll bars */
    double position, proportion; /* scroll bars: 0..1, and the visible part (0 when nothing to scroll) */
    int pressed_part;            /* scroll bars: 20 up arrow, 21 down, 22/23 page, 129 thumb */
    char title[256];             /* buttons: UTF-8 */
    /* Scroll bars, set by the host: where its knob and the knob's slot are
     * along the bar, in pixels from its top or left (knob_hi 0: no knob). */
    int knob_lo, knob_hi, slot_lo, slot_hi;
} machine_control_request;
/* Fill rgba (width*height*4, transparent where nothing is drawn), and for
 * a scroll bar its knob geometry; false to fall back to the classic look.
 * Called on the emulator thread. */
typedef bool (*machine_control_fn)(machine_control_request *rq, uint8_t *rgba, void *ctx);
void machine_set_control_renderer(machine_control_fn fn, void *ctx);

/* ---- Threads ----
 * Guest threads run on host threads of their own, one at a time.  While
 * the process's main thread (a headless run) waits for its turn it calls
 * this every few milliseconds, to serve work other threads hand it (the
 * main dispatch queue). */
void machine_set_main_thread_pump(void (*fn)(void));

/* ---- Launching other applications ----
 * LaunchApplication: start the classic application at `path` (a host path)
 * as a separate run.  Returns false if it could not be started. */
/* Start another application (with the documents it is to open). */
typedef bool (*machine_launch_fn)(const char *path, const char *const *documents, int ndocuments, void *ctx);
void machine_set_launch_handler(machine_launch_fn fn, void *ctx);
/* Called by the handler with the process ID of the run it started (none
 * if it only reported the launch): LaunchApplication answers with that
 * run's serial number, and events sent to it wait for it to listen. */
void machine_launched_process(int pid);
/* Open a URL (Internet Config's ICLaunchURL): in the host's own browser or
 * whatever handles its scheme.  Returns false if it could not. */
typedef bool (*machine_url_fn)(const char *url, void *ctx);
void machine_set_url_handler(machine_url_fn fn, void *ctx);
/* An Internet Config preference every program reads (the Internet
 * settings): key as Internet Config names it ("Email", "UseHTTPProxy",
 * "HTTPProxyHost"), data in its format (a Pascal string in MacRoman, a
 * Boolean byte, a STR# list).  Set before or while the machine runs. */
/* bluebox movie-speed: a movie's pictures and sound decoded by our own
 * decoders back to back; false (and why) when they don't play it. */
typedef struct {
    uint32_t length_ms;          /* the movie's length */
    uint32_t frames;             /* distinct pictures */
    int width, height;
    bool video, sound;
    double decode_ms;            /* the time decoding all of it took */
} machine_movie_speed_result;
bool machine_movie_speed(const char *path, machine_movie_speed_result *r, char *err, size_t errlen);
void machine_set_internet_pref(const char *key, const uint8_t *data, int32_t len);

/* ---- Color Picker ----
 * GetColor: show `prompt`, start from `in` (16-bit RGB), and on OK write the
 * choice to `out` and return true. */
typedef bool (*machine_color_fn)(const char *prompt, const uint16_t in[3], uint16_t out[3], void *ctx);
void machine_set_color_picker(machine_color_fn fn, void *ctx);

/* ---- Notifications ---- */

enum { MACHINE_NOTIFY_MARK = 1, MACHINE_NOTIFY_ICON = 2 };
/* A notification request (id > 0) with its message (UTF-8, may be empty)
 * and MACHINE_NOTIFY_* parts; id < 0 withdraws request -id. */
typedef void (*machine_notify_fn)(int id, const char *text, int flags, void *ctx);
void machine_set_notification_handler(machine_notify_fn fn, void *ctx);

/* ---- Sound ---- */

/* Play 16-bit PCM (interleaved when stereo) on a channel; pcm NULL means
 * stop that channel.  Called on the emulator thread; must not block. */
typedef void (*machine_sound_fn)(int channel, const int16_t *pcm, int frames, int channels, double rate, float volume,
                                 void *ctx);
void machine_set_sound_handler(machine_sound_fn fn, void *ctx);

/* MIDI for the host's General MIDI synthesizer (QuickTime music tracks):
 * each message to be played delay_ms from now, on a synthesizer of the
 * caller's id; n 0 means silence it at once (all notes off). */
typedef struct {
    int32_t delay_ms;
    uint8_t status, data1, data2;
} machine_midi_event;
typedef void (*machine_midi_fn)(int id, const machine_midi_event *events, int n, void *ctx);
void machine_set_midi_handler(machine_midi_fn fn, void *ctx);
/* The MIDI Manager's host client: what programs send its input port, a
 * whole message at a time (System Exclusive whole, F0 to F7, up to 4K),
 * for the host's MIDI destinations.  Return false when there are none, and
 * channel messages play on the General MIDI synthesizer instead (the MIDI
 * handler above, id 'MI' << 16).  Called on the emulator thread. */
typedef bool (*machine_midi_out_fn)(const uint8_t *bytes, int n, void *ctx);
void machine_set_midi_out_handler(machine_midi_out_fn fn, void *ctx);
/* MIDI bytes from the host's sources (any thread): they leave the host
 * client's output port as packets. */
void machine_midi_in(const uint8_t *bytes, int n);

/* Sound input from the host's microphone, for the Sound Input Manager:
 * the handler starts capturing (start true; false if there is no
 * microphone) and stops; captured samples come back through
 * machine_sound_input (floats, -1 to 1, interleaved; any thread). */
typedef bool (*machine_sound_input_fn)(bool start, void *ctx);
void machine_set_sound_input_handler(machine_sound_input_fn fn, void *ctx);
void machine_sound_input(const float *pcm, int frames, int channels, double rate);

/* ---- Game controllers ----
 * The host's game controllers, for InputSprocket: up to MACHINE_PADS of
 * them, each with its buttons (a mask of MACHINE_PAD_*), two sticks (-1 to
 * 1, up and right positive) and two triggers (0 to 1).  The host sets a
 * pad's state whenever it changes, and connected false when it goes away;
 * any thread. */
enum { MACHINE_PADS = 4 };
enum {
    MACHINE_PAD_A = 1 << 0, MACHINE_PAD_B = 1 << 1, MACHINE_PAD_X = 1 << 2, MACHINE_PAD_Y = 1 << 3,
    MACHINE_PAD_L1 = 1 << 4, MACHINE_PAD_R1 = 1 << 5, MACHINE_PAD_L2 = 1 << 6, MACHINE_PAD_R2 = 1 << 7,
    MACHINE_PAD_L3 = 1 << 8, MACHINE_PAD_R3 = 1 << 9, MACHINE_PAD_MENU = 1 << 10, MACHINE_PAD_OPTIONS = 1 << 11,
    MACHINE_PAD_UP = 1 << 12, MACHINE_PAD_DOWN = 1 << 13, MACHINE_PAD_LEFT = 1 << 14, MACHINE_PAD_RIGHT = 1 << 15,
};
enum { MACHINE_PAD_LX, MACHINE_PAD_LY, MACHINE_PAD_RX, MACHINE_PAD_RY, MACHINE_PAD_LT, MACHINE_PAD_RT, MACHINE_PAD_AXES };
typedef struct {
    bool connected;
    char name[64]; /* UTF-8, as the host names it */
    uint32_t buttons;
    float axes[MACHINE_PAD_AXES];
} machine_gamepad;
void machine_set_gamepad(int index, const machine_gamepad *pad);

/* ---- Images ---- */

/* Decode a still image the host's frameworks read (JPEG for QuickTime's
 * Photo-JPEG codec) into RGBA rows (*rgba malloc'd, *w by *h). */
typedef bool (*machine_image_fn)(const uint8_t *data, size_t len, uint8_t **rgba, int *w, int *h, void *ctx);
void machine_set_image_decoder(machine_image_fn fn, void *ctx);
/* Encode RGBA rows (w by h) as a baseline JPEG at a quality from 0 to 1
 * (*out malloc'd, *len bytes), for Photo-JPEG compression. */
typedef bool (*machine_image_encode_fn)(const uint8_t *rgba, int w, int h, double quality, uint8_t **out, size_t *len,
                                        void *ctx);
void machine_set_image_encoder(machine_image_encode_fn fn, void *ctx);

/* ---- Clipboard ---- */

/* Text the app put on its clipboard (UTF-8), for the host clipboard. */
typedef void (*machine_clipboard_fn)(const char *utf8, void *ctx);
void machine_set_clipboard_handler(machine_clipboard_fn fn, void *ctx);
/* The host clipboard changed: this text becomes the app's clipboard. */
void machine_set_clipboard_text(const char *utf8);
/* The app went to the background or came back (suspend/resume events);
 * convert_clipboard tells it to read the clipboard again. */
void machine_post_suspend_resume(bool resume, bool convert_clipboard);

/* Documents to open at launch (call before machine_launch): given to the
 * app through its launch parameters, as the Finder did. */
void machine_set_launch_documents(const char *const *paths, int n);

/* The host folder that stands in for the System Folder (Preferences and
 * the other FindFolder folders live inside it).  The default is
 * ~/Library/Application Support/Blue Box/System Folder on macOS and
 * $XDG_DATA_HOME/classic/System Folder elsewhere. */
void machine_set_system_folder(const char *path);
/* The folder that is the Mac's volume ("Macintosh HD"); by default
 * Application Support/Blue Box/Macintosh HD.  Before machine_launch. */
void machine_set_disk(const char *path);
/* What a serial port (0 the modem port, 1 the printer port) connects to:
 * "modem" or "modem:PORT" (a Hayes modem on TCP, answering calls to PORT),
 * "tcp:HOST:PORT", "listen:PORT", "pty", a host device path, or "none".
 * Before boot; false if the spec isn't one of these. */
bool machine_set_serial(int port, const char *spec);
/* AppleTalk on LocalTalk over UDP: "local" (the default: copies on this
 * host only), "lan" (the local network, on the interface the system
 * picks), "lan:IF" (on network interface IF, e.g. en0) or "off"; or
 * "bridge:IF", EtherTalk on wired interface IF (asks for an administrator
 * password when AppleTalk opens).  Before boot. */
bool machine_set_appletalk(const char *how);
/* bluebox bpf-helper: run as root, a BPF device for IF passed to SOCKET. */
int machine_bpf_helper(const char *sockpath, const char *ifname);
/* A headless run's clock: by default its time follows the instructions
 * run (10 million a second), so runs repeat exactly; real time instead
 * for programs that talk to other programs over the network. */
void machine_set_realtime(bool on);
/* With the wall clock (a host attached or real time), a program waiting
 * by spinning (a poll loop doing nothing, again and again) lets the host
 * thread sleep (idle.c); on by default, BLUEBOX_IDLE_SLEEP=0 too turns it
 * off.  The sleeps so far, and the time they took (ns). */
void machine_set_idle_sleep(bool allowed);
uint64_t machine_idle_sleeps(uint64_t *ns);
/* The system and application heaps checked after every nth Toolbox call
 * (0: never; BLUEBOX_HEAPCHECK=n sets it too): a call that damages one
 * halts the run with its name. */
void machine_set_heap_check(int n);
/* The program's loaded PowerPC code sections compared with a copy after
 * every nth Toolbox call (0: never; BLUEBOX_CODECHECK=n sets it too): a
 * call that changes them halts the run with its name. */
void machine_set_code_check(int n);
/* --walk-menus: a headless run chooses every menu item it can, one at a
 * time whenever the program waits for input, dismissing the dialogs they
 * open (Escape, Command-period, Return), and reports each step; it ends
 * "menu walk finished", or stuck in a dialog, or where the program or
 * Blue Box failed.  A program with no menu bar has its screen walked
 * instead (what it tests the mouse against and draws clicked), ending
 * "screen walk finished". */
void machine_set_menu_walk(bool on);
/* --walk-avoid: a step a walk with no menu bar (walking a program's screen)
 * is not to take, as it said it at an earlier run of the same program
 * (they go the same way each time) where it ended the program: "X,Y" a
 * click there, "key NAME" a key.  NULL clears them. */
void machine_walk_avoid(const char *step);

/* A hard cap on instructions executed, also inside routines the Toolbox
 * calls back (filters, hooks); 0 for none.  For headless runs. */
void machine_set_instruction_limit(uint64_t total);
/* Whether the application idled recently (a null event, an unpressed
 * button): stopped at the limit, it was waiting for input. */
bool machine_waiting_for_input(void);

/* Apple Events to the running app (as the Finder would send them). */
void machine_open_documents(const char *const *paths, int n, bool print);
/* A drag from the host's Finder or another program over the machine's
 * windows: ENTER with its files (host paths) and text (UTF-8), then MOVE,
 * and LEAVE or DROP, at Mac screen coordinates.  The program's Drag
 * Manager handlers see it at its next event call. */
enum { MACHINE_DRAG_ENTER, MACHINE_DRAG_MOVE, MACHINE_DRAG_LEAVE, MACHINE_DRAG_DROP };
void machine_host_drag(int kind, int x, int y, const char *const *files, int nfiles, const char *text);

/* Drags out of the machine: a program's TrackDrag that leaves its windows
 * for the host.  While a TrackDrag follows the mouse (machine_drag_out_
 * tracking), the host says whether the pointer is outside the machine's
 * windows; once it is, the drag is handed to the host's handler (on the
 * emulator thread, which must not block: the host copies what it needs and
 * starts its own drag session).  Each item's flavors come converted:
 * 'TEXT' as UTF-8 text, 'TEXT' with 'styl' as RTF, 'hfs ' as the file's
 * host path, 'PICT' as RGBA pixels, 'phfs' (a promised file) as a file the
 * sender writes into a folder the host names.  A representation without
 * data (promised, or costly to make) is fetched with machine_drag_out_data,
 * from any thread but the emulator's, which calls the sender's send
 * procedure.  The host reports the pointer back over the machine's windows
 * (machine_drag_out_over) and how the drag ended (machine_drag_out_end):
 * TrackDrag then returns, with the drop location the program asks for. */
enum { MACHINE_FLAVOR_TEXT, MACHINE_FLAVOR_RTF, MACHINE_FLAVOR_FILE, MACHINE_FLAVOR_IMAGE, MACHINE_FLAVOR_PROMISED_FILE };
typedef struct {
    int kind;                    /* MACHINE_FLAVOR_* */
    uint32_t type;               /* the classic flavor it comes from */
    const uint8_t *data;         /* NULL: ask machine_drag_out_data */
    size_t size;                 /* TEXT, RTF: the bytes; FILE: a UTF-8 path */
    uint32_t file_type, file_creator; /* PROMISED_FILE: the file to come */
} machine_drag_rep;
typedef struct {
    int nreps;
    const machine_drag_rep *reps;
} machine_drag_item;
typedef struct {
    int nitems;
    const machine_drag_item *items;
    int16_t h, v;                /* where it left, Mac global */
} machine_drag_export;
/* False refuses the drag: it goes on inside the machine. */
typedef bool (*machine_drag_out_fn)(const machine_drag_export *drag, void *ctx);
void machine_set_drag_out_handler(machine_drag_out_fn fn, void *ctx);
bool machine_drag_out_tracking(void);
void machine_drag_out_pointer(bool outside);
/* A representation's data (*data malloc'd; IMAGE: width * height * 4
 * bytes of RGBA).  PROMISED_FILE: folder is where the file is to go (the
 * drop location the sender sees), and *data the path it wrote.  False if
 * the sender could not give it, or the drag is over. */
typedef struct {
    uint8_t *data;
    size_t size;
    int width, height;
} machine_drag_data;
bool machine_drag_out_data(int item, int kind, const char *folder, machine_drag_data *out);
/* The host's drag over the machine's windows again at x, y (inside), or
 * away from them. */
void machine_drag_out_over(bool inside, int x, int y);
/* DROPPED: the host took it (folder: where it went, if a folder; or NULL);
 * INSIDE: dropped back on the machine's windows at x, y; CANCELLED. */
enum { MACHINE_DRAG_OUT_DROPPED, MACHINE_DRAG_OUT_INSIDE, MACHINE_DRAG_OUT_CANCELLED };
void machine_drag_out_end(int how, int x, int y, const char *folder);

void machine_send_open_application(void);
void machine_send_quit(void);
/* Any Apple Event, delivered as a high-level event: its class and ID, and
 * a direct object of files (host paths, as a list of FSSpecs) or of text
 * (UTF-8, as 'TEXT': 'misc'/'dosc' takes a script this way), or neither.
 * With `reply`, the program's reply comes back to it (flattened as
 * aem_flatten does, and the handler's result) once handled, from the
 * emulator's thread; NULL data if it was never handled. */
typedef void (*machine_ae_reply_fn)(void *ctx, const uint8_t *reply, uint32_t len, int16_t err);
void machine_send_apple_event(uint32_t cls, uint32_t id, const char *const *files, int nfiles, const char *text,
                              machine_ae_reply_fn reply, void *ctx);
/* Take Apple Events from other processes (other runs, `bluebox send`):
 * a Unix socket named for this process and its application's signature,
 * removed at exit.  After machine_launch. */
bool machine_listen_for_apple_events(void);
/* The runs listening (with this home folder): process ID and signature. */
typedef struct {
    int pid;
    uint32_t signature;
} machine_ae_process;
int machine_ae_processes(machine_ae_process *out, int max);
/* What a listed run says of itself: its name (a Pascal string), its type
 * ('APPL', or 'FNDR' for a Finder) and its application's host path; false
 * if it says nothing. */
bool machine_ae_process_info(int pid, uint32_t sig, uint8_t name[32], uint32_t *type, char *path, size_t size);
/* Send a run an event as machine_send_apple_event builds them; with wait,
 * its result (the reply's direct object as text, or the error's message)
 * and keyErrorNumber come back.  The handler's result, or procNotFound,
 * errAETimeout. */
int16_t machine_ae_send_to(int pid, uint32_t sig, uint32_t cls, uint32_t id, const char *const *files, int nfiles,
                           const char *text, bool wait, char *result, size_t size, int32_t *errn);
/* A flattened reply's direct object as text (UTF-8) and keyErrorNumber;
 * false if it has no direct object (text then holds any error message). */
bool machine_ae_reply_result(const uint8_t *reply, uint32_t len, char *text, size_t size, int32_t *errn);
/* Ask the app to quit (Quit Application event, or its own Quit command). */
void machine_request_quit(void);

/* Suspending the program (the host's Suspend command): the machine's
 * thread stops at its next pause point and sleeps there until
 * machine_unpause, with the machine's clock (Ticks, the Time Manager, VBL
 * tasks, sound, the date) standing still from the moment of asking, so
 * the program finds no jump when it goes on.  From any thread;
 * machine_pause_for (the pause:N scripted step) ends by itself after ms.
 * machine_pause_parked: the machine's thread is stopped;
 * machine_wait_parked waits up to ms for it to be. */
void machine_pause(void);
void machine_unpause(void);
void machine_pause_for(uint32_t ms);
bool machine_paused(void);
bool machine_pause_parked(void);
bool machine_wait_parked(uint32_t ms);
/* The machine's thread: stop here while suspended. */
void machine_pause_point(void);

/* Draining (drain.c): the machine runs on to a safe point, its event loop
 * with nothing of the Toolbox on the host's stack, where a snapshot of it
 * can be taken and resumed in another process.  machine_pause starts one
 * (and stops there if it comes in time); exit_run makes machine_run
 * return RUN_DRAINED there instead.  Refused, with a reason to show, if
 * no safe point comes within two seconds or the program holds what a
 * snapshot cannot carry (machine_hibernate_blocker).  From any thread. */
enum { MACHINE_DRAIN_NONE, MACHINE_DRAIN_GOING, MACHINE_DRAIN_PARKED, MACHINE_DRAIN_REFUSED };
void machine_drain_begin(bool exit_run);
void machine_drain_end(void);
int machine_drain_state(void);
const char *machine_drain_reason(void);
/* Where it stopped: the trap (the program counter back on it), or "between
 * instructions". */
const char *machine_drain_point(void);
/* Stopped at a safe point: a snapshot may be taken now. */
bool machine_hibernatable(void);
/* Why the program cannot be saved now (its own threads, a connection, a
 * serial port, a print job), or NULL. */
const char *machine_hibernate_blocker(void);
/* Headless: a drain (exit_run) once the instruction count reaches this. */
void machine_hibernate_at(uint64_t icount);
/* Hibernating (reattach.c): the snapshot of a machine stopped at a safe
 * point written to path (compressed), with the host's record of the launch
 * (text, read back with hb_file_extra) and its size in *bytes.  Restore:
 * into a fresh process booted (machine_boot) as the record says, instead
 * of machine_launch; the program goes on with machine_run. */
bool machine_hibernate_save(const char *path, const char *record, uint64_t *bytes, char *err, size_t errlen);
bool machine_hibernate_restore(const char *path, char *err, size_t errlen);
/* Whether a resumed program's date catches up to the host's (the default)
 * or goes on from where it stopped. */
void machine_set_resume_date_catch_up(bool on);
/* A host monotonic time (ns) less the time spent suspended. */
uint64_t machine_unpaused_ns(uint64_t now);

/* Trap census for the current run. */
typedef struct {
    uint16_t trap;
    int32_t selector;   /* -1 if not a dispatched routine */
    const char *name;
    uint32_t calls;
    bool implemented;
} census_row;
/* Fills rows (up to max); returns the number of distinct routines seen. */
unsigned machine_census(census_row *rows, unsigned max);
/* PowerPC imports nothing provides: their count, and "Library:symbol"
 * names separated by spaces (data imports carry a trailing "*"). */
unsigned machine_ppc_missing(const char **names);

/* Dump the register state to a buffer (for diagnostics). */
void machine_describe_cpu(char *buf, size_t len);

#endif
