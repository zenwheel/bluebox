/*
 * The PowerPC side of the Toolbox: InterfaceLib's exports (generated glue,
 * ppc_glue_gen.c) and the Code Fragment Manager (cfm.c).
 */
#ifndef PPC_BRIDGE_H
#define PPC_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ppc.h"

typedef struct {
    const char *name;
    void (*fn)(ppc_cpu *c);
} ppc_export;

/* Sorted by name. */
extern const ppc_export ppc_exports[];
extern const unsigned ppc_export_count;
/* Hand-written (ppc_glue.c): the File Manager's glue calls. */
extern const ppc_export ppc_glue_exports[];
extern const unsigned ppc_glue_export_count;
/* The Code Fragment Manager's own calls (cfm.c). */
extern const ppc_export cfm_exports[];
extern const unsigned cfm_export_count;
/* Libraries' functions and data (ppc_libs.c): MathLib, StdCLib and the
 * InterfaceLib calls that are glue rather than traps. */
extern const ppc_export lib_exports[];
extern const unsigned lib_export_count;
/* The function that implements an export, or NULL. */
void (*cfm_export_fn(const char *name))(ppc_cpu *);
/* The address of an exported data symbol (allocated on first use), or 0. */
gaddr ppc_data_export(const char *name);
bool ppc_data_export_known(const char *name);
/* MathLib's long double, classification, environment and decimal calls
 * (mathlib.c), and its data (_FE_DFL_ENV). */
extern const ppc_export ml_exports[];
extern const unsigned ml_export_count;
bool ml_data_export_known(const char *name);
gaddr ml_data_export(const char *name);
/* StdCLib, MPW's C library (stdclib.c, stdclib_io.c, stdclib_stdio.c): its
 * routines, its data, a new program's library, and the streams flushed and
 * closed when a program ends without exit. */
void (*stdclib_export_fn(const char *name))(ppc_cpu *);
bool stdclib_data_known(const char *name);
gaddr stdclib_data_export(const char *name);
void stdclib_reset(void);
void stdclib_terminate(void);
/* The Sound Converter and ParseSndHeader (sndconvert.c). */
extern const ppc_export snd_exports[];
extern const unsigned snd_export_count;
/* CarbonLib's Core Foundation basics and accessors (carbon.c). */
extern const ppc_export carbon_exports[];
extern const unsigned carbon_export_count;
/* The Carbon Event Manager (carbon_events.c) and Multiprocessing
 * Services (multiprocessing.c). */
extern const ppc_export ce_exports[];
extern const unsigned ce_export_count;
extern const ppc_export mp_exports[];
extern const unsigned mp_export_count;
/* CarbonLib's controls and dialogs (carbon_controls.c). */
extern const ppc_export carbon_ctl_exports[];
extern const unsigned carbon_ctl_export_count;
extern const ppc_export carbonupps_exports[];
extern const unsigned carbonupps_export_count;
/* The low-memory accessors beyond gen.py's (lowmem_acc_data.c). */
extern const ppc_export lmacc_exports[];
extern const unsigned lmacc_export_count;
extern const ppc_export carbongaps_exports[];
extern const unsigned carbongaps_export_count;
/* Routines the archive walk found missing (vam.c). */
extern const ppc_export vam_exports[];
extern const unsigned vam_export_count;
/* Weak imports the archive walk's PowerPC programs called unchecked (ppc_walk.c). */
extern const ppc_export walk_exports[];
extern const unsigned walk_export_count;
/* QuickTimeLib's sequence grabber, video digitizer and movie import and
 * export component calls (qt_capture.c). */
extern const ppc_export qtcapture_exports[];
extern const unsigned qtcapture_export_count;
/* Mac OS 9's private names Blue Box can back (private_os9.c). */
extern const ppc_export priv_os9_exports[];
extern const unsigned priv_os9_export_count;
void priv_os9_reset(void);
/* QuickTime's graphics importers (qt_gimport.c). */
extern const ppc_export gimport_exports[];
extern const unsigned gimport_export_count;
/* CarbonLib's user and machine names (carbon_user.c). */
extern const ppc_export carbonuser_exports[];
extern const unsigned carbonuser_export_count;
/* CarbonLib's printing, Navigation, scrap, ATSUI and MLTE (carbon_svc.c). */
void (*carbonsvc_export_fn(const char *name))(ppc_cpu *);
/* Its remaining managers: ColorSync, HTML Rendering ... (carbonrest.c). */
void (*carbonrest_export_fn(const char *name))(ppc_cpu *);
/* Its Core Foundation and Math64 (carbon_cf*.c): routines, data, and the
 * state a launch starts without. */
void (*carbon_cf_export_fn(const char *name))(ppc_cpu *);
bool carbon_cf_data_known(const char *name);
gaddr carbon_cf_data_export(const char *name);
void carbon_cf_reset(void);
/* DrawSprocket (drawsprocket.c). */
extern const ppc_export dsp_exports[];
extern const unsigned dsp_export_count;
/* InputSprocket (inputsprocket.c). */
extern const ppc_export isp_exports[];
extern const unsigned isp_export_count;
void inputsprocket_reset(void);
/* Open Transport (opentransport.c). */
extern const ppc_export ot_exports[];
extern const unsigned ot_export_count;
void ot_unsupported(ppc_cpu *c); /* the rest of Open Transport: kOTNotSupportedErr */
/* Open Transport's AppleTalk (ot_appletalk.c). */
extern const ppc_export ota_exports[];
extern const unsigned ota_export_count;
/* Its PAP and ASP endpoints' transactions (ot_session.c). */
extern const ppc_export ots_exports[];
extern const unsigned ots_export_count;
/* The Communications Toolbox (ctb.c); names of routines that are 68k only
 * are empty. */
extern const ppc_export ctb_exports[];
extern const unsigned ctb_export_count;
/* The Text Encoding Converter: TextCommon, UnicodeConverter and
 * TextEncodingConverter (tec.c). */
extern const ppc_export tec_exports[];
extern const unsigned tec_export_count;
/* InterfaceLib's utilities: Driver Services' time and atomic operations,
 * low memory, the Text Services, Dictionary and Display Managers and the
 * rest (ilutil.c gathers their tables). */
void (*ilutil_export_fn(const char *name))(ppc_cpu *);

/* The HFS Plus calls' hand-written exports (hfsplus.c). */
extern const ppc_export hfs_exports[];
extern const unsigned hfs_export_count;
/* Mixed Mode (mixedmode.c). */
extern const ppc_export mm_exports[];
extern const unsigned mm_export_count;

/* Glue that runs a 68k trap (ppc_glue.c): begin saves the 68k registers,
 * set/push lay out the arguments (registers 0-7 are D0-D7, 8-15 A0-A7;
 * pushes of 1 byte take a word, the byte high), ppc_trap runs the trap
 * through the A-line dispatcher, get/result read what it left (result:
 * the function result on top of the stack), end restores the registers. */
void ppc_trap_begin(void);
void ppc_trap_set(int reg, uint32_t value);
uint32_t ppc_trap_get(int reg);
void ppc_trap_push(int size, uint32_t value);
void ppc_trap(uint16_t trapword);
uint32_t ppc_trap_result(int size);
void ppc_trap_end(void);
/* Whether the trap word at pc is one this glue runs for PowerPC code. */
bool ppc_trap_word_at(gaddr pc);
/* Whether the program has patched a Toolbox trap: its glue then runs the
 * trap through the table instead of calling our function. */
bool ppc_trap_patched(uint16_t trapword);
/* A small struct result (NumVersion, Point): written through the pointer a
 * caller passed in r3 (result, r3 on entry), when it points into the
 * caller's stack as such a buffer does, and also left in r3 itself, where
 * other compilers' callers take a four-byte struct (Shockwave). */
void ppc_struct_result(ppc_cpu *c, gaddr result, uint32_t value, int size);
/* Copy n bytes of guest memory (a NULL destination or source copies nothing). */
void ppc_copy(gaddr dst, gaddr src, uint32_t n);
/* A parameter block call finished with err (trap.c): its ioResult set, and
 * when asynchronous its completion routine queued, as the trap dispatcher
 * does for 68k callers.  Returns what the caller gets back. */
int16_t pb_call_done(gaddr pb, int16_t err, bool async);

#endif
