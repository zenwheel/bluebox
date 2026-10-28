/*
 * The rest of Open Transport's client calls on providers and endpoints:
 * the endpoint's description (OTGetEndpointInfo), OTSync, the modes
 * (OTIsSynchronous, OTIsBlocking), OTCancelSynchronousCalls, the generic
 * provider opens (OTOpenProvider and OTAsyncOpenProvider, which pick the
 * endpoint, Internet services, AppleTalk services or mapper the
 * configuration names), and who owns a provider.
 */
#include <stdlib.h>
#include <string.h>

#include "ot_internal.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

#define PROVIDER(p)                                    \
    provider *p = ot_provider_of(R(0));                \
    if (!p) {                                          \
        RETURN((uint32_t)kOTBadReferenceErr);          \
        return;                                        \
    }

/* OTGetEndpointInfo(ep, TEndpointInfo *) */
static void c_GetEndpointInfo(ppc_cpu *c)
{
    PROVIDER(p);
    if (!R(1)) {
        RETURN((uint32_t)kOTBadReferenceErr);
        return;
    }
    ot_put_info(R(1), p->kind);
    RETURN(kOTNoError);
}

/* OTSync(ep): the endpoint's state once the provider has caught up with
 * what's happened (here, after the pending events are looked at). */
static void c_Sync(ppc_cpu *c)
{
    PROVIDER(p);
    ot_poll_provider(p);
    RETURN((uint32_t)p->state);
}

static void c_IsSynchronous(ppc_cpu *c)
{
    provider *p = ot_provider_of(R(0));
    RETURN(p && !p->async);
}

static void c_IsBlocking(ppc_cpu *c)
{
    provider *p = ot_provider_of(R(0));
    RETURN(p && p->blocking);
}

/* OTCancelSynchronousCalls(ref, err): a synchronous call blocked on the
 * provider returns err (kOTCanceledErr if err is 0). */
static void c_CancelSynchronousCalls(ppc_cpu *c)
{
    PROVIDER(p);
    p->cancel_err = R(1) ? (int32_t)R(1) : kOTCanceledErr;
    RETURN(kOTNoError);
}

/* OTTransferProviderOwnership(ref, prevOwner, &err): the provider handed
 * from the client that opened it; there is only the one client here. */
static void c_TransferOwnership(ppc_cpu *c)
{
    provider *p = ot_provider_of(R(0));
    if (R(2))
        wr32(R(2), p ? 0 : (uint32_t)kOTBadReferenceErr);
    RETURN(p ? R(0) : 0);
}

/* OTWhoAmI(): the client making the call. */
static void c_WhoAmI(ppc_cpu *c) { RETURN(0x4F54636Cu /* 'OTcl' */); }

static void c_noErr(ppc_cpu *c) { RETURN(kOTNoError); }

/* ---- OTOpenProvider and OTAsyncOpenProvider ---- */

typedef enum { W_ENDPOINT, W_INET, W_ATALK, W_MAPPER } what;

static what provider_what(gaddr cfg)
{
    if (cfg == 0xFFFFFFFDu) /* the default Internet (or AppleTalk) services path */
        return W_INET;
    char s[256] = "";
    for (uint32_t i = 0; cfg && i < 255 && rd8(cfg + i); i++)
        s[i] = (char)rd8(cfg + i);
    if (strstr(s, "dnr"))
        return W_INET;
    if (strstr(s, "nbp") && !strstr(s, "ddp") && !strstr(s, "atp"))
        return W_MAPPER;
    if (strstr(s, "atlk") && !strstr(s, "ddp") && !strstr(s, "atp") && !strstr(s, "adsp"))
        return W_ATALK;
    return W_ENDPOINT;
}

static void forward(ppc_cpu *c, const char *name)
{
    void (*fn)(ppc_cpu *) = cfm_export_fn(name);
    if (fn)
        fn(c);
    else
        RETURN((uint32_t)kOTNotSupportedErr);
}

/* OTOpenProvider(cfig, flags, &err) */
static void c_OpenProvider(ppc_cpu *c)
{
    switch (provider_what(R(0))) {
    case W_INET: forward(c, "OTOpenInternetServices"); break;
    case W_ATALK: forward(c, "OTOpenAppleTalkServices"); break;
    case W_MAPPER: forward(c, "OTOpenMapper"); break;
    default: /* an endpoint takes the info pointer third, the error fourth */
        R(3) = R(2);
        R(2) = 0;
        forward(c, "OTOpenEndpoint");
    }
}

/* OTAsyncOpenProvider(cfig, flags, proc, context) */
static void c_AsyncOpenProvider(ppc_cpu *c)
{
    switch (provider_what(R(0))) {
    case W_INET: forward(c, "OTAsyncOpenInternetServices"); break;
    case W_ATALK: forward(c, "OTAsyncOpenAppleTalkServices"); break;
    case W_MAPPER: forward(c, "OTAsyncOpenMapper"); break;
    default:
        R(4) = R(3);
        R(3) = R(2);
        R(2) = 0;
        forward(c, "OTAsyncOpenEndpoint");
    }
}

const ppc_export otc_exports[] = {
    { "OTGetEndpointInfo", c_GetEndpointInfo }, { "OTSync", c_Sync },
    { "OTIsSynchronous", c_IsSynchronous }, { "OTIsBlocking", c_IsBlocking },
    { "OTCancelSynchronousCalls", c_CancelSynchronousCalls },
    { "OTTransferProviderOwnership", c_TransferOwnership }, { "OTTransferOwnershipPriv", c_TransferOwnership },
    { "OTWhoAmI", c_WhoAmI },
    { "InitOpenTransportUtilities", c_noErr },
    { "OTOpenProvider", c_OpenProvider }, { "OTOpenProviderPriv", c_OpenProvider },
    { "OTAsyncOpenProvider", c_AsyncOpenProvider }, { "OTAsyncOpenProviderPriv", c_AsyncOpenProvider },
};
const unsigned otc_export_count = sizeof otc_exports / sizeof otc_exports[0];
