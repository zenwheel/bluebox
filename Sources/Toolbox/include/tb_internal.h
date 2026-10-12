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
static inline void set_res_err(int16_t e) { wr16(LM_ResErr, (uint16_t)e); }

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
uint32_t seg_launch(int16_t app_refnum, uint32_t *a5_out, char *err, size_t errlen);

/* ---- Gestalt ---- */

void gestalt_init(void);

/* ---- File Manager ---- */

/* An FSSpec for a host path (the file need not exist). */
void fm_spec_for_path(const char *path, gaddr spec);
/* Host path for a (vRefNum, dirID, name) triple. */
bool fm_host_path(int16_t vref, int32_t dirid, gaddr name, bool hfs, char *out, size_t outsize);
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
/* Mouse button and location into low memory (events.c). */
void events_sync_lowmem(void);
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
bool native_draw_scrollbar(qrect r, bool vertical, int hilite, bool enabled, bool active, int value, int min, int max);

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

#endif
