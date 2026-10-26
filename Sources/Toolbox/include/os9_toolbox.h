/*
 * The Mac OS 8.5 to 9 Toolbox libraries' own calls (os9_toolbox.c):
 * WindowsLib, ControlsLib, MenusLib and AppearanceLib's exports that
 * aren't InterfaceLib's or CarbonLib's under the same name.
 */
#ifndef OS9_TOOLBOX_H
#define OS9_TOOLBOX_H

#include <stdbool.h>
#include <stdint.h>

#include "ppc_bridge.h"

/* The function behind one of these exports, or NULL. */
void (*os9tbx_export_fn(const char *name))(ppc_cpu *);

/* A new run: the libraries' settings back to the System's. */
void os9tbx_reset(void);

/* The Appearance control panel's settings, which AppearanceLib's private
 * calls (and SetTheme) change and the Appearance Manager's getters
 * (appearance.c) and GetTheme report. */
enum { kThemeFontSystem, kThemeFontSmallSystem, kThemeFontViews };
typedef struct {
    char font[3][64];      /* the system, small system and views fonts */
    int16_t views_size;
    uint16_t arrow_style;  /* ThemeScrollBarArrowStyle */
    uint16_t thumb_style;  /* ThemeScrollBarThumbStyle */
    uint16_t check_style;  /* ThemeCheckBoxStyle */
    bool smoothing;        /* font smoothing, from smoothing_size points */
    uint16_t smoothing_size;
    bool sounds;           /* theme sounds (there being no sound track, none play) */
    uint32_t sound_mask;
    uint32_t dbl_click;    /* what double-clicking a title bar does: 0 nothing, 1 collapse */
} appearance_settings;
appearance_settings *ap_settings(void);
/* SetTheme's collection taken into the settings: noErr, or the error for
 * a font that isn't installed. */
int16_t ap_apply_theme(gaddr collection);

#endif
