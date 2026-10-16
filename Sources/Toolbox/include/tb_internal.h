/* Shared declarations for the Toolbox implementation (not a public API). */
#ifndef TB_INTERNAL_H
#define TB_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "m68k.h"
#include "log.h"
#include "lowmem_gen.h"
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
uint32_t seg_launch(int16_t app_refnum, uint32_t *a5_out, char *err, size_t errlen);

/* ---- Gestalt ---- */

void gestalt_init(void);
uint32_t gestalt_value(uint32_t selector, uint32_t fallback);

/* ---- File Manager ---- */

/* An FSSpec for a host path (the file need not exist). */
void fm_spec_for_path(const char *path, gaddr spec);
/* Host path for a (vRefNum, dirID, name) triple. */
bool fm_host_path(int16_t vref, int32_t dirid, gaddr name, bool hfs, char *out, size_t outsize);
/* Classic file type of a host file (0 if unknown). */
uint32_t fm_type_of_path(const char *path);
/* The application (host path) that opens a document: one with the
 * document's creator, on the disk or in a granted folder. */
bool fm_app_for_document(const char *doc, char *app, size_t appsize);
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
/* Let the application see and change a host file (and its neighbours) or,
 * with tree, a folder and its contents (filemgr.c). */
void fm_grant(const char *path, bool tree);
bool fm_refnum_host_path(int16_t refnum, char *out, size_t outsize);
/* Components of our own (components.c). */
uint32_t components_register_native(uint32_t type, uint32_t subtype, uint32_t manufacturer, gaddr entry);
uint32_t components_open(uint32_t component);
void components_set_storage(uint32_t ci, gaddr storage);
void components_close(uint32_t ci);
void components_reset(void);
void quicktime_reset(void);
/* Mouse button and location into low memory (events.c). */
void events_sync_lowmem(void);
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
bool events_host_attached(void);
/* A null event in a headless run: counts towards the instruction limit. */
void machine_idle(void);
void time_reset(void);
/* MIDI Manager time base ports (midi.c), run from time_pump. */
void midi_pump(void);
void midi_reset(void);
void raw_RelString(cpu68k *c, uint16_t tw);
void raw_UprString(cpu68k *c, uint16_t tw);
void raw_LowerText(cpu68k *c, uint16_t tw);
void raw_DrvrInstall(cpu68k *c, uint16_t tw);
void raw_ReadXPRam(cpu68k *c, uint16_t tw);
void raw_WriteXPRam(cpu68k *c, uint16_t tw);
void raw_InitZone(cpu68k *c, uint16_t tw);
void raw_CursorDeviceDispatch(cpu68k *c, uint16_t tw);
void raw_SlotManager(cpu68k *c, uint16_t tw);
void raw_ComponentDispatch(cpu68k *c, uint16_t tw);
/* A MacRoman character's comparison key. */
int mac_char_key(uint8_t c, bool case_sens, bool ignore_marks);
/* Default US 'itl0' and 'itl1' resources. */
size_t intl_default_itl0(uint8_t out[32]);
size_t intl_default_itl1(uint8_t out[332]);

/* ---- Notification Manager ---- */

void notify_pump(void);
void notify_reset(void);

/* ---- Sound Manager ---- */

/* Run sound commands whose time has come (at safe points). */
void sound_pump(void);
void sound_beep(void);
void sound_reset(void);

/* ---- Appearance ---- */

/* MACHINE_APPEARANCE_*: how the standard definition procedures draw. */
extern int g_appearance;
/* Themes (theme.c): each returns false when no theme applies, and the
 * standard black-and-white drawing is used instead. */
#include "qd_internal.h"
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
bool native_draw_popup(qrect r, const uint8_t *item, int n, bool pressed, bool disabled);
bool native_draw_edit_frame(qrect r, bool focused);

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
extern bool g_ppc_mode;
bool cfm_has_ppc(void);
void gestalt_powerpc(void);
bool cfm_launch(const char *app_path, char *err, size_t errlen);
/* A transition vector whose code runs fn natively. */
gaddr cfm_native_tvector(void (*fn)(struct ppc_cpu *), const char *name);

/* Mixed Mode (mixedmode.c). */
void mixedmode_init(void);
bool mm_is_descriptor(gaddr p);
/* InterfaceLib's CallComponentUPP variable (a descriptor in it). */
gaddr mm_call_component_upp(void);
/* Run the PowerPC routine of a transition vector with these arguments;
 * returns its r3. */
uint32_t ppc_call(gaddr tvector, const uint32_t *args, int nargs);

#endif
