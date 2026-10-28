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
    GROUP(text),
    GROUP(priv),
    GROUP(locale),
};

/* Names that are other exports under a second name. */
static const struct { const char *name, *target; } aliases[] = {
    /* the Window Manager's screen bits under the names its own code gave them */
    { "SaveBits", "SaveScreenBits" }, { "RestoreBits", "RestoreScreenBits" },
    { "DiscardBits", "DisposeSavedScreenBits" },
    { "CloneIconRef", "AcquireIconRef" }, { "DisposeIconRef", "ReleaseIconRef" },
    { "__Comp3to1", "Comp3to1" }, { "__Comp6to1", "Comp6to1" }, { "__Exp1to3", "Exp1to3" }, { "__Exp1to6", "Exp1to6" },
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
    return os9rest_sound_data_known(name);
}

gaddr os9rest_data_export(const char *name)
{
    gaddr p = os9svc_keychain_data(name);
    return p ? p : os9rest_sound_data(name);
}

void os9rest_reset(void)
{
    os9rest_debug_reset();
    os9rest_hw_reset();
    os9rest_locale_reset();
    os9rest_icons_reset();
    os9rest_sound_reset();
    os9rest_fsm_reset();
    os9rest_priv_reset();
    os9rest_cloth_reset();
    drawsprocket_reset();
}

/* Call another export as a PowerPC caller would, with n arguments (the
 * first eight in r3 on, the rest in a parameter area made below the
 * caller's stack), keeping the registers of this call. */
uint32_t os9rest_call(ppc_cpu *c, const char *name, const uint32_t *args, int n)
{
    void (*fn)(ppc_cpu *) = cfm_export_fn(name);
    if (!fn)
        return (uint32_t)-4; /* unimpErr */
    uint32_t saved[8], sp = c->gpr[1], lr = c->lr, pc = c->pc;
    for (int i = 0; i < 8; i++)
        saved[i] = c->gpr[3 + i];
    uint32_t frame = (sp - (24u + 4u * (uint32_t)(n > 8 ? n : 8) + 32u)) & ~15u;
    wr32(frame, sp);
    c->gpr[1] = frame;
    for (int i = 0; i < n; i++) {
        if (i < 8)
            c->gpr[3 + i] = args[i];
        else
            wr32(frame + 24 + 4u * (uint32_t)i, args[i]);
    }
    fn(c);
    uint32_t r = c->gpr[3];
    for (int i = 0; i < 8; i++)
        c->gpr[3 + i] = saved[i];
    c->gpr[1] = sp;
    c->lr = lr;
    c->pc = pc;
    return r;
}
