/*
 * The lookup over the Mac OS 9 libraries' leftover exports (os9left_*.c).
 */
#include <string.h>

#include "tb_internal.h"
#include "os9left.h"

#define G(n) { os9left_##n##_exports, &os9left_##n##_export_count, os9left_##n##_reset }

static const struct {
    const ppc_export *exports;
    const unsigned *count;
    void (*reset)(void);
} groups[] = {
    G(text), G(mp), G(display), G(threads), G(ui), G(sound), G(misc),
};

void (*os9left_export_fn(const char *name))(ppc_cpu *)
{
    for (unsigned g = 0; g < sizeof groups / sizeof groups[0]; g++)
        for (unsigned i = 0; i < *groups[g].count; i++)
            if (!strcmp(groups[g].exports[i].name, name))
                return groups[g].exports[i].fn;
    return NULL;
}

static gaddr scratch_cell;

void os9left_reset(void)
{
    scratch_cell = 0;
    for (unsigned g = 0; g < sizeof groups / sizeof groups[0]; g++)
        groups[g].reset();
}

/* Data: the Font Manager's scratch storage, a pointer to a block kept
 * for the run. */
bool os9left_data_known(const char *name) { return !strcmp(name, "gQDTextRenderingScratchStorage"); }

gaddr os9left_data_export(const char *name)
{
    if (strcmp(name, "gQDTextRenderingScratchStorage"))
        return 0;
    if (!scratch_cell) {
        scratch_cell = tb_NewPtr(4, true, true);
        gaddr block = tb_NewPtr(4096, true, true);
        if (scratch_cell)
            wr32(scratch_cell, block);
    }
    return scratch_cell;
}
