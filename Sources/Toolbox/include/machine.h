/* Public interface to the classic environment, used by the host shells and CLI. */
#ifndef MACHINE_H
#define MACHINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    RUN_LIMIT = 0,   /* instruction limit reached, still runnable */
    RUN_HALTED = 1,  /* the app quit or an error stopped the CPU */
} run_status;

/* Reset everything and set up an empty machine. Returns false on failure. */
bool machine_boot(bool census);
/* Whether Blue Box provides a PowerPC library's export (a routine, or data). */
bool machine_provides_export(const char *name, bool data);
/* Load an application (AppleDouble data-fork path) and prepare to run it. */
bool machine_launch(const char *app_path, char *err, size_t errlen);
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
/* A raw floppy disk image (a sector dump of 400K/720K/800K/1440K, or Disk
 * Copy 4.2) as the disk in the Sony driver's drive 1: a key disk whose
 * sectors the program's own copy protection reads (sony.c). */
bool machine_set_floppy(const char *path, char *err, size_t errlen);
bool sony_has_disk(void);
uint32_t sony_disk_size(void);
bool sony_sector_tags(uint32_t sector, uint8_t out[12]);
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
/* Screen depth: 1 (black and white, the default), 8 or 32; before boot. */
void machine_set_screen_depth(int depth);
/* The depth the next boot gives the screen (machine_screen_depth is the one it has). */
int machine_screen_depth_setting(void);
/* The screen's depth now (a program may have changed it). */
int machine_screen_depth(void);
/* 68040 (with FPU, the default) or 68020 (no FPU). Before machine_boot. */
void machine_set_cpu(int model);
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
int machine_menus(machine_menu *out, int max);
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
} machine_print_job;

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
/* The host's default paper, used for new print records. */
void machine_set_page_defaults(const machine_page_setup *ps);

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
    double rate;             /* words per minute */
    double pitch;            /* base pitch, as SetSpeechPitch's (about 30 to 65) */
    double modulation, volume;
    const char *text;
    bool phonemes;           /* the text is phonemes ([[inpt PHON]]) */
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
 * Blue Box failed. */
void machine_set_menu_walk(bool on);

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
