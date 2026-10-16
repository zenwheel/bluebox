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

#endif
