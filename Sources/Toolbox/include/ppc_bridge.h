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
/* Copy n bytes of guest memory (a NULL destination or source copies nothing). */
void ppc_copy(gaddr dst, gaddr src, uint32_t n);

#endif
