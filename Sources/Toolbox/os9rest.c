/*
 * The lookup over the Mac OS 9 libraries' remaining exports (os9rest*.c).
 */
#include <string.h>

#include "os9rest.h"
#include "tb_internal.h"

#define GROUP(n) { os9rest_##n##_exports, &os9rest_##n##_export_count }

static const os9rest_group groups[] = {
    GROUP(math),
};

void (*os9rest_export_fn(const char *name))(ppc_cpu *)
{
    for (unsigned g = 0; g < sizeof groups / sizeof groups[0]; g++)
        for (unsigned i = 0; i < *groups[g].count; i++)
            if (!strcmp(groups[g].exports[i].name, name))
                return groups[g].exports[i].fn;
    return NULL;
}

bool os9rest_data_known(const char *name) { (void)name; return false; }
gaddr os9rest_data_export(const char *name) { (void)name; return 0; }

void os9rest_reset(void) {}
