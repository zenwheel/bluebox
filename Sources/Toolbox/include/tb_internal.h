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
/* Mac full path (MacRoman) of a host path; returns its length, 0 if too long. */
size_t fm_mac_path_of_host(const char *host, uint8_t *out, size_t outsize);
/* Host path of a Mac full path. */
bool fm_host_of_mac_path(const uint8_t *mac, size_t n, char *out, size_t outsize);

/* ---- String comparison (register traps) ---- */

void raw_CmpString(cpu68k *c, uint16_t tw);
void raw_RelString(cpu68k *c, uint16_t tw);
void raw_UprString(cpu68k *c, uint16_t tw);

/* ---- Sound Manager ---- */

/* Run sound commands whose time has come (at safe points). */
void sound_pump(void);
void sound_beep(void);
void sound_reset(void);

/* ---- Appearance ---- */

/* MACHINE_APPEARANCE_*: how the standard definition procedures draw. */
extern int g_appearance;
/* Colour artwork (appearance.c); the app_draw_* calls return false or do
 * nothing when the black-and-white look applies. */
#include "qd_internal.h"
bool app_colour(void);
void app_draw_document(gaddr w, qrect c, qrect t, bool hilited, bool goaway, bool zoom, int param);
void app_draw_grow(qrect c, bool hilited);
void app_draw_scrollbar(qrect r, bool vertical, int hilite, bool enabled, bool active, qrect thumb);
bool app_draw_button(qrect r, const uint8_t *title, int n, bool pressed, bool disabled);
bool app_draw_check(qrect box, bool radio, bool on, bool pressed, bool disabled);

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
