/*
 * Internet Config's calls from the 2.0 and 2.5 interfaces that had no
 * PowerPC export: the profiles, the defaults, GURL events and the access
 * mode.  The component answers (components.c); these call it as any
 * client does.
 */
#include "carbonrest.h"

/* A call to the instance (ComponentCallNow): the instance and the
 * arguments (sizes 4 or 2) pushed, the parameter size and selector last,
 * then _ComponentDispatch with D0 = 0. */
static uint32_t instance_call(ppc_cpu *c, int16_t what, const char *sizes)
{
    ppc_trap_begin();
    ppc_trap_push(4, 0);
    ppc_trap_push(4, A(0));
    uint32_t size = 0;
    for (int i = 0; sizes[i]; i++) {
        int n = sizes[i] - '0';
        ppc_trap_push(n, A(1 + (unsigned)i));
        size += n == 4 ? 4 : 2;
    }
    ppc_trap_push(4, size << 16 | (uint16_t)what);
    ppc_trap_set(0, 0);
    ppc_trap(0xA82A);
    uint32_t r = ppc_trap_result(4);
    ppc_trap_end();
    return r;
}

#define CALL(name, sel, sizes) static void ic_##name(ppc_cpu *c) { RET(instance_call(c, sel, sizes)); }
CALL(ICGetPerm, 0x0D, "4")
CALL(ICGetCurrentProfile, 0x25, "4")
CALL(ICSetCurrentProfile, 0x26, "4")
CALL(ICCountProfiles, 0x27, "4")
CALL(ICGetIndProfile, 0x28, "44")
CALL(ICGetProfileName, 0x29, "44")
CALL(ICSetProfileName, 0x2A, "44")
CALL(ICAddProfile, 0x2B, "44")
CALL(ICDeleteProfile, 0x2C, "4")
CALL(ICGetDefaultPref, 0x31, "44")
CALL(ICCreateGURLEvent, 0x33, "444")
CALL(ICSendGURLEvent, 0x34, "4")

void rest_ic_reset(void) {}

#define X(name) { #name, ic_##name }
const ppc_export rest_ic_exports[] = {
    X(ICAddProfile), X(ICCountProfiles), X(ICCreateGURLEvent), X(ICDeleteProfile), X(ICGetCurrentProfile),
    X(ICGetDefaultPref), X(ICGetIndProfile), X(ICGetPerm), X(ICGetProfileName), X(ICSendGURLEvent),
    X(ICSetCurrentProfile), X(ICSetProfileName),
};
const unsigned rest_ic_export_count = sizeof rest_ic_exports / sizeof rest_ic_exports[0];
