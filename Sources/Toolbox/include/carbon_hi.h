/*
 * CarbonLib's drawing, windows and menus: its QuickDraw, Window Manager,
 * Menu Manager, List Manager, TextEdit, Appearance, Font Manager and help
 * tag calls, and its routines that make, call and dispose of those
 * managers' universal procedure pointers (carbon_qd.c, carbon_wm.c,
 * carbon_menu.c, carbon_ui.c, carbon_upp.c).
 */
#ifndef CARBON_HI_H
#define CARBON_HI_H

#include <stdbool.h>
#include <stdint.h>

#include "mem.h"
#include "ppc_bridge.h"
#include "qd_internal.h"

/* (In the files of the exports themselves) an export's arguments: r3
 * up, the ninth on in the caller's parameter area; its result, and its
 * return. */
#ifdef CARBON_HI_EXPORTS
#define R(n) ((n) < 8 ? c->gpr[3 + (n)] : rd32(c->gpr[1] + 24 + 4 * (uint32_t)(n)))
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)
#define DONE() (c->pc = c->lr)
#endif

/* The function behind one of these exports, or NULL. */
void (*carbonhi_export_fn(const char *name))(ppc_cpu *);

extern const ppc_export carbonqd_exports[], carbonwm_exports[], carbonmenu_exports[], carbonui_exports[],
    carbonupp_exports[];
extern const unsigned carbonqd_export_count, carbonwm_export_count, carbonmenu_export_count,
    carbonui_export_count, carbonupp_export_count;

/* The Window Manager's side (wm.c): whether a pointer is a window, the
 * part a global point is in, the zoom states, and a window given another
 * definition or variation. */
bool wm_is_window(gaddr w);
int16_t wm_hit_part(gaddr w, uint32_t p);
gaddr wm_zoom_state(gaddr w);
qrect wm_std_state(void);
void wm_redefine(gaddr w, gaddr defproc, int16_t var);
/* The procID CreateNewWindow gives a class and attributes, or -1. */
int16_t wm_class_proc(uint32_t windowClass, uint32_t attributes);

/* And what it asks of carbon_wm.c: a definition RegisterWindowDefinition
 * gave a resource ID (or 0), and a window's uncovered content (global)
 * painted by the program's own procedure (false when it has none). */
gaddr carbonwm_registered_def(int16_t id);
bool carbonwm_paint_content(gaddr w, const hrgn *content);
/* The class a window was made with or given (0: none, the Window Manager
 * works it out from the definition), set when it's made; and a window
 * gone. */
uint32_t carbonwm_class(gaddr w);
/* The attributes kept beside what the definition draws (no updates, the
 * standard handler ...). */
uint32_t carbonwm_attributes(gaddr w);
void carbonwm_set_class(gaddr w, uint32_t cls, uint32_t attributes);
void carbonwm_forget(gaddr w);

/* CarbonLib's fields of menus and items, kept with the Menu Manager's own
 * (menu.c), and moved with an item as items come and go. */
typedef struct hi_prop {
    uint32_t creator, tag, attributes, size;
    uint8_t *data;
    struct hi_prop *next;
} hi_prop;

typedef struct {
    uint32_t attributes, indent;
    uint16_t virtual_key;
    bool invalid;
    hi_prop *props;
} hi_item;

typedef struct {
    uint32_t attributes, retain, title_icon_type;
    gaddr title_icon;
    bool retained, invalid_size;
} hi_menu;

void hi_item_free(hi_item *h);
void hi_menu_free(hi_menu *h);
hi_menu *menu_hi_menu(gaddr mh, bool create);
hi_item *menu_hi_item(gaddr mh, int item, bool create);
bool menu_set_title(gaddr mh, gaddr title);
/* The list definition LNew is to use for an LDEF ID: CreateCustomList's
 * procedure for the list it's making, or one RegisterListDefinition
 * registered (or 0). */
gaddr carbonui_list_def(int16_t id);
/* A definition RegisterMenuDefinition gave an MDEF ID (or 0). */
gaddr carbonmenu_registered_def(int16_t id);

/* CFStrings, through Core Foundation's own exports: one made from a
 * Pascal string (Mac Roman), one's text into a Str255 (as much of it as
 * Mac Roman has), retained and released. */
gaddr hi_cf_from_pstr(gaddr pstr);
void hi_cf_to_pstr(gaddr cf, gaddr out);
void hi_cf_retain(gaddr cf);
void hi_cf_release(gaddr cf);

/* Call one of the libraries' exports (a Carbon Event Manager call, say)
 * with these arguments; its r3, and whether it was there. */
uint32_t hi_call(const char *name, const uint32_t *args, int nargs, bool *found);

/* Whether the program runs on CarbonLib (cfm.c): its windows are all
 * colour windows, as CarbonLib's were. */
bool cfm_imports_carbonlib(void);

/* For a new launch. */
void carbonhi_reset(void);
void carbonqd_reset(void);
void carbonwm_reset(void);
void carbonmenu_reset(void);
void carbonui_reset(void);

#endif
