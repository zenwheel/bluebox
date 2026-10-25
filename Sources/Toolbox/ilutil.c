/*
 * InterfaceLib's utility calls that PowerPC programs import by name with
 * no 68k trap behind them to glue to: the tables of the files that make
 * them (Driver Services' time and atomic operations, low memory, ...),
 * and those small enough to live here.
 */
#include <string.h>

#include "ppc_bridge.h"
#include "tb_internal.h"

extern const ppc_export dsl_exports[];
extern const unsigned dsl_export_count;

typedef struct {
    const ppc_export *table;
    const unsigned *count;
} export_table;

static const export_table tables[] = {
    { dsl_exports, &dsl_export_count },
};

void (*ilutil_export_fn(const char *name))(ppc_cpu *)
{
    for (unsigned t = 0; t < sizeof tables / sizeof tables[0]; t++)
        for (unsigned i = 0; i < *tables[t].count; i++)
            if (!strcmp(tables[t].table[i].name, name))
                return tables[t].table[i].fn;
    return NULL;
}
