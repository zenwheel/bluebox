/*
 * The lookup over Mac OS 9's service libraries' extra exports: each
 * library's file (os9svc_<library>.c) has a table of its own routines and
 * one of names that are other exports under a second name.
 */
#include <string.h>

#include "os9svc.h"

typedef struct {
    const ppc_export *exports;
    const unsigned *export_count;
    const os9_alias *aliases;
    const unsigned *alias_count;
} os9_library;

static const os9_library libraries[] = {
    { os9_font_exports, &os9_font_export_count, os9_font_aliases, &os9_font_alias_count },
    { os9_sound_exports, &os9_sound_export_count, os9_sound_aliases, &os9_sound_alias_count },
    { os9_driver_exports, &os9_driver_export_count, os9_driver_aliases, &os9_driver_alias_count },
    { os9_url_exports, &os9_url_export_count, os9_url_aliases, &os9_url_alias_count },
    { os9_registry_exports, &os9_registry_export_count, os9_registry_aliases, &os9_registry_alias_count },
};

void (*cfm_export_fn(const char *name))(ppc_cpu *);

void (*os9svc_export_fn(const char *name))(ppc_cpu *)
{
    for (unsigned l = 0; l < sizeof libraries / sizeof libraries[0]; l++) {
        const os9_library *lib = &libraries[l];
        for (unsigned i = 0; i < *lib->export_count; i++)
            if (!strcmp(lib->exports[i].name, name))
                return lib->exports[i].fn;
        for (unsigned i = 0; i < *lib->alias_count; i++)
            if (!strcmp(lib->aliases[i].name, name))
                return cfm_export_fn(lib->aliases[i].target);
    }
    return NULL;
}

void os9svc_reset(void)
{
    os9svc_sound_reset();
    os9svc_driver_reset();
    os9svc_power_reset();
    os9svc_url_reset();
    os9svc_registry_reset();
}
