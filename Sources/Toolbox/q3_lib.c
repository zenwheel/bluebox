/*
 * QuickDraw 3D's libraries as the Code Fragment Manager sees them: their
 * names, and their exports by name.
 */
#include <string.h>

#include "q3_internal.h"
#include "qd3d.h"
#include "tb_internal.h"

extern const ppc_export q3_storage_exports[];
extern const unsigned q3_storage_export_count;
extern const ppc_export q3_shapes_exports[], q3_group_exports[], q3_pick_exports[], q3_mesh_exports[];
extern const unsigned q3_shapes_export_count, q3_group_export_count, q3_pick_export_count, q3_mesh_export_count;

static const struct {
    const ppc_export *table;
    const unsigned *count;
} tables[] = {
    { rave_exports, &rave_export_count },
    { q3_exports, &q3_export_count },
    { q3_geom_exports, &q3_geom_export_count },
    { q3_mesh_exports, &q3_mesh_export_count },
    { q3_view_exports, &q3_view_export_count },
    { q3_shapes_exports, &q3_shapes_export_count },
    { q3_group_exports, &q3_group_export_count },
    { q3_pick_exports, &q3_pick_export_count },
    { q3_file_exports, &q3_file_export_count },
    { q3_viewer_exports, &q3_viewer_export_count },
    { q3_storage_exports, &q3_storage_export_count },
    { q3_math_exports, &q3_math_export_count },
};

/* The library's routine of this name, or NULL: QuickDraw 3D's start Q3
 * (and the Viewer's Q3Viewer), RAVE's QA, Custom Elements' CE. */
void (*qd3d_export_fn(const char *name))(ppc_cpu *)
{
    if (!((name[0] == 'Q' && (name[1] == '3' || name[1] == 'A')) || (name[0] == 'C' && name[1] == 'E')))
        return NULL;
    for (unsigned t = 0; t < sizeof tables / sizeof tables[0]; t++)
        for (unsigned i = 0; i < *tables[t].count; i++)
            if (!strcmp(tables[t].table[i].name, name))
                return tables[t].table[i].fn;
    return NULL;
}

/* Whether name is one of the libraries, as programs import them. */
bool qd3d_library(const char *name)
{
    if (!r3d_available())
        return false;
    return !strcmp(name, "QuickDraw\xAA 3D") || !strcmp(name, "QuickDraw\xAA 3D Accelerator") ||
           !strcmp(name, "QD3DViewerLib") || !strcmp(name, "QD3DCustomElementsLib") ||
           !strcmp(name, "QuickDraw\xAA 3D Viewer") || !strcmp(name, "QuickDraw\xAA 3D RAVE");
}

void rave_reset(void);
void q3_object_reset(void);
void q3_view_reset(void);

void qd3d_reset(void)
{
    q3_view_reset();
    q3_object_reset();
    rave_reset();
}
