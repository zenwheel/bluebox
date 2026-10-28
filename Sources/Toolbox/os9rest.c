/*
 * The lookup over the Mac OS 9 libraries' remaining exports (os9rest*.c).
 */
#include <string.h>

#include "os9rest.h"
#include "os9svc.h"
#include "tb_internal.h"

#define GROUP(n) { os9rest_##n##_exports, &os9rest_##n##_export_count }

static const os9rest_group groups[] = {
    GROUP(math),
    GROUP(hw),
    GROUP(locale),
};

/* Names that are other exports under a second name. */
static const struct { const char *name, *target; } aliases[] = {
    { "CloneIconRef", "AcquireIconRef" }, { "DisposeIconRef", "ReleaseIconRef" },
};

void (*cfm_export_fn(const char *name))(ppc_cpu *);

void (*os9rest_export_fn(const char *name))(ppc_cpu *)
{
    for (unsigned a = 0; a < sizeof aliases / sizeof aliases[0]; a++)
        if (!strcmp(aliases[a].name, name))
            return cfm_export_fn(aliases[a].target);
    for (unsigned g = 0; g < sizeof groups / sizeof groups[0]; g++)
        for (unsigned i = 0; i < *groups[g].count; i++)
            if (!strcmp(groups[g].exports[i].name, name))
                return groups[g].exports[i].fn;
    return NULL;
}

static const char *const data_names[] = {
    "KCItemRefArrayCallBacks",
};

bool os9rest_data_known(const char *name)
{
    for (unsigned i = 0; i < sizeof data_names / sizeof data_names[0]; i++)
        if (!strcmp(name, data_names[i]))
            return true;
    return false;
}

gaddr os9rest_data_export(const char *name)
{
    gaddr p = os9svc_keychain_data(name);
    return p;
}

void os9rest_reset(void)
{
    os9rest_debug_reset();
    os9rest_hw_reset();
    os9rest_locale_reset();
    os9rest_icons_reset();
    os9rest_fsm_reset();
    drawsprocket_reset();
}
