/*
 * CarbonLib's remaining managers (carbonrest.h): the lookup over their
 * export tables, the reset between programs, and what they share.
 */
#include <string.h>

#include "carbonrest.h"

size_t rest_cstr(gaddr p, char *out, size_t max)
{
    size_t n = 0;
    while (p && n + 1 < max && rd8(p + n))
        out[n] = (char)rd8(p + (uint32_t)n), n++;
    out[n] = 0;
    return n;
}

void rest_pstr_get(gaddr p, char *out, size_t max)
{
    size_t n = p ? rd8(p) : 0;
    if (n + 1 > max)
        n = max - 1;
    for (size_t i = 0; i < n; i++)
        out[i] = (char)rd8(p + 1 + (uint32_t)i);
    out[n] = 0;
}

void rest_pstr_put(gaddr dst, const char *s, size_t max)
{
    size_t n = strlen(s);
    if (n > max)
        n = max;
    wr8(dst, (uint8_t)n);
    for (size_t i = 0; i < n; i++)
        wr8(dst + 1 + (uint32_t)i, (uint8_t)s[i]);
}

static const struct {
    const ppc_export *table;
    const unsigned *count;
    void (*reset)(void);
} areas[] = {
    { rest_misc_exports, &rest_misc_export_count, rest_misc_reset },
    { rest_ae_exports, &rest_ae_export_count, rest_ae_reset },
    { rest_osa_exports, &rest_osa_export_count, rest_osa_reset },
    { rest_ic_exports, &rest_ic_export_count, rest_ic_reset },
    { rest_ica_exports, &rest_ica_export_count, rest_ica_reset },
    { rest_nsl_exports, &rest_nsl_export_count, rest_nsl_reset },
    { rest_dcm_exports, &rest_dcm_export_count, rest_dcm_reset },
    { rest_loc_exports, &rest_loc_export_count, rest_loc_reset },
    { rest_uc_exports, &rest_uc_export_count, rest_uc_reset },
    { rest_la_exports, &rest_la_export_count, rest_la_reset },
    { rest_fbc_exports, &rest_fbc_export_count, rest_fbc_reset },
    { rest_fns_exports, &rest_fns_export_count, rest_fns_reset },
};

void rest_sr_reset(void); /* the Speech Recognition Manager's objects (carbonrest_sr.c) */

void (*carbonrest_export_fn(const char *name))(ppc_cpu *)
{
    for (unsigned t = 0; t < sizeof areas / sizeof *areas; t++)
        for (unsigned i = 0; i < *areas[t].count; i++)
            if (!strcmp(areas[t].table[i].name, name))
                return areas[t].table[i].fn;
    return NULL;
}

void carbonrest_reset(void)
{
    rest_sr_reset();
    for (unsigned t = 0; t < sizeof areas / sizeof *areas; t++)
        if (areas[t].reset)
            areas[t].reset();
}
