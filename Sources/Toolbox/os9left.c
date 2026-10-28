/*
 * The lookup over the Mac OS 9 libraries' leftover exports (os9left_*.c).
 */
#include <string.h>

#include "os9left.h"

#define G(n) { os9left_##n##_exports, &os9left_##n##_export_count, os9left_##n##_reset }

static const struct {
    const ppc_export *exports;
    const unsigned *count;
    void (*reset)(void);
} groups[] = {
    G(text), G(mp), G(display), G(threads),
};

void (*os9left_export_fn(const char *name))(ppc_cpu *)
{
    for (unsigned g = 0; g < sizeof groups / sizeof groups[0]; g++)
        for (unsigned i = 0; i < *groups[g].count; i++)
            if (!strcmp(groups[g].exports[i].name, name))
                return groups[g].exports[i].fn;
    return NULL;
}

void os9left_reset(void)
{
    for (unsigned g = 0; g < sizeof groups / sizeof groups[0]; g++)
        groups[g].reset();
}
