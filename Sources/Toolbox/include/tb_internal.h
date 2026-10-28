/* Shared declarations for the Toolbox implementation (not a public API). */
#ifndef TB_INTERNAL_H
#define TB_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "m68k.h"
#include "log.h"
#include "lowmem_gen.h"
#include "machine.h"
#include "mem.h"
#include "structs_gen.h"
#include "tb_protos_gen.h"
#include "trap.h"

/* The one CPU in this process. */
extern cpu68k *g_cpu;

/* Natives that manage the 68k stack themselves (no generated glue). */
typedef struct {
    uint16_t trap;
    const char *name;
    native_fn fn;
} raw_trap;
extern const raw_trap raw_traps[]; /* terminated by fn == NULL */

/* Error codes. */
enum {
    noErr = 0,
    paramErr = -50,
    memROZErr = -99,
    memFullErr = -108,
    nilHandleErr = -109,
    memWZErr = -111,
    memPurErr = -112,
    memLockedErr = -117,
    resNotFound = -192,
    resFNotFound = -193,
    addResFailed = -194,
    rmvResFailed = -196,
    resAttrErr = -198,
    mapReadErr = -199,
    fnfErr = -43,
    ioErr = -36,
    gestaltUnknownErr = -5550,
};

static inline void set_mem_err(int16_t e) { wr16(LM_MemErr, (uint16_t)e); }
/* ResErr, and the application's ResErrProc (if any) told of an error. */
void res_err_proc(int16_t e);
static inline void set_res_err(int16_t e)
{
    wr16(LM_ResErr, (uint16_t)e);
    if (e && rd32(LM_ResErrProc))
        res_err_proc(e);
}

/* ---- Memory Manager internals ---- */

/* Set up a zone from start to limit. */
void mm_init_zone(gaddr zone, gaddr limit, uint16_t more_masters);
/* Make the application zone growable up to max_limit. */
void mm_set_appl_growth(gaddr max_limit);
/* Allocate in a specific zone. */
gaddr mm_new_handle_in(gaddr zone, uint32_t size, bool clear);
gaddr mm_new_ptr_in(gaddr zone, uint32_t size, bool clear);
/* Cap on memory sizes reported to apps (FreeMem, MaxBlock ...). */
extern uint32_t g_mem_report_cap;
/* Check that the zone's blocks tile it correctly; returns false and logs on damage. */
bool mm_check_zone(gaddr zone);
/* BLUEBOX_HEAPCHECK: the zones checked after a Toolbox call; false if damaged. */
bool mm_debug_after_call(const char *name);
/* BLUEBOX_HEAPCHECK=1: the zones checked before a call too; false if the
 * code run since the last check damaged them. */
bool mm_debug_before_call(const char *name);
/* BLUEBOX_CODECHECK (codecheck.c): code loaded at base kept for comparing
 * after Toolbox calls (what names it in the report), forgotten when it is
 * unloaded or the next program launches; false after a call when some of
 * it has changed. */
void code_check_add(gaddr base, uint32_t size, const char *what);
void code_check_remove(gaddr base);
bool code_check_after_call(const char *name);
bool code_check_before_call(const char *name);
void code_check_reset(void);
/* The menu bar for the menu walker (menu.c, dialogs.c, walker.c). */
int menu_bar_count(void);
int16_t menu_bar_id(int i);
bool menu_bar_item(int i, int n, char *text, size_t size, bool *enabled, bool *submenu);
gaddr dialog_in_front(void);
uint32_t dialog_signature(gaddr d);
bool dialog_modal(gaddr d);
int dialog_button(gaddr d, int n, char *title, size_t size, int *x, int *y);
/* The standard dialog filter: Return and Enter press the default button,
 * Escape and Command-period the cancel button (dialogs.c). */
bool dialog_std_filter(gaddr d, gaddr evt, gaddr itemp);
int window_button(gaddr w, int n, char *title, size_t size, int *x, int *y); /* controls.c */
bool ctl_push_button(gaddr ch, bool custom);
void walker_idle(uint64_t now);

/* ---- Resource Manager internals ---- */

void rm_init(void);
/* Write every changed resource file (at ExitToShell). */
void rm_update_all(void);
/* Open a host file's resource fork as a resource file; returns refnum or -1. */
int16_t rm_open_host_file(const char *path, const uint8_t *fork, uint32_t len, bool read_only);
/* Make an open resource file the System file. */
void rm_set_system(int16_t rn);
/* Register an empty resource map to act as the System file. */
int16_t rm_open_empty_system(void);

/* ---- Segment Loader ---- */

/* Load CODE 0, build the A5 world and jump table; returns entry point or 0. */
/* Start another classic application (the host's launcher); false if
 * there is none or it failed. */
bool process_launch(const char *app, const char *const *docs, int ndocs);
bool process_is_finder(uint32_t hi, uint32_t lo); /* the Finder's serial number */
bool process_open_url(const char *url);
/* Internet Config's ICLaunchURL: the URL in data[a..b) (all len bytes when
 * the selection is empty) opened by the host; an Internet Config error. */
int16_t ic_launch_url(gaddr data, int32_t len, int32_t a, int32_t b);
/* Internet Config's preferences (components.c), for both CPUs. */
void ic_make_file(void); /* ICFindConfigFile: the Internet Preferences file, made when missing */
/* Internet Config's mappings, URLs and configuration (vam.c), for both its
 * PowerPC calls and its component's selectors. */
int16_t ic_map_count(gaddr entries, gaddr count);
int16_t ic_map_get_ind(gaddr entries, int32_t index, gaddr pos, gaddr entry);
int16_t ic_map_get(gaddr entries, int32_t pos, gaddr entry);
int16_t ic_map_set(gaddr entries, int32_t pos, gaddr entry);
int16_t ic_map_delete(gaddr entries, int32_t pos);
int16_t ic_map_add(gaddr entries, gaddr entry);
int16_t ic_map_entries_filename(gaddr entries, gaddr name, gaddr entry);
int16_t ic_map_entries_type_creator(gaddr entries, uint32_t type, uint32_t creator, gaddr name, gaddr entry);
int16_t ic_map_filename(gaddr name, gaddr entry);
int16_t ic_map_type_creator(uint32_t type, uint32_t creator, gaddr name, gaddr entry);
int16_t ic_parse_url(gaddr hint, gaddr data, int32_t len, gaddr sel_start, gaddr sel_end, gaddr url);
int16_t ic_get_config_ref(gaddr ref);
int16_t ic_set_config_ref(gaddr ref);
int16_t ic_edit_preferences(void);
gaddr components_open_default(uint32_t type, uint32_t subtype);
int16_t ic_get_pref(gaddr key, gaddr attr, gaddr buf, gaddr size);
int16_t ic_get_pref_handle(gaddr key, gaddr attr, gaddr out, gaddr h);
int16_t ic_set_pref(gaddr key, gaddr buf, int32_t len);
int16_t ic_delete_pref(gaddr key);
int16_t ic_get_ind_pref(int32_t index, gaddr key);
int32_t ic_count_prefs(void);
uint32_t ic_get_seed(void);
uint32_t seg_launch(int16_t app_refnum, uint32_t *a5_out, char *err, size_t errlen);

/* ---- Gestalt ---- */

void gestalt_init(void);
uint32_t gestalt_value(uint32_t selector, uint32_t fallback);

/* Standard File's host panels (stdfile.c), for Navigation Services too:
 * a request answered with a host path, and a filter of its own for the
 * files an open panel offers (true hides; NULL for none). */
bool stdfile_request(machine_file_request *rq, char *path, size_t pathsize);
void stdfile_set_filter(bool (*fn)(const char *path));

/* ---- File Manager ---- */

/* An FSSpec for a host path (the file need not exist). */
void fm_spec_for_path(const char *path, gaddr spec);
/* Host path for a (vRefNum, dirID, name) triple. */
bool fm_host_path(int16_t vref, int32_t dirid, gaddr name, bool hfs, char *out, size_t outsize);
/* Whether a host file exists for the Mac: its data fork, or only its
 * AppleDouble sidecar (a file with an empty data fork). */
bool fm_host_exists(const char *path);
/* Classic file type of a host file (0 if unknown). */
uint32_t fm_type_of_path(const char *path);
/* The application (host path) that opens a document: one with the
 * document's creator, on the disk or in a granted folder. */
bool fm_app_for_document(const char *doc, char *app, size_t appsize);
/* The application with a creator, found the same way. */
bool fm_app_for_creator(uint32_t creator, char *app, size_t appsize);
/* The System Folder's working directory refnum. */
int16_t fm_system_vref(void);
/* The guest FCB table: an access path opened, closed, or resized. */
void fm_fcb_record(int16_t rn, const char *path, bool rsrc, bool write, uint32_t eof, uint32_t pos);
void fm_fcb_forget(int16_t rn);
/* Forget the guest tables (at boot, before memory is reused). */
void fm_reset(void);
void fm_fcb_eof(int16_t rn, uint32_t eof);
/* Mac full path (MacRoman) of a host path; returns its length, 0 if too long. */
size_t fm_mac_path_of_host(const char *host, uint8_t *out, size_t outsize);
/* Host path of a Mac full path. */
bool fm_host_of_mac_path(const uint8_t *mac, size_t n, char *out, size_t outsize);

/* ---- String comparison (register traps) ---- */

void raw_CmpString(cpu68k *c, uint16_t tw);
void raw_FP68K(cpu68k *c, uint16_t tw);
void raw_Fix2X(cpu68k *c, uint16_t tw);
void raw_Frac2X(cpu68k *c, uint16_t tw);
void raw_Microseconds(cpu68k *c, uint16_t tw);
void raw_GetVideoDefault(cpu68k *c, uint16_t tw);
void raw_ResourceStub(cpu68k *c, uint16_t tw);
void raw_Elems68K(cpu68k *c, uint16_t tw);
void sane_reset(void);
/* Time Manager and VBL tasks due now (interrupt time). */
void time_pump(void);
/* An asynchronous call's completion routine, at the next interrupt pump. */
void io_complete_later(gaddr pb, int16_t result);
void io_complete_soon(gaddr pb, int16_t result); /* before the call returns, unless in a completion */
bool time_busy(void);  /* a Time Manager or VBL task waiting */
bool sound_busy(void); /* a sound channel open */
void adb_install(void); /* the ADB Manager's ADBOp, at boot */
void adb_pump(void);    /* the mouse to a program's service routine, at interrupt time */
/* Let the application see and change a host file (and its neighbours) or,
 * with tree, a folder and its contents (filemgr.c). */
void fm_grant(const char *path, bool tree);
bool fm_refnum_host_path(int16_t refnum, char *out, size_t outsize);
bool fm_last_data_fork(char *out, size_t outsize); /* the data fork opened last */
/* AppleShare volumes mirrored into cache folders (appleshare.c): a folder
 * filled from the server before it is looked into, and changes sent back. */
void as_ensure_dir(const char *host_dir);
void as_ensure_file(const char *path); /* a placeholder's forks fetched before they are read */
void as_file_changed(const char *path);
void as_dir_created(const char *path);
void as_deleted(const char *path);
void as_moved(const char *from, const char *to);
void as_idle(void); /* at event calls: idle sessions kept open */
void notify_host(int id, const char *text); /* a host notification of ours (notify.c) */
/* Components of our own (components.c). */
uint32_t components_register_native(uint32_t type, uint32_t subtype, uint32_t manufacturer, gaddr entry);
/* AppleScript's components and the applet shell (osa.c), each run. */
void osa_register(void);
uint32_t components_open(uint32_t component);
void components_set_storage(uint32_t ci, gaddr storage);
void components_close(uint32_t ci);
/* Components of ours in C (QuickTime's): fn gets the instance and the
 * ComponentParameters record; the result is the call's. */
typedef int32_t (*component_c_fn)(uint32_t ci, gaddr params, void *cookie);
uint32_t components_register_c(uint32_t type, uint32_t subtype, uint32_t manufacturer, uint32_t flags,
                               const char *name, const char *info, component_c_fn fn, void *cookie);
int32_t components_call(uint32_t ci, gaddr params);
uint32_t components_component_of(uint32_t ci);
bool components_describe(uint32_t cid, uint32_t *type, uint32_t *subtype, uint32_t *manufacturer);
uint32_t components_find_next(uint32_t after, uint32_t type, uint32_t subtype, uint32_t manufacturer);
uint32_t components_open_component(uint32_t cid, int16_t *err);
void qt_components_reset(void);
void qt_callbacks_periodic(void); /* QuickTime's interrupt-time callbacks (qt_callbacks.c) */
void components_reset(void);
void quicktime_reset(void);
/* Mouse button and location into low memory (events.c). */
void events_sync_lowmem(void);
void fm_flush_dirty(void); /* open forks written out (filemgr.c) */
/* The mouse and modifiers now (events.c); the window under a global point (wm.c). */
bool ev_still_down(void);
uint32_t ev_mouse_global(void);
/* Mouse presses since boot, counted so a quick click is never missed (events.c). */
uint32_t ev_press_count(void);
uint16_t ev_modifiers(void);
void ev_wait_tick(void);
/* Headless, n pixels' worth of QuickDraw time passes (machine.c). */
void machine_charge_pixels(uint64_t n);
/* TickCount polled: headless, a tight loop of polls skips to the next tick. */
void machine_tick_polled(void);
gaddr wm_window_at(uint32_t p);
void drag_reset(void); /* Drag Manager (drag.c): a fresh machine */
void print_reset(void); /* Printing Manager (print.c) */
/* A print record's page setup in points, and one put in at a resolution
 * (the Carbon Printing Manager's page formats are print records too). */
void print_get_setup(gaddr p, machine_page_setup *ps);
void print_put_setup(gaddr p, const machine_page_setup *ps, int vres, int hres);
void carbonsvc_reset(void); /* CarbonLib's services (carbon_svc.c) */
void carbonrest_reset(void); /* CarbonLib's remaining managers (carbonrest.c) */
void icm_reset(void); /* Image Compression Manager (qt_icm.c) */
void gx_reset(void); /* QuickDraw GX (gx.c) */
/* The mouse's own motion for ADB packets (events.c). */
bool ev_take_motion(int *dh, int *dv, int limit);
bool ev_motion_pending(void);
/* All the mouse's own motion since boot, and its button (InputSprocket);
 * capture: the program is reading the motion, so the host captures it. */
void ev_mouse_raw(int32_t *h, int32_t *v, bool *down, bool capture);
/* After a Toolbox trap from the program, not a callback (display.c). */
void display_trap_done(void);
/* A pop-up menu's area while it is up (display.c), for rootless hosts. */
void display_set_transient(bool on, int16_t top, int16_t left, int16_t bottom, int16_t right);
/* Scripted input (events.c): apply the steps due by `now`; the next one's time. */
void events_script_pump(uint64_t now);
uint64_t events_script_next_at(void);
/* Bring Ticks up to host time. */
void machine_advance_ticks(void);
/* The machine's clock (ns): wall clock with a host, instructions without. */
uint64_t machine_time_ns(void);
void machine_skip_time(uint64_t ns);
void machine_set_time(uint32_t secs); /* SetDateTime: Time counts from secs */
bool events_host_attached(void);
/* A null event in a headless run: counts towards the instruction limit. */
void machine_idle(void);
void time_reset(void);
/* Layers and Open Transport's tasks (vam.c): a new run; the tasks due. */
void vam_reset(void);
void ot_tasks_pump(void);
/* The MIDI Manager (midi.c): packets and time base ports, run from time_pump. */
void midi_pump(void);
void midi_reset(void);
void raw_RelString(cpu68k *c, uint16_t tw);
void raw_AddDrive(cpu68k *c, uint16_t tw); /* vam.c */
void raw_UprString(cpu68k *c, uint16_t tw);
void raw_LowerText(cpu68k *c, uint16_t tw);
void raw_DrvrInstall(cpu68k *c, uint16_t tw);
int16_t driver_install(gaddr drvr, int16_t refnum); /* drivers.c */
void raw_ReadXPRam(cpu68k *c, uint16_t tw);
void raw_WriteXPRam(cpu68k *c, uint16_t tw);
void raw_InitZone(cpu68k *c, uint16_t tw);
void raw_PostEvent(cpu68k *c, uint16_t tw);
void raw_CursorDeviceDispatch(cpu68k *c, uint16_t tw);
/* The Cursor Device Manager by selector, its arguments left to right (osutil.c). */
int16_t cursor_device(uint16_t sel, const uint32_t *args);
void cursor_devices_reset(void);
void raw_SlotManager(cpu68k *c, uint16_t tw);
/* The Slot Manager (slots.c): a selector on an SpBlock, as _SlotManager has them. */
int16_t slot_manager(uint16_t sel, gaddr spb);
void slots_reset(void);
void raw_MemoryDispatch(cpu68k *c, uint16_t tw);
/* Virtual memory's calls by _MemoryDispatch selector (driverservices.c);
 * *a0_out is A0 after. */
int16_t memory_dispatch(uint16_t sel, uint32_t a0, uint32_t a1, uint32_t *a0_out);
void raw_IdleUpdate(cpu68k *c, uint16_t tw);
void raw_ComponentDispatch(cpu68k *c, uint16_t tw);
/* A MacRoman character's comparison key. */
int mac_char_key(uint8_t c, bool case_sens, bool ignore_marks);
void intl_convert_text(gaddr text, int32_t len, bool upper, bool strip);
uint8_t *intl_itl2(uint32_t *lenp);
/* Default US 'itl0' and 'itl1' resources. */

/* ---- Notification Manager ---- */

void notify_pump(void);
void notify_reset(void);
void iconservices_reset(void); /* Icon Services (iconservices.c) */

/* ---- Sound Manager ---- */

/* Run sound commands whose time has come (at safe points). */
void sound_pump(void);
void sound_beep(void);
void sound_reset(void);
/* The Sound Input Manager (sound_input.c): recordings go on from the
 * sound pump; busy while one is asynchronous. */
void sound_input_pump(void);
bool sound_input_busy(void);
void sound_input_reset(void);
/* PCM straight to the host on a channel id of the caller's (movies). */
void sound_host_pcm(int id, const int16_t *pcm, int frames, int nch, double rate, float volume);
void sound_host_stop(int id);
/* The sound output device's settings by SndGetInfo/SndSetInfo selector
 * (GetSoundOutputInfo and SetSoundOutputInfo). */
int16_t sound_output_get_info(uint32_t sel, gaddr info);
int16_t sound_output_set_info(uint32_t sel, gaddr info);
/* MIDI to the host's synthesizer id (n 0: silence it). */
void sound_host_midi(int id, const machine_midi_event *events, int n);

/* ---- Appearance ---- */

/* MACHINE_APPEARANCE_*: how the standard definition procedures draw. */
extern int g_appearance;
#include "qd_internal.h"
/* A compressed picture's image ($8200 opcode), drawn at dst in the current port. */
void icm_draw_compressed(gaddr desc, gaddr data, qrect src, qrect dst, gaddr mask);

/* Themes (theme.c): each returns false when no theme applies, and the
 * standard black-and-white drawing is used instead. */
bool theme_active(void);
bool theme_window_insets(int var, int *l, int *t, int *r, int *b);
bool theme_draw_window(gaddr w, int var, qrect c, bool hilited, bool goaway, bool zoom, int param);
void theme_decorate_window(gaddr w, int var, qrect c, bool hilited, bool goaway, bool zoom);
bool theme_window_region(gaddr w, int var, qrect c, hrgn *out);
int theme_window_hit(gaddr w, int var, qrect c, int h, int v);
bool theme_draw_grow(qrect c, bool active);
bool theme_draw_scrollbar(qrect r, bool vertical, int hilite, bool enabled, bool active, qrect thumb);
bool theme_draw_button(qrect r, const uint8_t *title, int n, bool pressed, bool disabled);
bool theme_draw_check(qrect box, bool radio, bool on, bool pressed, bool disabled);
/* Native controls (native_controls.c), drawn by the host. */
bool native_draw_button(qrect r, const uint8_t *title, int n, bool pressed, bool disabled);
bool native_draw_check(qrect box, bool radio, bool on, bool pressed, bool disabled);
bool native_draw_scrollbar(gaddr ch, qrect r, bool vertical, int hilite, bool enabled, bool active, int value, int min,
                           int max);
/* Where the host last drew a scroll bar's knob and its slot, along the bar. */
bool native_scroll_knob(gaddr ch, int *knob_lo, int *knob_hi, int *slot_lo, int *slot_hi);
void native_forget_control(gaddr ch);
void qd_forget_control_colors(gaddr ctl);
bool native_draw_popup(qrect r, const uint8_t *item, int n, bool pressed, bool disabled);
bool native_draw_edit_frame(qrect r, bool focused);

/* The Appearance Manager (appearance.c): whether the program registered as
 * a client; the host's colours taken up at event calls; the Platinum
 * look's drawing, in RGB through the current port's clip (black and white
 * below 4 bits a pixel).  States are ThemeDrawState's: 0 inactive, 1
 * active, 2 pressed. */
bool appearance_client(void);
void appearance_reset(void);
void appearance_poll(void);
void appearance_hilite_changed(uint16_t r, uint16_t g, uint16_t b);
bool ap_brush_rgb(int brush, uint16_t out[3]);
bool ap_text_rgb(int color, uint16_t out[3]);
bool ap_in_color(void);
void ap_fill_rgn(const hrgn *g, const uint16_t rgb[3]);
void ap_fill(qrect r, const uint16_t rgb[3]);
void ap_fill_grey(qrect r, uint16_t v);
void ap_frame(qrect r, const uint16_t rgb[3]);
void ap_frame_grey(qrect r, uint16_t v);
void ap_bevel(qrect r, uint16_t light, uint16_t dark);
void ap_text(int x, int y, const uint8_t *s, int n, const uint16_t rgb[3]);
void ap_text_in(qrect r, const uint8_t *s, int n, bool center, const uint16_t rgb[3]);
void ap_font_info(int *ascent, int *descent);
void ap_arrow(qrect r, int dir, int size, const uint16_t rgb[3]);
void ap_placard(qrect r, uint32_t state);
void ap_well_frame(qrect r, uint32_t state);
void ap_focus_rect(qrect r, bool focus);
void ap_group(qrect r, uint32_t state, bool primary);
void ap_separator(qrect r, uint32_t state);
void ap_tab_pane(qrect r, uint32_t state);
void ap_tab(qrect r, int style, const uint8_t *title, int n);
void ap_push_button(qrect r, uint32_t state, bool is_default);
void ap_default_ring(qrect r, uint32_t state);
void ap_check_box(qrect box, bool radio, int value, uint32_t state);
void ap_bevel_button(qrect r, int kind, uint32_t state, bool on);
void ap_disclosure(qrect r, int value, uint32_t state, bool left_facing);
void ap_little_arrows(qrect r, uint32_t state);
void ap_popup_button(qrect r, uint32_t state, bool arrow_only);
void ap_button(qrect r, int kind, uint32_t state, int value, int adorn);
void ap_track(gaddr info); /* a ThemeTrackDrawInfo in guest memory */
void ap_scroll_bar(qrect r, bool vertical, int hilite, bool active, bool live, qrect thumb);

/* ---- Control Manager (controls.c) ---- */

/* What we keep for each control: its variant and CDEF (procID & 15 and
 * >> 4), its embedder and when it was embedded, what its embedder's hiding
 * or deactivation changed, its ControlFontStyleRec, and the Appearance
 * CDEF's own state (cdefs_appearance.c, malloc'd). */
typedef struct {
    gaddr ch;
    int16_t var, cdef;
    gaddr parent, supervisor;
    uint32_t seq;
    bool is_root, hidden_with_parent, inactive_with_parent, has_font;
    uint8_t saved_hilite;
    int8_t new_msgs; /* takes Control Manager 2.0's messages: -1 not asked yet */
    uint8_t font[24];
    int32_t view_size; /* SetControlViewSize's */
    void *ap;
    void *carbon; /* CarbonLib's: its ID, command, properties (carbon_controls.c) */
} ctl_info;
typedef struct {
    int16_t font, size, mode;
    uint8_t face;
} ctl_font_saved;
ctl_info *ctl_find(gaddr ch);
int32_t ctl_call(gaddr ch, int16_t msg, int32_t param);
bool ctl_platinum(gaddr ch);
gaddr ctl_window_root(gaddr w);
void ctl_set_window_themed(gaddr w, bool themed);
int ctl_subcontrols(gaddr ch, gaddr *out, int max);
void ctl_font_begin(gaddr ch, ctl_font_saved *s);
void ctl_font_end(const ctl_font_saved *s);
bool ctl_font_color(gaddr ch, uint16_t rgb[3]);
uint32_t ctl_local_mouse(gaddr ch);
void ctl_call_action(gaddr action, gaddr ch, int16_t part);
void controls_reset(void);
void controls_forget_window(gaddr w);
gaddr ctl_new(gaddr wst, gaddr r, gaddr title, bool vis, int16_t value, int16_t min, int16_t max, int16_t var,
              int16_t cdef_id, gaddr defh, int32_t rc, int32_t init_param);
/* Called with the part a click on a control ended in (TrackControl,
 * HandleControlClick): for the Carbon Event Manager's kEventControlHit
 * and the control's command (carbon_ctl_command).  NULL until set. */
extern void (*ctl_hit_hook)(gaddr ch, int16_t part, uint16_t modifiers);
/* CarbonLib's controls (carbon_controls.c): a control's Carbon state
 * freed, its command ID and ControlID, the definition a program
 * registered for a CDEF ID (the initCntl parameter, a Collection made for
 * it, in *init; _done disposes of it), and the colour procedure a
 * program set (true when it set the colours). */
void carbon_ctl_free(ctl_info *c);
uint32_t carbon_ctl_command(gaddr ch);
bool carbon_ctl_id(gaddr ch, uint32_t *signature, int32_t *id);
gaddr carbon_registered_cdef(int16_t id, gaddr w, gaddr r, gaddr title, bool vis, int16_t value, int16_t min,
                             int16_t max, int16_t procid, int32_t rc, int32_t *init);
void carbon_registered_cdef_done(int32_t init);
bool carbon_ctl_color(gaddr ch, int16_t msg, int16_t depth, bool color);
void carbon_controls_reset(void);
/* CFStrings to and from MacRoman, through CarbonLib's own calls. */
void carbon_cfstring_to_pstr(gaddr cf, uint8_t out[256]);
gaddr carbon_cfstring_from_text(const uint8_t *text, size_t n);
void carbon_cfrelease(gaddr cf);
uint32_t carbon_call_export(const char *name, const uint32_t *args, int n);
/* The Data Browser (carbon_databrowser.c) */
void databrowser_reset(void);
/* The Appearance CDEFs (cdefs_appearance.c): their entry, the standard
 * buttons drawn in Platinum (kind 0 push, 1 check box, 2 radio), tracking
 * of their own (false when the Control Manager's will do), and what
 * follows a click (auto-toggles, tabs, radio groups). */
typedef struct {
    int16_t id;
    native_fn fn;
} ap_cdef_stub;
extern const ap_cdef_stub ap_cdef_stubs[];
extern const int ap_cdef_stub_count;
void ap_draw_standard(gaddr ch, int kind);
bool ap_ctl_track(gaddr ch, int16_t part, uint32_t pt, gaddr action, int16_t modifiers, int16_t *result);
void ap_after_track(gaddr ch, int16_t part);
/* A dialog item's stand-in control (GetDialogItemAsControl), out of the
 * window's control list; a user item's draws and takes clicks through its
 * procedures when it has them (ap_item_click: -1 when it has none). */
gaddr ap_item_control(gaddr dialog, int item, int kind);
bool ap_stand_in_item(gaddr ch, gaddr *dialog, int *item); /* the item a stand-in is for */
void ap_forget_dialog(gaddr dialog);
void ap_items_shifted(gaddr dialog, int from, int delta);
/* What CarbonLib's Create...Control calls set beyond NewControl's
 * arguments: content (push buttons with icons), a slider's tick marks, a
 * list box's list made to the arguments. */
void ap_set_content(gaddr ch, gaddr info);
void ap_set_ticks(gaddr ch, int n);
enum { AP_AUTO_TOGGLE = 1, AP_DRAWS_TITLE = 2 }; /* check box group boxes, disclosure triangles */
void ap_set_behavior(gaddr ch, int16_t bits);
bool ap_list_setup(gaddr ch, int rows, int cols, int cell_h, int cell_w, bool hs, bool vs, bool grow, gaddr ldef_upp);
bool ap_item_draw(gaddr dialog, int item);
int16_t ap_item_click(gaddr dialog, int item, uint32_t pt, int16_t modifiers);
void ap_cdefs_reset(void);
bool ap_is_stand_in(gaddr ch);
/* For the stand-ins (dialogs.c): an item drawn, its text selected, the
 * dialog's TERec if the item is the edit text being edited. */
void dialog_draw_item(gaddr d, int n);
void dialog_select_item_text(gaddr d, int n, int start, int end);
gaddr dialog_item_te(gaddr d, int n);

/* ---- Menu Manager (menu.c) ---- */
void menu_reset(void);
uint8_t menu_item_modifiers(gaddr mh, int item); /* Mac OS 8's kMenu...Modifier bits */

/* ---- Dialog Manager ---- */

/* A dialog from DLOG id; vis_override -1 keeps the resource's visibility. */
gaddr dialog_from_resource(int16_t id, gaddr dst, gaddr behind, int vis_override, int32_t refcon_override,
                           bool use_refcon);
void dialogs_reset(void);

/* ---- Alias Manager ---- */

/* A new alias handle for a host path, stored at alias_out. */
int16_t alias_new_for_host(const char *host, gaddr alias_out);

/* ---- Process Manager ---- */

/* The application's SIZE flags (isHighLevelEventAware is 0x0040). */
extern uint16_t g_app_size_flags;

/* Dialog Manager (dialog.c): text with ^0..^3 replaced by the ParamText
 * strings; a message asking for another screen depth ends the run when
 * the host allows (true when it did). */
size_t append_text(char *out, size_t n, size_t cap, const uint8_t *s, size_t len);
bool depth_alert(const char *message);

/* The Apple Menu Items folder (filemgr.c). */
int fm_apple_menu_items(char (*names)[256], int max);
bool fm_open_apple_menu_item(const uint8_t *name, int n);

/* Speech Manager (speech.c): callbacks at safe points; a fresh machine. */
void speech_pump(void);
void speech_reset(void);

/* Code Fragment Manager (cfm.c): PowerPC applications. */
struct ppc_cpu;
extern struct ppc_cpu g_ppc;
/* The bottom of the PowerPC stack's block (0 before PowerPC code runs). */
extern gaddr cfm_ppc_stack_lo;
/* The running thread's free stack (StackSpace, ThreadCurrentStackSpace). */
uint32_t thread_stack_space(void);
extern bool g_ppc_mode;
bool cfm_has_ppc(void);
void cfm_reset(void);   /* a launch: no fragments, no PowerPC processor yet */
bool cfm_ppc_cpu(void); /* the PowerPC processor, set up when first needed */
void gestalt_powerpc(void);
const char *fm_system_folder_host(void);
/* The Appearance Manager's StandardAlert (dialog.c). */
int16_t dialog_standard_alert(int16_t type, gaddr error, gaddr explanation, gaddr param, gaddr item_hit);
/* Native drivers (drivers.c): opened by name, called by negative refnum. */
enum { DRIVER_CONTROL, DRIVER_STATUS, DRIVER_READ, DRIVER_WRITE, DRIVER_CLOSE, DRIVER_KILLIO };
typedef struct {
    const char *name; /* ".IPP" */
    int16_t unit;     /* reference number -1 - unit */
    int16_t (*open)(gaddr pb);
    int16_t (*close)(gaddr pb);
    int16_t (*control)(gaddr pb, bool async);
    int16_t (*status)(gaddr pb, bool async);
    int16_t (*read)(gaddr pb, bool async);
    int16_t (*write)(gaddr pb, bool async);
    int16_t (*kill_io)(gaddr pb);
} native_driver;
/* A driver call's result while it runs on (asynchronous calls only): the
 * driver completes it, with its ioCompletion, later. */
enum { ioInProgress = 1 };
bool driver_open(gaddr pb, int16_t *err); /* false: no driver of that name */
enum { CD_UNIT = 35 };                    /* .AppleCD, the mounted volumes' driver */
void io_complete_now(gaddr pb, int16_t result); /* KillIO's: inside the call (timemgr.c) */
void io_complete_now_for(int16_t refnum);
void drivers_reset(void);                 /* the unit table, at boot */
/* Drivers a program supplies, in 68k code (driver68k.c). */
bool drv68k_refnum(int16_t refnum);
bool drv68k_open_name(gaddr pb, int16_t *err); /* false: no such driver or DRVR resource */
int16_t drv68k_call(gaddr pb, bool async, int kind);
int16_t drv68k_install(gaddr drvr, int16_t refnum);
void drv68k_reset(void);
int16_t drv68k_open_desk_acc(gaddr name, bool *found);
void drv68k_close_desk_acc(int16_t refnum);
bool drv68k_system_event(gaddr evp, bool automatic); /* automatic: GetNextEvent's own call */
void drv68k_system_click(gaddr evp, gaddr wp);
void drv68k_system_menu(int32_t result);
bool drv68k_system_edit(int16_t cmd);
void drv68k_periodic(void);
int16_t drv68k_take_opened(void);
bool drv68k_open_da_path(const char *path); /* a desk accessory file's DRVRs installed, the first opened */
/* The whole screen in one window instead of rootless windows (display.c). */
void display_want_screen(const char *why);
/* QuickDraw drew rectangle r (global) on the screen: a program filling much
 * of it outside its windows (Solarian II, through a port of its own) wants
 * the whole screen. */
void display_note_screen_draw(qrect r);
void display_sync(void); /* the host's snapshots, at a safe point */
void drag_host_pump(void); /* drags from the host, at the program's event calls */
uint32_t qd_screen_row_bytes(void);
int qd_screen_depth(void);
bool driver_refnum(int16_t refnum);
int16_t driver_call(gaddr pb, bool async, int kind);
/* MacTCP (mactcp.c): the .IPP driver, its Domain Name Resolver, and the
 * polling that moves its streams on at interrupt time. */
extern const native_driver mactcp_driver;
void mactcp_poll(void);
void mactcp_reset(void);
void mactcp_install_resolver(void);
/* The Sony driver (.Sony) for a raw floppy disk image, key-disk checks (sony.c). */
extern const native_driver sony_driver;
void sony_install_drive(gaddr zone, int16_t drive);
void sony_reset(void);
int16_t driver_drive_status(gaddr pb);
/* The Sound Driver (.Sound) of the first Macs (sound.c). */
extern const native_driver sound_driver;
/* AppleTalk (appletalk.c): the .MPP driver on LocalTalk over UDP. */
extern const native_driver mpp_driver, atp_driver, xpp_driver, dsp_driver;
void adsp_poll(void);
void adsp_reset(void);
/* For ATP (appletalk_atp.c): DDP on the node appletalk.c runs. */
typedef void (*at_handler)(uint8_t skt, uint8_t snode, uint8_t sskt, uint8_t type, const uint8_t *data, int len);
int16_t at_open(void); /* .MPP's open: the link and a node */
bool at_opened(void);
uint8_t at_node(void);
int at_open_socket(int want, at_handler h);
void at_close_socket(int s);
void at_send_ddp(uint8_t dnode, uint8_t dskt, uint8_t sskt, uint8_t type, const uint8_t *data, int n);
void at_complete(gaddr pb, int16_t r);
void atp_poll(void);
/* For host code (ASP, asp.c): an NBP lookup, and ATP from C. */
int16_t at_nbp_lookup(const char *obj, const char *type, const char *zone, int tries, int interval_ms,
                      uint16_t *net, uint8_t *node, uint8_t *skt);
/* For Open Transport (ot_appletalk.c): lookups that run on with the stack
 * (each answer's tuple to fn: network, node, socket, enumerator, entity),
 * names on host code's sockets, and its endpoints hearing this node. */
typedef void (*at_nbp_fn)(void *ctx, const uint8_t *tuple, int len);
int at_nbp_start(const uint8_t *packed, int len, int tries, int interval_ms, at_nbp_fn fn, void *ctx);
bool at_nbp_busy(int handle);
void at_nbp_stop(int handle);
gaddr at_register_name(const uint8_t *packed, int len, uint8_t skt);
void at_remove_name(gaddr nte);
bool at_name_taken(const uint8_t *packed, int len);
void at_hold_selfsend(bool on);
uint16_t at_network(void);
/* The AppleTalk Transition Queue: entries added and removed, events sent. */
int16_t at_transition_queue(gaddr entry, bool add);
int32_t at_transition_event(int32_t event, gaddr info, bool preflight, int32_t cancel);
/* ADSP's driver calls from C, on a parameter block in guest memory. */
int16_t adsp_control(gaddr pb, bool async);
typedef struct {
    int16_t result; /* ioInProgress while waiting */
    int count;      /* responses: the highest sequence number got, plus 1 */
    uint8_t data[8][578];
    int len[8];
    uint32_t user[8];
} atp_reply;
typedef void (*atp_request_fn)(void *ctx, uint8_t skt, uint8_t node, uint8_t rskt, uint16_t tid, uint32_t user,
                               const uint8_t *data, int len);
int atp_host_open(atp_request_fn fn, void *ctx); /* a dynamic socket, or 0 */
void atp_host_close(int skt);
int16_t atp_host_request(int skt, uint8_t node, uint8_t dskt, uint32_t user, const uint8_t *data, int len, bool xo,
                         int nresp, int timeout_ms, int retries, atp_reply *out);
/* The same without waiting: out->result stays ioInProgress until it ends. */
int16_t atp_host_start(int skt, uint8_t node, uint8_t dskt, uint32_t user, const uint8_t *data, int len, bool xo,
                       int nresp, int timeout_ms, int retries, atp_reply *out);
bool atp_host_xo(int skt, uint8_t node, uint8_t rskt, uint16_t tid); /* a request heard was exactly-once */
void atp_host_cancel(atp_reply *out);
void atp_host_respond(int skt, uint8_t node, uint8_t rskt, uint16_t tid, int count, const uint8_t (*data)[578],
                      const int *lens, const uint32_t *users);
void atp_host_send_once(int skt, uint8_t node, uint8_t dskt, uint32_t user, const uint8_t *data, int len);
/* EtherTalk (ethertalk.c), for --appletalk bridge:IF. */
bool et_open(const char *ifname);
int et_fd(void);
int et_heard(void); /* AppleTalk frames heard from other machines */
const char *at_bridge_interface(void);
void et_close(void);
void et_send(const uint8_t *llap, int len);
int et_receive(uint8_t *llap, int cap);
void et_acquire(uint32_t seed, uint16_t *net, uint8_t *node);
void et_use_fake(int fd, const uint8_t hw[6]);
bool et_interface_mac(const char *ifname, uint8_t out[6]);
bool host_interface_mac(const char *ifname, uint8_t out[6]); /* hostmac.c: IOKit's */
void atp_reset(void);
void appletalk_poll(void);
void appletalk_reset(void);
bool appletalk_enabled(void);
/* The Serial Driver (serial.c): .AIn, .AOut, .BIn and .BOut. */
extern const native_driver serial_drivers[4];
void serial_poll(void);
void serial_reset(void);
/* The ports from C, for the Communications Toolbox's tools: port 0 is the
 * modem port, 1 the printer port. */
void serial_port_open(int port);
void serial_port_close(int port);
int serial_port_read(int port, uint8_t *b, int max);
int serial_port_available(int port);
void serial_port_write(int port, const uint8_t *b, int n);
void serial_port_setup(int port, uint32_t baud, int data_bits, int parity, int stop_bits, int handshake);
void serial_port_break(int port, bool on);
void serial_port_dtr(int port, bool on);
int serial_port_fd(int port);
bool serial_port_online(int port);
bool serial_port_in_use(int port);
/* The Communications Toolbox (ctb.c): _CommToolboxDispatch and the
 * managers' routines by selector, for 68k and PowerPC programs alike. */
void ctb_reset(void);
void ctb_poll(void); /* connections moved on, completion routines called */
void crm_install(gaddr rec); /* CRMInstall: a device record queued, its ID given */
gaddr crm_search(gaddr req); /* CRMSearch: the next record of a type after an ID */
uint32_t ctb_call(uint16_t selector, const uint32_t *args, int nargs);
void ppc_glue_reset(void); /* ppc_glue.c */
void ppc_libs_reset(void); /* ppc_libs.c */
void sound_converters_reset(void); /* sndconvert.c */
void carbon_reset(void); /* carbon.c */
void ilutil_reset(void); /* InterfaceLib's utilities (ilutil.c) */
/* Whether every Pascal form InterfaceLib's C forms call is there; the
 * missing named (cglue.c). */
bool cglue_check(char *missing, size_t size);
/* An open resource file's fork, by its reference number (rsrcmgr.c). */
bool rm_fork_of(int16_t rn, const uint8_t **data, uint32_t *len);
bool rm_host_path(int16_t rn, char *out, size_t outsize);
/* The File Manager writing a resource file's fork by its reference number:
 * the bytes from pos (zeros up to it past the end), or a new length. */
int16_t rm_fork_write(int16_t rn, uint32_t pos, const uint8_t *src, uint32_t n);
int16_t rm_fork_set_eof(int16_t rn, uint32_t len);
bool gestalt_exists(uint32_t selector);
void gestalt_delete(uint32_t selector);
void wm_hide_menu_bar(bool hide);
bool wm_is_collapsed(gaddr w);
bool wm_set_collapsed(gaddr w, bool on); /* false when the window can't collapse */
uint32_t wm_window_features(gaddr w);
/* For WindowsLib (os9_toolbox.c): floating windows highlighted or not,
 * the menu bar's height, and the desk's region made again. */
void wm_set_floats_active(bool on);
bool wm_floats_active(void);
void wm_set_menu_bar_height(int16_t h);
void wm_rebuild_desk(void);
/* For MenusLib: the standard menu definition and the menu bar's, and a
 * menu's own definition called. */
void menu_standard_mdef(int16_t msg, gaddr mh, gaddr rect, uint32_t hit, gaddr which);
int32_t menu_standard_mbdf(int16_t msg, int16_t param1, int32_t param2);
void menu_call_def(gaddr mh, int16_t msg, gaddr rect, uint32_t hit, gaddr which);
void menu_skip_draws(int n);
bool menu_item_bounds(gaddr mh, int item, qrect *out);
int menu_bar_titles_end(void);
/* The menu bar clock (MenusLib's), drawn with the bar when it's on. */
void os9tbx_draw_clock(void);
/* The Control Manager's Drag Manager handlers (carbon_controls.c). */
gaddr ctl_drag_tracking_handler(void);
gaddr ctl_drag_receive_handler(void);
bool cfm_launch(const char *app_path, char *err, size_t errlen);
bool cfm_app_fragment_loadable(const char *path);
/* Load a PEF container in memory (an accelerated code resource's) once;
 * returns its main transition vector, or 0. */
gaddr cfm_prepare_container(gaddr pef);
/* A transition vector whose code runs fn natively. */
gaddr cfm_native_tvector(void (*fn)(struct ppc_cpu *), const char *name);

/* The Apple Event Manager (aem.c) and the Object Support Library (osl.c). */
/* Call a handler's universal procedure pointer (either ISA, or a bare
 * transition vector from PowerPC code). */
uint32_t aem_call_proc(gaddr upp, uint32_t procinfo, const uint32_t *args, int n);
/* The application's special handler for key, else the system's; 0 if none. */
gaddr aem_special_handler(uint32_t key);
/* A descriptor's data flattened for another process (malloc'd), and such
 * an event queued for the application, its reply to `reply`. */
uint8_t *aem_flatten(gaddr desc, uint32_t *len_out);
void aem_queue_flat(const uint8_t *data, uint32_t len, machine_ae_reply_fn reply, void *ctx);
/* An event from the host, flattened likewise, without guest memory. */
uint8_t *aem_build_flat(uint32_t cls, uint32_t id, const char *const *files, int nfiles, const char *text,
                        bool reply, uint32_t *len_out);
/* Another Blue Box process (aem_remote.c): a flattened event to the one
 * an address names ('sign', 'kpid' or 'targ'); with kAEWaitReply, its
 * reply.  procNotFound when none listens. */
int16_t aem_remote_send(const uint8_t *addr, uint32_t addr_type, uint32_t addr_len, const uint8_t *event,
                        uint32_t len, int mode, int32_t timeout, uint8_t **reply, uint32_t *reply_len);
/* A flattened event to process pid (with signature sig), waiting at most
 * timeout_ms (negative: for ever) for its reply; the handler's result. */
int16_t machine_ae_send(int pid, uint32_t sig, const uint8_t *event, uint32_t len, int mode, int timeout_ms,
                        uint8_t **reply, uint32_t *reply_len);
void osl_reset(void);

/* Carbon's small additions to older managers (carbon_gaps.c's exports). */
int32_t tb_AEGetDescDataSize(gaddr desc);
int16_t tb_AEGetDescData(gaddr desc, gaddr data, int32_t max_size);
void tb_MoreMasterPointers(uint32_t count);

/* Mixed Mode (mixedmode.c). */
void mixedmode_init(void);
bool mm_is_descriptor(gaddr p);
/* A routine of Blue Box's own: 68k code in the ROM's native stubs, never a
 * transition vector, whichever ISA asks for it. */
bool mm_is_native(gaddr p);
/* A procedure pointer that 68k code run from PowerPC installed (noted by
 * the calls that install handlers): 68k code, though PowerPC code runs. */
void mm_note_proc(gaddr p);
bool mm_is_68k_proc(gaddr p);
/* CarbonLib's IsPointerValid, IsHandleValid and IsHeapValid (memmgr.c). */
bool mm_pointer_valid(gaddr p);
bool mm_handle_valid(gaddr h);
bool mm_heap_valid(void);
/* The machine location's gmtDelta field, flags byte included (ppc_libs.c). */
int32_t machine_gmt_delta(void);
/* Call a universal procedure pointer (either ISA) with arguments as the
 * procInfo describes; returns the result. */
uint32_t mm_call(gaddr upp, uint32_t procinfo, const uint32_t *args, int nargs);
/* A new one-routine descriptor (CarbonLib's NewXxxUPP calls). */
gaddr mm_new_descriptor(gaddr proc, uint32_t procinfo, int isa);
/* InterfaceLib's CallComponentUPP variable (a descriptor in it). */
gaddr mm_call_component_upp(void);
/* Run the PowerPC routine of a transition vector with these arguments;
 * returns its r3. */
uint32_t ppc_call(gaddr tvector, const uint32_t *args, int nargs);

/* What a running thread keeps besides its registers: how deep it is in
 * calls to guest code and in traps run for PowerPC code. */
typedef struct {
    int call_depth, returned_depth;
    uint16_t current_trap, last_trapword;
} trap_ctx;
void trap_ctx_save(trap_ctx *t);
void trap_ctx_load(const trap_ctx *t);

#define TRAP_DEPTH 32
typedef struct {
    uint32_t d[8], a[8];
    uint16_t sr;
} ppc_trap_regs;
typedef struct {
    int depth;
    ppc_trap_regs saved[TRAP_DEPTH];
    bool finished[TRAP_DEPTH];
} ppc_trap_ctx;
void ppc_trap_ctx_save(ppc_trap_ctx *t);
void ppc_trap_ctx_load(const ppc_trap_ctx *t);
/* Open Transport (opentransport.c). */
void opentransport_reset(void);
/* Open Transport for 68k code (ot_68k.c): the Shared Library Manager the glue finds, made
 * ready when 68k code asks Gestalt for Open Transport. */
void ot_68k_prepare(void);
void ot_68k_reset(void);
/* A routine 68k code gave Open Transport to call later is 68k code, whichever ISA the program runs. */
bool ot_is_68k_proc(gaddr p);
bool ot_68k_find(const char *name, char lib[64], int *index, char *conv, char sig[24], char *ret);
/* Open Transport's DNS queries ask this name server (an address and port) instead of the
 * host's; 0 undoes it (the tests', and a program's environment). */
void ot_inet_set_dns(uint32_t ip, uint16_t port);
void opentransport_poll(void);
bool mm_ctx_returned(void);
void mm_ctx_set_returned(bool r);

#endif
