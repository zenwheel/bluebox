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
};

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
    for (unsigned t = 0; t < sizeof areas / sizeof *areas; t++)
        if (areas[t].reset)
            areas[t].reset();
}
