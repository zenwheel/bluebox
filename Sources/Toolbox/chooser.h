/* The Chooser (chooser.c), its Chooser extensions (chooser_ext.c) and
 * AppleShare connections (chooser_afp.c). */
#ifndef CHOOSER_H
#define CHOOSER_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "forks.h"
#include "machine.h"
#include "qd_internal.h"

/* The run's settings the Chooser edits. */
typedef struct {
    char user[32];      /* MacRoman */
    char printer[512];  /* machine_set_printer's spec, UTF-8 */
    char folder[1024];  /* the print folder, UTF-8 */
} chooser_config;
extern chooser_config g_chooser;

enum { CH_MAX_DEVICES = 48, CH_MAX_FOUND = 128 };

typedef struct {
    char name[64];      /* MacRoman, as under its icon */
    int kind;
    bool appletalk;     /* dimmed while AppleTalk is inactive */
    int16_t icon;       /* our ICN# */
    char path[PATH_MAX]; /* a Chooser extension's file */
    uint32_t type;      /* its file type: PRES, PRER or RDEV */
    gaddr suite;        /* its icon */
} ch_device;

typedef struct {
    char name[33];      /* MacRoman */
    uint8_t addr[4];    /* AddrBlock: network, node, socket */
} ch_found;

typedef struct {
    gaddr dce, window, scratch;
    gaddr icon_bar, icon_action;
    gaddr button1, button2, radio_on, radio_off, at_on, at_off;
    gaddr list, zones, te;
    bool active, show_radios, button1_default, button1_needs_sel;
    qrect radio_rect;
    char radio_label[64];
    char label[256];
    int icon_top;
    ch_device devices[CH_MAX_DEVICES];
    int ndevices, current;
    char zone[33];      /* the zone looked in ("" this one) */
    int lookup;         /* the NBP lookup going on */
    char lookup_type[33];
    uint32_t lookup_ticks;
    ch_found found[CH_MAX_FOUND];
    int nfound;
    bool found_changed;
} chooser_state;
extern chooser_state g_ch;

/* chooser.c */
int16_t rm_open_path(const char *path, int8_t perm); /* rsrcmgr.c */
size_t chooser_system_resources(rsrc_spec *specs, size_t n, size_t max);
void chooser_reset(void);
int chooser_print_destination(machine_print_job *job);
bool chooser_laserwriter(char *nbp, size_t size); /* the LaserWriter chosen, as an NBP name */
gaddr ch_pstr(const char *mac);
gaddr ch_rect(qrect r);
void chooser_changed(const char *key, const char *value);
void chooser_set_printer(const char *spec);
void chooser_set_user(const char *mac);
void chooser_driver_name(char *out, size_t size);
void chooser_add_extension(const char *name, const char *path, uint32_t type);
bool chooser_device_dimmed(int i);
void chooser_set_label(const char *mac);
void chooser_layout_controls(const qrect rects[5], const char *b1, const char *b2, const char *on, const char *off,
                             bool radios);
void chooser_show_button(int which, bool show);
void chooser_update_buttons(void);
void chooser_new_list(int columns);
int chooser_list_rows(void);
int chooser_selected_row(void);
void chooser_select_row(int row, bool on);
void chooser_cell_text(int row, int col, char *out, size_t size);
int chooser_add_row(const char *mac, const void *col1, int col1_len);
void chooser_clear_rows(void);
void chooser_start_lookup(const char *type);
void chooser_get_selection(const char *was);
bool chooser_confirm(const char *mac_text, const char *ok);
void chooser_message(const char *mac_text);
void chooser_note_appletalk(void);

/* chooser_ext.c: Chooser extensions, called through their 'PACK' -4096 */
void chooser_find_extensions(void);
void chooser_ext_select(ch_device *d);
void chooser_ext_terminate(void);
void chooser_ext_get_selection(void);
void chooser_ext_selection_changed(int was, int now);
void chooser_ext_button(int which, int16_t modifiers);
void chooser_ext_click(uint32_t where, int16_t modifiers);
void chooser_ext_draw(void);
void chooser_ext_run(void);
void chooser_ext_forget(void);
void chooser_ext_reset(void);

/* pap.c: a job printed on a LaserWriter (its NBP name) as PostScript */
bool pap_print(const machine_print_job *job, const char *nbp);

/* chooser_afp.c: AppleShare */
void chooser_afp_connect_found(int row);
void chooser_afp_connect_address(void);

#endif
