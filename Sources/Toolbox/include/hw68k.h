/*
 * The hardware a classic Mac's programs touch directly: the machine being
 * presented (a profile), and the memory-mapped devices of its I/O space.
 */
#ifndef HW68K_H
#define HW68K_H

#include <stdbool.h>
#include <stdint.h>

#include "mem.h"

/* A machine as Blue Box can present it.  The default is the Quadra 800
 * every run has been; the others are the classic Macs whose programs
 * address the hardware by number. */
typedef struct {
    const char *name;          /* "plus", "se", "classic", "macii", "quadra" */
    const char *title;         /* "Macintosh Plus" */
    int gestalt_mach;          /* gestaltMachineType (SysEnvirons' is two less) */
    int cpu;                   /* 68000, 68020, 68030 or 68040 */
    bool fpu;
    bool color_qd;
    int env_keyboard;          /* SysEnvirons keyBoardType */
    int gestalt_kbd;           /* gestaltKeyboardType */
    uint16_t rom85;            /* ROM85: bit 15 set on the 64K ROM, bit 14 on pre-Color QuickDraw ROMs */
    uint16_t hwcfg;            /* HWCfgFlags (SCSIFlags) */
    uint16_t rom_version;      /* the ROM version word */
    uint16_t sys_version;      /* the System Blue Box claims on it */
    int screen_w, screen_h, screen_depth; /* the built-in screen; 0 keeps the run's */
    bool compact_io;           /* the 24-bit I/O map of the Plus and the SE: VIA at $EFE1FE ... */
    bool via_sound;            /* the sound buffer the VIA's video circuitry plays (compact Macs) */
    bool adb;                  /* ADB rather than the keyboard on the shift register */
    gaddr via, scc_rd, scc_wr, iwm; /* the devices' addresses */
} hw_profile;

const hw_profile *hw_find_profile(const char *name);
const hw_profile *hw_profile_at(int i); /* NULL past the last */
/* Choose the machine (before machine_boot); false if unknown. */
bool hw_select_machine(const char *name);
const hw_profile *hw_machine(void);
/* Is a machine other than the default selected? */
bool hw_machine_chosen(void);

/* Boot: the devices at their addresses, low memory pointing at them. */
void hw_boot(void);
/* Interrupt time (time_pump): the VIA's timers and interrupts, the sound
 * buffer, the serial lines. */
void hw_pump(void);
/* The sound buffer(s) and screen pages, as the compact Macs lay them out
 * below the top of memory. */
gaddr hw_sound_buffer(bool alternate);
gaddr hw_screen_page(bool alternate);

/* The VIA and SCC, for tests and for the rest of the Toolbox. */
uint8_t hw_via_read(int reg);
void hw_via_write(int reg, uint8_t v);
/* Which screen page the video circuitry shows (VIA port A bit 6). */
bool hw_screen_alternate(void);
bool hw_sound_alternate(void);
int hw_sound_volume(void);
bool hw_sound_enabled(void);

#endif
