/*
 * Open Transport's XTI interface (OpenTransportLib's t_open, t_bind,
 * t_connect, t_snd ... and t_errno): the same endpoints as the OT calls,
 * under XTI's conventions.  An XTI "file descriptor" is the endpoint
 * reference; the structures (t_call, t_bind, t_unitdata ...) are the ones
 * the OT calls take.  Each call here runs its OT counterpart and turns the
 * OSStatus into XTI's: -1 and t_errno set (TLOOK, TNODATA, TBADF ...; a UNIX
 * error is TSYSERR), or 0 and the count or state the call returns.
 */
#include <stdlib.h>
#include <string.h>

#include "ot_internal.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

enum { TSYSERR = 8, TLASTXTIERROR = 31, ELASTERRNO = 79 };

/* t_errno, the global the calls set. */
static gaddr t_errno_addr;

gaddr ot_xti_errno_address(void)
{
    if (!t_errno_addr)
        t_errno_addr = tb_NewPtr(4, true, true);
    return t_errno_addr;
}

void ot_xti_reset(void) { t_errno_addr = 0; }

static void set_t_errno(int32_t err)
{
    uint32_t t;
    if (err < -3149 && err >= -3149 - TLASTXTIERROR)
        t = (uint32_t)(-3149 - err);
    else
        t = TSYSERR; /* a UNIX error, or one with no XTI number */
    gaddr a = ot_xti_errno_address();
    if (a)
        wr32(a, t);
}

typedef enum {
    X_STATUS,  /* an OSStatus: 0 on success */
    X_RESULT,  /* an OTResult: a count or state on success */
    X_PLAIN,   /* a Boolean or value that can't fail */
    X_VOID,
} xti_style;

static void xti_finish(ppc_cpu *c, xti_style style)
{
    int32_t r = (int32_t)c->gpr[3];
    switch (style) {
    case X_STATUS:
        if (r < 0) {
            set_t_errno(r);
            c->gpr[3] = (uint32_t)-1;
        } else {
            c->gpr[3] = 0;
        }
        break;
    case X_RESULT:
        if (r < 0) {
            set_t_errno(r);
            c->gpr[3] = (uint32_t)-1;
        }
        break;
    default: break;
    }
}

/* Run the OT call with these same arguments and convert what it returned. */
static void xti_call(ppc_cpu *c, void (**cache)(ppc_cpu *), const char *ot, xti_style style)
{
    if (!*cache)
        *cache = cfm_export_fn(ot);
    if (!*cache) {
        set_t_errno(kOTNotSupportedErr);
        RETURN((uint32_t)-1);
        return;
    }
    (*cache)(c);
    xti_finish(c, style);
}

#define XTI(name, ot, style)                          \
    static void x_##name(ppc_cpu *c)                  \
    {                                                 \
        static void (*fn)(ppc_cpu *);                 \
        xti_call(c, &fn, ot, style);                  \
    }

XTI(t_accept, "OTAccept", X_STATUS)
XTI(t_bind, "OTBind", X_STATUS)
XTI(t_connect, "OTConnect", X_STATUS)
XTI(t_getinfo, "OTGetEndpointInfo", X_STATUS)
XTI(t_getprotaddr, "OTGetProtAddress", X_STATUS)
XTI(t_getstate, "OTGetEndpointState", X_RESULT)
XTI(t_listen, "OTListen", X_STATUS)
XTI(t_look, "OTLook", X_RESULT)
XTI(t_optmgmt, "OTOptionManagement", X_STATUS)
XTI(t_rcv, "OTRcv", X_RESULT)
XTI(t_rcvconnect, "OTRcvConnect", X_STATUS)
XTI(t_rcvdis, "OTRcvDisconnect", X_STATUS)
XTI(t_rcvrel, "OTRcvOrderlyDisconnect", X_STATUS)
XTI(t_rcvudata, "OTRcvUData", X_STATUS)
XTI(t_rcvuderr, "OTRcvUDErr", X_STATUS)
XTI(t_resolveaddr, "OTResolveAddress", X_STATUS)
XTI(t_snd, "OTSnd", X_RESULT)
XTI(t_snddis, "OTSndDisconnect", X_STATUS)
XTI(t_sndrel, "OTSndOrderlyDisconnect", X_STATUS)
XTI(t_sndudata, "OTSndUData", X_STATUS)
XTI(t_sync, "OTSync", X_RESULT)
XTI(t_unbind, "OTUnbind", X_STATUS)
XTI(t_close, "OTCloseProvider", X_STATUS)
XTI(t_blocking, "OTSetBlocking", X_STATUS)
XTI(t_nonblocking, "OTSetNonBlocking", X_STATUS)
XTI(t_asynchronous, "OTSetAsynchronous", X_STATUS)
XTI(t_synchronous, "OTSetSynchronous", X_STATUS)
XTI(t_issynchronous, "OTIsSynchronous", X_PLAIN)
XTI(t_usesyncidleevents, "OTUseSyncIdleEvents", X_STATUS)
XTI(t_installnotifier, "OTInstallNotifier", X_STATUS)
XTI(t_removenotifier, "OTRemoveNotifier", X_VOID)
/* the transaction calls */
XTI(t_sndrequest, "OTSndRequest", X_STATUS)
XTI(t_rcvreply, "OTRcvReply", X_STATUS)
XTI(t_sndreply, "OTSndReply", X_STATUS)
XTI(t_rcvrequest, "OTRcvRequest", X_STATUS)
XTI(t_cancelrequest, "OTCancelRequest", X_STATUS)
XTI(t_cancelreply, "OTCancelReply", X_STATUS)
XTI(t_sndurequest, "OTSndURequest", X_STATUS)
XTI(t_rcvureply, "OTRcvUReply", X_STATUS)
XTI(t_sndureply, "OTSndUReply", X_STATUS)
XTI(t_rcvurequest, "OTRcvURequest", X_STATUS)
XTI(t_cancelurequest, "OTCancelURequest", X_STATUS)
XTI(t_cancelureply, "OTCancelUReply", X_STATUS)

/* t_isnonblocking(fd): whether the endpoint is non-blocking. */
static void x_t_isnonblocking(ppc_cpu *c)
{
    static void (*fn)(ppc_cpu *);
    xti_call(c, &fn, "OTIsBlocking", X_PLAIN);
    c->gpr[3] = !c->gpr[3];
}

/* t_cancelsynchronouscalls(fd): a blocked call returns kOTCanceledErr. */
static void x_t_cancelsynchronouscalls(ppc_cpu *c)
{
    static void (*fn)(ppc_cpu *);
    R(1) = 0;
    xti_call(c, &fn, "OTCancelSynchronousCalls", X_STATUS);
}

/* t_open(path, oflag, info): the endpoint a configuration path names, as
 * OTOpenEndpoint opens it; its reference, or -1.  t_open_priv takes the
 * client context after these. */
static void x_t_open(ppc_cpu *c)
{
    static void (*create)(ppc_cpu *), (*open)(ppc_cpu *);
    if (!create)
        create = cfm_export_fn("OTCreateConfiguration");
    if (!open)
        open = cfm_export_fn("OTOpenEndpoint");
    gaddr err = tb_NewPtr(4, false, true);
    uint32_t oflag = R(1), info = R(2);
    if (!create || !open || !err) {
        set_t_errno(kOTOutOfMemoryErr);
        RETURN((uint32_t)-1);
        return;
    }
    create(c); /* the path in r3, the configuration back in r3 */
    R(0) = c->gpr[3];
    R(1) = oflag;
    R(2) = info;
    R(3) = err;
    open(c);
    uint32_t ref = c->gpr[3];
    int32_t e = (int32_t)rd32(err);
    tb_DisposePtr(err);
    if (e < 0 || !ref) {
        set_t_errno(e < 0 ? e : kOTBadNameErr);
        RETURN((uint32_t)-1);
        return;
    }
    RETURN(ref);
}

/* t_alloc(fd, structType, fields): the structure, or NULL. */
static void x_t_alloc(ppc_cpu *c)
{
    static void (*fn)(ppc_cpu *);
    if (!fn)
        fn = cfm_export_fn("OTAlloc");
    gaddr err = tb_NewPtr(4, false, true);
    uint32_t fd = R(0), type = R(1), fields = R(2);
    if (!fn || !err) {
        set_t_errno(kOTOutOfMemoryErr);
        RETURN(0);
        return;
    }
    R(0) = fd;
    R(1) = type;
    R(2) = fields;
    R(3) = err;
    fn(c);
    uint32_t t = c->gpr[3];
    int32_t e = (int32_t)rd32(err);
    tb_DisposePtr(err);
    if (!t || e < 0) {
        set_t_errno(e < 0 ? e : kOTOutOfMemoryErr);
        RETURN(0);
        return;
    }
    RETURN(t);
}

/* t_free(ptr, structType) */
static void x_t_free(ppc_cpu *c)
{
    static void (*fn)(ppc_cpu *);
    xti_call(c, &fn, "OTFree", X_STATUS);
}

/* t_error(msg): XTI prints the message and the error; there is nowhere to
 * print. */
static void x_t_error(ppc_cpu *c) { RETURN(0); }

const ppc_export otx_exports[] = {
    { "t_accept", x_t_accept }, { "t_alloc", x_t_alloc }, { "t_alloc_priv", x_t_alloc }, { "t_bind", x_t_bind },
    { "t_close", x_t_close }, { "t_close_priv", x_t_close }, { "t_connect", x_t_connect },
    { "t_free", x_t_free }, { "t_getinfo", x_t_getinfo }, { "t_getprotaddr", x_t_getprotaddr },
    { "t_getstate", x_t_getstate }, { "t_listen", x_t_listen }, { "t_look", x_t_look },
    { "t_open", x_t_open }, { "t_open_priv", x_t_open }, { "t_optmgmt", x_t_optmgmt }, { "t_rcv", x_t_rcv },
    { "t_rcvconnect", x_t_rcvconnect }, { "t_rcvdis", x_t_rcvdis }, { "t_rcvrel", x_t_rcvrel },
    { "t_rcvudata", x_t_rcvudata }, { "t_rcvuderr", x_t_rcvuderr }, { "t_resolveaddr", x_t_resolveaddr },
    { "t_snd", x_t_snd }, { "t_snddis", x_t_snddis }, { "t_sndrel", x_t_sndrel }, { "t_sndudata", x_t_sndudata },
    { "t_sync", x_t_sync }, { "t_unbind", x_t_unbind }, { "t_error", x_t_error },
    { "t_blocking", x_t_blocking }, { "t_nonblocking", x_t_nonblocking }, { "t_isnonblocking", x_t_isnonblocking },
    { "t_asynchronous", x_t_asynchronous }, { "t_synchronous", x_t_synchronous },
    { "t_issynchronous", x_t_issynchronous }, { "t_usesyncidleevents", x_t_usesyncidleevents },
    { "t_installnotifier", x_t_installnotifier }, { "t_removenotifier", x_t_removenotifier },
    { "t_cancelsynchronouscalls", x_t_cancelsynchronouscalls },
    { "t_sndrequest", x_t_sndrequest }, { "t_rcvreply", x_t_rcvreply }, { "t_sndreply", x_t_sndreply },
    { "t_rcvrequest", x_t_rcvrequest }, { "t_cancelrequest", x_t_cancelrequest },
    { "t_cancelreply", x_t_cancelreply }, { "t_sndurequest", x_t_sndurequest },
    { "t_rcvureply", x_t_rcvureply }, { "t_sndureply", x_t_sndureply }, { "t_rcvurequest", x_t_rcvurequest },
    { "t_cancelurequest", x_t_cancelurequest }, { "t_cancelureply", x_t_cancelureply },
};
const unsigned otx_export_count = sizeof otx_exports / sizeof otx_exports[0];
