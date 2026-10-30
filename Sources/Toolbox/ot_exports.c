/*
 * Open Transport's exports beyond opentransport.c's, ot_appletalk.c's and
 * ot_session.c's: one lookup over the tables of the files that hold them
 * (ot_util.c, ot_ports.c, ot_options.c, ot_inet.c, ot_client.c, ot_xti.c,
 * ot_streams.c and ot_config.c), and Open Transport's data exports (t_errno).
 */
#include <string.h>

#include "ot_internal.h"
#include "ppc_bridge.h"

typedef struct {
    const ppc_export *table;
    const unsigned *count;
} table_ref;

static const table_ref tables[] = {
    { otu_exports, &otu_export_count }, { otp_exports, &otp_export_count }, { oto_exports, &oto_export_count },
    { oti_exports, &oti_export_count }, { otc_exports, &otc_export_count }, { otx_exports, &otx_export_count },
    { otst_exports, &otst_export_count }, { otf_exports, &otf_export_count },
};

void (*ot2_export_fn(const char *name))(ppc_cpu *)
{
    for (unsigned t = 0; t < sizeof tables / sizeof tables[0]; t++)
        for (unsigned i = 0; i < *tables[t].count; i++)
            if (!strcmp(tables[t].table[i].name, name))
                return tables[t].table[i].fn;
    return NULL;
}

bool ot2_data_known(const char *name) { return !strcmp(name, "t_errno"); }

gaddr ot2_data_export(const char *name)
{
    return !strcmp(name, "t_errno") ? ot_xti_errno_address() : 0;
}
