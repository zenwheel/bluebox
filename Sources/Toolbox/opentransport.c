/*
 * Open Transport for PowerPC programs (OTClientLib, OTClientUtilLib,
 * OTInetClientLib, OTUtilityLib), over the host's BSD sockets.
 *
 * TCP and UDP endpoints and Internet Services (name lookups).  Every host
 * socket is non-blocking; a call made in blocking mode waits for its
 * socket in short slices, sending the provider's notifier kOTSyncIdleEvent
 * between them, as Open Transport did (programs yield to their threads
 * there).  Asynchronous events (T_DATA, T_ORDREL, T_CONNECT ...) and the
 * completions of calls made in asynchronous mode go to the notifier when
 * the program next asks for an event or yields to its threads, which is
 * as deferred as Open Transport's deferred tasks.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "machine.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

enum {
    kOTNoError = 0,
    kOTBadAddressErr = -3150, kOTBadOptionErr = -3151, kOTBadReferenceErr = -3153, kOTNoAddressErr = -3154,
    kOTOutStateErr = -3155, kOTLookErr = -3158, kOTBadDataErr = -3159, kOTFlowErr = -3161, kOTNoDataErr = -3162,
    kOTNoDisconnectErr = -3163, kOTNoUDErrErr = -3164, kOTNoReleaseErr = -3166, kOTNotSupportedErr = -3167,
    kOTBadNameErr = -3170, kOTAddressBusyErr = -3172, kOTOutOfMemoryErr = -3211, kOTNotFoundErr = -3201,
    kETIMEDOUTErr = -3259, kECONNREFUSEDErr = -3260, kEHOSTDOWNErr = -3263, kEHOSTUNREACHErr = -3264,
    kENETUNREACHErr = -3249, kECONNRESETErr = -3253,
};

enum {
    T_LISTEN = 0x0001, T_CONNECT = 0x0002, T_DATA = 0x0004, T_EXDATA = 0x0008, T_DISCONNECT = 0x0010,
    T_ERROR = 0x0020, T_UDERR = 0x0040, T_ORDREL = 0x0080, T_GODATA = 0x0100, T_GOEXDATA = 0x0200,
};

enum {
    T_BINDCOMPLETE = 0x20000001, T_UNBINDCOMPLETE = 0x20000002, T_ACCEPTCOMPLETE = 0x20000003,
    T_DISCONNECTCOMPLETE = 0x20000005, T_OPTMGMTCOMPLETE = 0x20000006, T_OPENCOMPLETE = 0x20000007,
    T_GETPROTADDRCOMPLETE = 0x20000008, kOTSyncIdleEvent = 0x20000011,
    T_DNRSTRINGTOADDRCOMPLETE = 0x10000001, T_DNRMAILEXCHANGECOMPLETE = 0x10000004,
};

enum { T_UNINIT = 0, T_UNBND = 1, T_IDLE = 2, T_OUTCON = 3, T_INCON = 4, T_DATAXFER = 5, T_OUTREL = 6, T_INREL = 7 };

enum { kOTAnyInetAddress = 0, AF_INET_OT = 2 };

#define REF_MAGIC 0x4F547072u /* 'OTpr' */
#define MAX_PROVIDERS 64
#define MAX_EVENTS 256

typedef enum { P_TCP, P_UDP, P_INET_SERVICES } provider_kind;

typedef struct {
    bool used;
    provider_kind kind;
    gaddr ref;           /* the program's ProviderRef: REF_MAGIC, index */
    int fd;              /* -1: none yet */
    bool async, blocking;
    gaddr notifier, context;
    int state;
    int qlen;            /* listening, with this queue */
    int pending_fd;      /* a connection waiting for OTAccept */
    struct sockaddr_in pending_peer;
    struct sockaddr_in peer;
    bool connecting;     /* a connect in progress */
    bool connected;      /* it finished: T_CONNECT until OTRcvConnect */
    bool told_connect, told_data, told_ordrel, told_disc, told_listen, flow;
    bool eof;            /* the other end released */
    bool disconnected;   /* reset or refused: T_DISCONNECT until OTRcvDisconnect */
    int disc_reason;
} provider;

typedef struct {
    int provider;
    uint32_t code;
    int32_t result;
    gaddr cookie;
} ot_event;

void machine_pass_tick(void); /* trap.c: time passes while waiting */

static provider providers[MAX_PROVIDERS];
static ot_event events[MAX_EVENTS];
static int nevents;
static bool delivering;

static gaddr new_ref(int i)
{
    gaddr p = tb_NewPtr(8, true, true);
    if (p) {
        wr32(p, REF_MAGIC);
        wr32(p + 4, (uint32_t)i);
    }
    return p;
}

static provider *provider_of(gaddr ref)
{
    if (!ref || rd32(ref) != REF_MAGIC)
        return NULL;
    uint32_t i = rd32(ref + 4);
    return i < MAX_PROVIDERS && providers[i].used ? &providers[i] : NULL;
}

static int new_provider(provider_kind kind)
{
    for (int i = 0; i < MAX_PROVIDERS; i++) {
        if (!providers[i].used) {
            providers[i] = (provider){ .used = true, .kind = kind, .fd = -1, .pending_fd = -1, .state = T_UNBND };
            providers[i].ref = new_ref(i);
            if (!providers[i].ref) {
                providers[i].used = false;
                return -1;
            }
            return i;
        }
    }
    return -1;
}

static void close_provider(provider *p)
{
    if (p->fd >= 0)
        close(p->fd);
    if (p->pending_fd >= 0)
        close(p->pending_fd);
    int i = (int)(p - providers);
    for (int k = 0; k < nevents; k++)
        if (events[k].provider == i)
            events[k].provider = -1;
    wr32(p->ref, 0);
    tb_DisposePtr(p->ref);
    p->used = false;
}

void opentransport_reset(void)
{
    for (int i = 0; i < MAX_PROVIDERS; i++) {
        if (providers[i].used) {
            if (providers[i].fd >= 0)
                close(providers[i].fd);
            if (providers[i].pending_fd >= 0)
                close(providers[i].pending_fd);
        }
    }
    memset(providers, 0, sizeof providers);
    nevents = 0;
    delivering = false;
}

/* ---- Notifiers ---- */

static void post(provider *p, uint32_t code, int32_t result, gaddr cookie)
{
    if (!p->notifier || nevents >= MAX_EVENTS)
        return;
    events[nevents++] = (ot_event){ (int)(p - providers), code, result, cookie };
}

static void call_notifier(provider *p, uint32_t code, int32_t result, gaddr cookie)
{
    if (code != kOTSyncIdleEvent)
        LOGF(LOG_TRAPS, "OT: notifier %d event %08X result %d cookie %08X\n", (int)(p - providers), code, result, cookie);
    /* void (*OTNotifyProcPtr)(void *contextPtr, OTEventCode, OTResult, void *cookie) */
    uint32_t args[4] = { p->context, code, (uint32_t)result, cookie };
    if (mm_is_descriptor(p->notifier))
        mm_call(p->notifier, 0x00003FC1 /* C, four longs */, args, 4);
    else
        ppc_call(p->notifier, args, 4);
}

static void poll_provider(provider *p);

/* Bring the providers' events up to date and hand the waiting ones to the
 * notifiers.  Called when the program asks for an event or yields. */
void opentransport_poll(void)
{
    if (delivering || g_cpu->halted)
        return;
    delivering = true;
    for (int i = 0; i < MAX_PROVIDERS; i++)
        if (providers[i].used)
            poll_provider(&providers[i]);
    for (int k = 0; k < nevents && !g_cpu->halted; k++) {
        ot_event e = events[k];
        if (e.provider >= 0 && providers[e.provider].used && providers[e.provider].notifier)
            call_notifier(&providers[e.provider], e.code, e.result, e.cookie);
    }
    nevents = 0;
    delivering = false;
}

/* ---- Addresses ---- */

/* InetAddress: fAddressType (2), fPort (2), fHost (4), 8 unused. */
static void put_inet(gaddr a, const struct sockaddr_in *sa)
{
    wr16(a, AF_INET_OT);
    wr16(a + 2, ntohs(sa->sin_port));
    wr32(a + 4, ntohl(sa->sin_addr.s_addr));
    gmemset(a + 8, 0, 8);
}

static bool get_inet(gaddr a, uint32_t len, struct sockaddr_in *sa)
{
    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
#ifdef __APPLE__
    sa->sin_len = sizeof *sa;
#endif
    if (!a || len < 8 || rd16(a) != AF_INET_OT)
        return false;
    sa->sin_port = htons(rd16(a + 2));
    sa->sin_addr.s_addr = htonl(rd32(a + 4));
    return true;
}

/* TNetbuf: maxlen, len, buf. */
static void netbuf_put_inet(gaddr nb, const struct sockaddr_in *sa)
{
    if (!nb || !rd32(nb + 8) || rd32(nb) < 16)
        return;
    put_inet(rd32(nb + 8), sa);
    wr32(nb + 4, 16);
}

static int32_t errno_result(int e)
{
    switch (e) {
    case ECONNREFUSED: return kECONNREFUSEDErr;
    case ETIMEDOUT: return kETIMEDOUTErr;
    case EHOSTDOWN: return kEHOSTDOWNErr;
    case EHOSTUNREACH: return kEHOSTUNREACHErr;
    case ENETUNREACH: return kENETUNREACHErr;
    case ECONNRESET: case EPIPE: return kECONNRESETErr;
    case EADDRINUSE: return kOTAddressBusyErr;
    default: return kOTBadAddressErr;
    }
}

static int make_socket(provider_kind kind)
{
    int fd = socket(AF_INET, kind == P_UDP ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return fd;
}

/* ---- Waiting ---- */

/* Wait for the socket in blocking mode: false if the machine stopped. */
static bool wait_fd(provider *p, int fd, short what)
{
    for (;;) {
        struct pollfd pf = { fd, what, 0 };
        if (poll(&pf, 1, 10) > 0)
            return true;
        if (p->notifier && !delivering)
            call_notifier(p, kOTSyncIdleEvent, 0, 0);
        machine_pass_tick();
        if (g_cpu->halted || !p->used)
            return false;
    }
}

/* What has happened on the socket since we last looked. */
static void poll_provider(provider *p)
{
    if (p->fd < 0 || p->kind == P_INET_SERVICES)
        return;
    if (p->connecting) {
        struct pollfd pf = { p->fd, POLLOUT, 0 };
        if (poll(&pf, 1, 0) > 0) {
            int err = 0;
            socklen_t len = sizeof err;
            getsockopt(p->fd, SOL_SOCKET, SO_ERROR, &err, &len);
            p->connecting = false;
            if (err) {
                p->disconnected = true;
                p->disc_reason = err;
                p->state = T_IDLE;
            } else {
                p->connected = true;
            }
        }
    }
    if (p->connected && !p->told_connect) {
        p->told_connect = true;
        post(p, T_CONNECT, 0, 0);
    }
    if (p->disconnected && !p->told_disc) {
        p->told_disc = true;
        post(p, T_DISCONNECT, 0, 0);
    }
    if (p->qlen > 0 && p->pending_fd < 0) {
        struct sockaddr_in sa;
        socklen_t len = sizeof sa;
        int fd = accept(p->fd, (struct sockaddr *)&sa, &len);
        if (fd >= 0) {
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            p->pending_fd = fd;
            p->pending_peer = sa;
            p->told_listen = false;
        }
    }
    if (p->pending_fd >= 0 && !p->told_listen) {
        p->told_listen = true;
        post(p, T_LISTEN, 0, 0);
    }
    if (p->state == T_DATAXFER || p->state == T_OUTREL || p->kind == P_UDP) {
        struct pollfd pf = { p->fd, POLLIN, 0 };
        if (poll(&pf, 1, 0) > 0) {
            char b;
            ssize_t n = p->kind == P_UDP ? 1 : recv(p->fd, &b, 1, MSG_PEEK);
            if (n > 0 && !p->told_data) {
                p->told_data = true;
                post(p, T_DATA, 0, 0);
            } else if (n == 0 && !p->eof) {
                p->eof = true;
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && !p->disconnected) {
                p->disconnected = true;
                p->disc_reason = errno;
            }
        }
        if (p->eof && !p->told_ordrel) {
            p->told_ordrel = true;
            post(p, T_ORDREL, 0, 0);
        }
        if (p->disconnected && !p->told_disc) {
            p->told_disc = true;
            post(p, T_DISCONNECT, 0, 0);
        }
        if (p->flow) {
            struct pollfd pw = { p->fd, POLLOUT, 0 };
            if (poll(&pw, 1, 0) > 0) {
                p->flow = false;
                post(p, T_GODATA, 0, 0);
            }
        }
    }
}

/* ---- OTClientUtilLib ---- */

static void ot_noErr(ppc_cpu *c) { RETURN(kOTNoError); }
static void ot_void(ppc_cpu *c) { c->pc = c->lr; }
static void ot_notSupported(ppc_cpu *c) { RETURN((uint32_t)kOTNotSupportedErr); }

/* OTCreateConfiguration(const char *path): the path, kept. */
static void ot_CreateConfiguration(ppc_cpu *c)
{
    gaddr path = R(0);
    uint32_t n = 0;
    while (path && n < 255 && rd8(path + n))
        n++;
    gaddr cfg = tb_NewPtr((int32_t)n + 1, true, true);
    if (cfg && path)
        gmemcpy(cfg, path, n);
    RETURN(cfg);
}

static provider_kind kind_of(gaddr cfg)
{
    char s[256] = { 0 };
    for (uint32_t i = 0; cfg && i < 255 && rd8(cfg + i); i++)
        s[i] = (char)rd8(cfg + i);
    return strstr(s, "udp") ? P_UDP : P_TCP;
}

/* TEndpointInfo: addr, options, tsdu, etsdu, connect, discon, servtype, flags. */
static void put_info(gaddr info, provider_kind kind)
{
    if (!info)
        return;
    wr32(info + 0, 16);
    wr32(info + 4, 1024);
    wr32(info + 8, kind == P_UDP ? 65535 : 0);
    wr32(info + 12, kind == P_UDP ? (uint32_t)-2 : 1);
    wr32(info + 16, (uint32_t)-2);
    wr32(info + 20, (uint32_t)-2);
    wr32(info + 24, kind == P_UDP ? 3 : 2); /* T_CLTS, T_COTS_ORD */
    wr32(info + 28, 0);
}

static int open_endpoint(gaddr cfg, gaddr info, int32_t *err)
{
    provider_kind kind = kind_of(cfg);
    int i = new_provider(kind);
    if (i < 0) {
        *err = kOTOutOfMemoryErr;
        return -1;
    }
    providers[i].fd = make_socket(kind);
    if (providers[i].fd < 0) {
        close_provider(&providers[i]);
        *err = kOTOutOfMemoryErr;
        return -1;
    }
    put_info(info, kind);
    *err = kOTNoError;
    return i;
}

/* OTOpenEndpointPriv(config, oflag, info, &err, ...): EndpointRef */
static void ot_OpenEndpoint(ppc_cpu *c)
{
    int32_t err;
    int i = open_endpoint(R(0), R(2), &err);
    if (R(3))
        wr32(R(3), (uint32_t)err);
    RETURN(i >= 0 ? providers[i].ref : 0);
}

/* OTAsyncOpenEndpointPriv(config, oflag, info, notifier, context, ...) */
static void ot_AsyncOpenEndpoint(ppc_cpu *c)
{
    int32_t err;
    int i = open_endpoint(R(0), R(2), &err);
    if (i < 0) {
        RETURN((uint32_t)err);
        return;
    }
    provider *p = &providers[i];
    p->async = true;
    p->notifier = R(3);
    p->context = R(4);
    post(p, T_OPENCOMPLETE, kOTNoError, p->ref);
    RETURN(kOTNoError);
}

/* OTOpenInternetServicesPriv(config, oflag, &err, ...): InetSvcRef */
static void ot_OpenInternetServices(ppc_cpu *c)
{
    int i = new_provider(P_INET_SERVICES);
    if (R(2))
        wr32(R(2), i < 0 ? (uint32_t)kOTOutOfMemoryErr : 0);
    RETURN(i >= 0 ? providers[i].ref : 0);
}

/* OTAsyncOpenInternetServicesPriv(config, oflag, notifier, context, ...) */
static void ot_AsyncOpenInternetServices(ppc_cpu *c)
{
    int i = new_provider(P_INET_SERVICES);
    if (i < 0) {
        RETURN((uint32_t)kOTOutOfMemoryErr);
        return;
    }
    provider *p = &providers[i];
    p->async = true;
    p->notifier = R(2);
    p->context = R(3);
    post(p, T_OPENCOMPLETE, kOTNoError, p->ref);
    RETURN(kOTNoError);
}

/* ---- OTClientLib: modes and notifiers ---- */

#define PROVIDER(p)                                    \
    provider *p = provider_of(R(0));                   \
    if (!p) {                                          \
        RETURN((uint32_t)kOTBadReferenceErr);          \
        return;                                        \
    }

static void ot_CloseProvider(ppc_cpu *c)
{
    PROVIDER(p);
    close_provider(p);
    RETURN(kOTNoError);
}

static void ot_SetSynchronous(ppc_cpu *c) { PROVIDER(p); p->async = false; RETURN(kOTNoError); }
static void ot_SetAsynchronous(ppc_cpu *c) { PROVIDER(p); p->async = true; RETURN(kOTNoError); }
static void ot_SetBlocking(ppc_cpu *c) { PROVIDER(p); p->blocking = true; RETURN(kOTNoError); }
static void ot_SetNonBlocking(ppc_cpu *c) { PROVIDER(p); p->blocking = false; RETURN(kOTNoError); }

/* OTInstallNotifier(ref, proc, context) */
static void ot_InstallNotifier(ppc_cpu *c)
{
    PROVIDER(p);
    p->notifier = R(1);
    p->context = R(2);
    RETURN(kOTNoError);
}

static void ot_RemoveNotifier(ppc_cpu *c)
{
    provider *p = provider_of(R(0));
    if (p)
        p->notifier = 0;
    c->pc = c->lr;
}

/* A call that completed now: in asynchronous mode it says so later. */
static void complete(ppc_cpu *c, provider *p, uint32_t event, int32_t result, gaddr cookie)
{
    if (p->async) {
        post(p, event, result, cookie);
        RETURN(kOTNoError);
    } else {
        RETURN((uint32_t)result);
    }
}

/* ---- Endpoints ---- */

/* OTBind(ep, reqAddr TBind *, retAddr TBind *) */
static void ot_Bind(ppc_cpu *c)
{
    PROVIDER(p);
    gaddr req = R(1), ret = R(2);
    struct sockaddr_in sa;
    bool have = req && get_inet(rd32(req + 8), rd32(req + 4), &sa);
    int32_t result = kOTNoError;
    int qlen = req ? (int)rd32(req + 12) : 0;
    if (have && (sa.sin_port || sa.sin_addr.s_addr)) {
        int one = 1;
        setsockopt(p->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(p->fd, (struct sockaddr *)&sa, sizeof sa) < 0)
            result = errno_result(errno);
    }
    if (result == kOTNoError && qlen > 0 && p->kind == P_TCP) {
        if (!have || !sa.sin_port) { /* any port */
            struct sockaddr_in any;
            get_inet(0, 0, &any);
            bind(p->fd, (struct sockaddr *)&any, sizeof any);
        }
        if (listen(p->fd, qlen) < 0)
            result = errno_result(errno);
        else
            p->qlen = qlen;
    }
    if (result == kOTNoError) {
        p->state = T_IDLE;
        if (ret) {
            struct sockaddr_in local;
            socklen_t len = sizeof local;
            memset(&local, 0, sizeof local);
            getsockname(p->fd, (struct sockaddr *)&local, &len);
            netbuf_put_inet(ret, &local);
            wr32(ret + 12, (uint32_t)p->qlen);
        }
    }
    complete(c, p, T_BINDCOMPLETE, result, ret);
}

static void ot_Unbind(ppc_cpu *c)
{
    PROVIDER(p);
    p->state = T_UNBND;
    complete(c, p, T_UNBINDCOMPLETE, kOTNoError, 0);
}

/* OTConnect(ep, sndCall TCall *, rcvCall TCall *) */
static void ot_Connect(ppc_cpu *c)
{
    PROVIDER(p);
    gaddr snd = R(1), rcv = R(2);
    struct sockaddr_in sa;
    if (!snd || !get_inet(rd32(snd + 8), rd32(snd + 4), &sa)) {
        RETURN((uint32_t)kOTBadAddressErr);
        return;
    }
    if (p->state != T_IDLE) {
        RETURN((uint32_t)kOTOutStateErr);
        return;
    }
    LOGF(LOG_TRAPS, "OT: connect %d to %s:%u (%s, %s)\n", (int)(p - providers), inet_ntoa(sa.sin_addr),
         ntohs(sa.sin_port), p->async ? "async" : "sync", p->blocking ? "blocking" : "non-blocking");
    p->peer = sa;
    p->connected = p->told_connect = p->disconnected = p->told_disc = false;
    int r = connect(p->fd, (struct sockaddr *)&sa, sizeof sa);
    if (r < 0 && errno != EINPROGRESS) {
        RETURN((uint32_t)errno_result(errno));
        return;
    }
    p->state = T_OUTCON;
    p->connecting = r < 0;
    if (!p->connecting)
        p->connected = true;
    if (!p->async && p->blocking) {
        if (p->connecting && !wait_fd(p, p->fd, POLLOUT)) {
            RETURN((uint32_t)kETIMEDOUTErr);
            return;
        }
        poll_provider(p);
        if (p->disconnected) {
            p->state = T_IDLE;
            p->told_disc = true;
            RETURN((uint32_t)errno_result(p->disc_reason));
            return;
        }
        p->told_connect = true;
        p->state = T_DATAXFER;
        if (rcv)
            netbuf_put_inet(rcv, &sa);
        RETURN(kOTNoError);
        return;
    }
    RETURN((uint32_t)kOTNoDataErr); /* T_CONNECT comes */
}

/* OTRcvConnect(ep, call TCall *) */
static void ot_RcvConnect(ppc_cpu *c)
{
    PROVIDER(p);
    if (p->state != T_OUTCON) {
        RETURN((uint32_t)kOTOutStateErr);
        return;
    }
    poll_provider(p);
    while (p->connecting && p->blocking && !p->async) {
        if (!wait_fd(p, p->fd, POLLOUT))
            break;
        poll_provider(p);
    }
    if (p->disconnected) {
        RETURN((uint32_t)kOTLookErr);
        return;
    }
    if (!p->connected) {
        RETURN((uint32_t)kOTNoDataErr);
        return;
    }
    p->told_connect = true;
    p->state = T_DATAXFER;
    if (R(1))
        netbuf_put_inet(R(1), &p->peer);
    RETURN(kOTNoError);
}

/* OTListen(ep, call TCall *): the next connection waiting. */
static void ot_Listen(ppc_cpu *c)
{
    PROVIDER(p);
    poll_provider(p);
    while (p->pending_fd < 0 && p->blocking && !p->async && p->qlen > 0) {
        if (!wait_fd(p, p->fd, POLLIN))
            break;
        poll_provider(p);
    }
    if (p->pending_fd < 0) {
        RETURN((uint32_t)kOTNoDataErr);
        return;
    }
    if (R(1)) {
        netbuf_put_inet(R(1), &p->pending_peer);
        wr32(R(1) + 36, 1); /* sequence */
    }
    RETURN(kOTNoError);
}

/* OTAccept(listener, worker, call): the worker takes the connection. */
static void ot_Accept(ppc_cpu *c)
{
    PROVIDER(p);
    provider *w = provider_of(R(1));
    if (!w || p->pending_fd < 0) {
        RETURN((uint32_t)(w ? kOTNoDataErr : kOTBadReferenceErr));
        return;
    }
    if (w != p) {
        if (w->fd >= 0)
            close(w->fd);
        w->fd = p->pending_fd;
        w->peer = p->pending_peer;
        w->state = T_DATAXFER;
        w->connected = w->told_connect = true;
    }
    p->pending_fd = -1;
    p->told_listen = false;
    complete(c, p, T_ACCEPTCOMPLETE, kOTNoError, R(2));
}

/* OTSnd(ep, buf, nbytes, flags): bytes sent */
static void ot_Snd(ppc_cpu *c)
{
    PROVIDER(p);
    gaddr buf = R(1);
    uint32_t n = R(2), done = 0;
    if (p->state != T_DATAXFER && p->state != T_INREL) {
        RETURN((uint32_t)(p->disconnected ? kOTLookErr : kOTOutStateErr));
        return;
    }
    while (done < n) {
        ssize_t r = send(p->fd, gptr(buf + done), n - done, 0);
        LOGF(LOG_TRAPS, "OT: snd %d: %zd of %u\n", (int)(p - providers), r, n - done);
        if (r > 0) {
            done += (uint32_t)r;
            continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (p->blocking && !p->async && wait_fd(p, p->fd, POLLOUT))
                continue;
            if (!done) {
                p->flow = true;
                RETURN((uint32_t)kOTFlowErr);
                return;
            }
            break;
        }
        p->disconnected = true;
        p->disc_reason = errno;
        RETURN((uint32_t)(done ? done : (uint32_t)kOTLookErr));
        return;
    }
    RETURN(done);
}

/* OTRcv(ep, buf, nbytes, &flags): bytes received */
static void ot_Rcv(ppc_cpu *c)
{
    PROVIDER(p);
    gaddr buf = R(1), flags = R(3);
    uint32_t n = R(2);
    if (flags)
        wr32(flags, 0);
    for (;;) {
        if (p->disconnected) {
            RETURN((uint32_t)kOTLookErr);
            return;
        }
        if (p->state != T_DATAXFER && p->state != T_OUTREL) {
            RETURN((uint32_t)kOTOutStateErr);
            return;
        }
        ssize_t r = recv(p->fd, gptr(buf), n, 0);
        LOGF(LOG_TRAPS, "OT: rcv %d: %zd of %u%s%s\n", (int)(p - providers), r, n, r < 0 ? ", " : "",
             r < 0 ? strerror(errno) : "");
        if (r > 0) {
            RETURN((uint32_t)r);
            return;
        }
        if (r == 0) {
            p->eof = true;
            RETURN((uint32_t)kOTLookErr); /* T_ORDREL */
            return;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            p->disconnected = true;
            p->disc_reason = errno;
            continue;
        }
        if (p->blocking && !p->async && wait_fd(p, p->fd, POLLIN))
            continue;
        p->told_data = false; /* the next data says so again */
        RETURN((uint32_t)kOTNoDataErr);
        return;
    }
}

/* OTLook(ep): the event waiting */
static void ot_Look(ppc_cpu *c)
{
    PROVIDER(p);
    poll_provider(p);
    if (p->disconnected)
        RETURN(T_DISCONNECT);
    else if (p->state == T_OUTCON && p->connected)
        RETURN(T_CONNECT);
    else if (p->pending_fd >= 0)
        RETURN(T_LISTEN);
    else if (p->eof) {
        /* data still to read comes first */
        char b;
        ssize_t r = p->fd >= 0 ? recv(p->fd, &b, 1, MSG_PEEK) : 0;
        RETURN(r > 0 ? T_DATA : T_ORDREL);
    } else {
        struct pollfd pf = { p->fd, POLLIN, 0 };
        RETURN(p->fd >= 0 && (p->state == T_DATAXFER || p->state == T_OUTREL) && poll(&pf, 1, 0) > 0 ? T_DATA : 0);
    }
}

static void ot_CountDataBytes(ppc_cpu *c)
{
    PROVIDER(p);
    int n = 0;
    if (p->fd >= 0)
        ioctl(p->fd, FIONREAD, &n);
    if (R(1))
        wr32(R(1), (uint32_t)n);
    RETURN(n > 0 ? kOTNoError : kOTNoDataErr);
}

static void ot_SndOrderlyDisconnect(ppc_cpu *c)
{
    PROVIDER(p);
    if (p->fd >= 0)
        shutdown(p->fd, SHUT_WR);
    p->state = p->state == T_INREL ? T_IDLE : T_OUTREL;
    RETURN(kOTNoError);
}

static void ot_RcvOrderlyDisconnect(ppc_cpu *c)
{
    PROVIDER(p);
    if (!p->eof) {
        RETURN((uint32_t)kOTNoReleaseErr);
        return;
    }
    p->state = p->state == T_OUTREL ? T_IDLE : T_INREL;
    RETURN(kOTNoError);
}

/* OTSndDisconnect(ep, call): abort the connection */
static void ot_SndDisconnect(ppc_cpu *c)
{
    PROVIDER(p);
    if (p->fd >= 0) {
        struct linger l = { 1, 0 };
        setsockopt(p->fd, SOL_SOCKET, SO_LINGER, &l, sizeof l);
        close(p->fd);
    }
    p->fd = make_socket(p->kind);
    p->state = T_IDLE;
    p->connecting = p->connected = p->eof = p->disconnected = false;
    p->told_connect = p->told_data = p->told_ordrel = p->told_disc = false;
    complete(c, p, T_DISCONNECTCOMPLETE, kOTNoError, R(1));
}

/* OTRcvDisconnect(ep, discon TDiscon *) */
static void ot_RcvDisconnect(ppc_cpu *c)
{
    PROVIDER(p);
    if (!p->disconnected) {
        RETURN((uint32_t)kOTNoDisconnectErr);
        return;
    }
    if (R(1))
        wr32(R(1) + 12, (uint32_t)p->disc_reason);
    if (p->fd >= 0)
        close(p->fd);
    p->fd = make_socket(p->kind);
    p->state = T_IDLE;
    p->connecting = p->connected = p->eof = p->disconnected = false;
    p->told_connect = p->told_data = p->told_ordrel = p->told_disc = false;
    RETURN(kOTNoError);
}

static void ot_GetEndpointState(ppc_cpu *c)
{
    PROVIDER(p);
    poll_provider(p);
    RETURN((uint32_t)p->state);
}

/* OTOptionManagement(ep, req, ret): options are taken as they are */
static void ot_OptionManagement(ppc_cpu *c)
{
    PROVIDER(p);
    gaddr req = R(1), ret = R(2);
    if (req && ret && rd32(ret + 8) && rd32(req + 8)) {
        uint32_t n = rd32(req + 4) < rd32(ret) ? rd32(req + 4) : rd32(ret);
        gmemcpy(rd32(ret + 8), rd32(req + 8), n);
        wr32(ret + 4, n);
        wr32(ret + 12, rd32(req + 12)); /* flags */
    }
    complete(c, p, T_OPTMGMTCOMPLETE, kOTNoError, ret);
}

/* OTGetProtAddress(ep, boundAddr TBind *, peerAddr TBind *) */
static void ot_GetProtAddress(ppc_cpu *c)
{
    PROVIDER(p);
    struct sockaddr_in local;
    socklen_t len = sizeof local;
    memset(&local, 0, sizeof local);
    if (p->fd >= 0)
        getsockname(p->fd, (struct sockaddr *)&local, &len);
    if (R(1))
        netbuf_put_inet(R(1), &local);
    if (R(2))
        netbuf_put_inet(R(2), &p->peer);
    complete(c, p, T_GETPROTADDRCOMPLETE, kOTNoError, R(2));
}

/* ---- Datagrams: TUnitData is addr, opt, udata (TNetbufs) ---- */

static void ot_SndUData(ppc_cpu *c)
{
    PROVIDER(p);
    gaddr ud = R(1);
    struct sockaddr_in sa;
    if (!ud || !get_inet(rd32(ud + 8), rd32(ud + 4), &sa)) {
        RETURN((uint32_t)kOTBadAddressErr);
        return;
    }
    ssize_t r = sendto(p->fd, gptr(rd32(ud + 32)), rd32(ud + 28), 0, (struct sockaddr *)&sa, sizeof sa);
    RETURN(r < 0 ? (uint32_t)(errno == EAGAIN ? kOTFlowErr : errno_result(errno)) : kOTNoError);
}

static void ot_RcvUData(ppc_cpu *c)
{
    PROVIDER(p);
    gaddr ud = R(1);
    if (R(2))
        wr32(R(2), 0);
    for (;;) {
        struct sockaddr_in sa;
        socklen_t len = sizeof sa;
        ssize_t r = recvfrom(p->fd, gptr(rd32(ud + 32)), rd32(ud + 24), 0, (struct sockaddr *)&sa, &len);
        if (r >= 0) {
            wr32(ud + 28, (uint32_t)r);
            netbuf_put_inet(ud, &sa);
            RETURN(kOTNoError);
            return;
        }
        if ((errno == EAGAIN || errno == EWOULDBLOCK) && p->blocking && !p->async && wait_fd(p, p->fd, POLLIN))
            continue;
        p->told_data = false;
        RETURN((uint32_t)kOTNoDataErr);
        return;
    }
}

static void ot_RcvUDErr(ppc_cpu *c) { RETURN((uint32_t)kOTNoUDErrErr); }
static void ot_AckSends(ppc_cpu *c) { RETURN(kOTNoError); }

/* ---- Internet Services ---- */

/* InetHostInfo: name (char[256]), addrs[10] */
static int32_t lookup(const char *name, gaddr hinfo)
{
    char host[256];
    snprintf(host, sizeof host, "%s", name);
    char *colon = strrchr(host, ':'); /* "name:port" */
    if (colon)
        *colon = 0;
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res)
        return kOTBadNameErr;
    gmemset(hinfo, 0, 256 + 40);
    const char *canon = res->ai_canonname ? res->ai_canonname : host;
    for (size_t i = 0; i < 255 && canon[i]; i++)
        wr8(hinfo + (gaddr)i, (uint8_t)canon[i]);
    int n = 0;
    for (struct addrinfo *a = res; a && n < 10; a = a->ai_next) {
        uint32_t ip = ntohl(((struct sockaddr_in *)a->ai_addr)->sin_addr.s_addr);
        bool dup = false;
        for (int k = 0; k < n; k++)
            dup |= rd32(hinfo + 256 + 4 * (gaddr)k) == ip;
        if (!dup)
            wr32(hinfo + 256 + 4 * (gaddr)n++, ip);
    }
    freeaddrinfo(res);
    return kOTNoError;
}

/* OTInetStringToAddress(ref, name, hinfo) */
static void ot_InetStringToAddress(ppc_cpu *c)
{
    PROVIDER(p);
    char name[256] = { 0 };
    for (uint32_t i = 0; R(1) && i < 255 && rd8(R(1) + i); i++)
        name[i] = (char)rd8(R(1) + i);
    int32_t r = lookup(name, R(2));
    LOGF(LOG_TRAPS, "OT: look up %s: %d\n", name, r);
    if (p->async) {
        post(p, T_DNRSTRINGTOADDRCOMPLETE, r, R(2));
        RETURN(kOTNoError);
    } else {
        RETURN((uint32_t)r);
    }
}

static void ot_InetMailExchange(ppc_cpu *c) { RETURN((uint32_t)kOTNotSupportedErr); }

/* OTInetHostToString(host, char *str) */
static void ot_InetHostToString(ppc_cpu *c)
{
    char s[32];
    uint32_t h = R(0);
    snprintf(s, sizeof s, "%u.%u.%u.%u", h >> 24, (h >> 16) & 255, (h >> 8) & 255, h & 255);
    if (R(1))
        gput(R(1), s, (uint32_t)strlen(s) + 1);
    c->pc = c->lr;
}

/* OTInitInetAddress(addr, port, host) */
static void ot_InitInetAddress(ppc_cpu *c)
{
    gaddr a = R(0);
    if (a) {
        wr16(a, AF_INET_OT);
        wr16(a + 2, (uint16_t)R(1));
        wr32(a + 4, R(2));
        gmemset(a + 8, 0, 8);
    }
    c->pc = c->lr;
}

/* The host's address on the network the default route uses. */
static uint32_t local_address(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return 0x7F000001;
    struct sockaddr_in sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_port = htons(53);
    sa.sin_addr.s_addr = htonl(0x08080808);
    uint32_t ip = 0x7F000001;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) {
        socklen_t len = sizeof sa;
        if (getsockname(fd, (struct sockaddr *)&sa, &len) == 0)
            ip = ntohl(sa.sin_addr.s_addr);
    }
    close(fd);
    return ip;
}

/* OTInetGetInterfaceInfo(InetInterfaceInfo *, index): one interface */
static void ot_InetGetInterfaceInfo(ppc_cpu *c)
{
    gaddr info = R(0);
    if ((R(1) != 0 && R(1) != 0xFFFFFFFFu) || !info) { /* 0 or kDefaultInetInterface */
        RETURN((uint32_t)kOTNotFoundErr);
        return;
    }
    uint32_t ip = local_address();
    gmemset(info, 0, 36);
    wr32(info + 0, ip);
    wr32(info + 4, 0xFFFFFF00);           /* netmask */
    wr32(info + 8, (ip & 0xFFFFFF00) | 0xFF);
    wr32(info + 12, (ip & 0xFFFFFF00) | 1); /* gateway */
    wr32(info + 16, (ip & 0xFFFFFF00) | 1); /* name server */
    wr16(info + 20, 1);                    /* fVersion */
    RETURN(kOTNoError);
}

/* ---- OTUtilityLib ---- */

static void ot_AllocMem(ppc_cpu *c) { RETURN(tb_NewPtr((int32_t)R(0), false, false)); }
static void ot_FreeMem(ppc_cpu *c)
{
    if (R(0))
        tb_DisposePtr(R(0));
    c->pc = c->lr;
}
static void ot_MemcpyOrMove(ppc_cpu *c)
{
    if (R(0) && R(1))
        gmemcpy(R(0), R(1), R(2));
    c->pc = c->lr;
}
static void ot_Memzero(ppc_cpu *c)
{
    if (R(0))
        gmemset(R(0), 0, R(1));
    c->pc = c->lr;
}
static void ot_Memset(ppc_cpu *c)
{
    if (R(0))
        gmemset(R(0), (int)(R(1) & 0xFF), R(2));
    c->pc = c->lr;
}
static void ot_StrLength(ppc_cpu *c)
{
    uint32_t n = 0;
    while (R(0) && rd8(R(0) + n))
        n++;
    RETURN(n);
}

static uint64_t micros(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

/* OTGetTimeStamp(OTTimeStamp *): microseconds */
static void ot_GetTimeStamp(ppc_cpu *c)
{
    uint64_t t = micros();
    if (R(0)) {
        wr32(R(0), (uint32_t)(t >> 32));
        wr32(R(0) + 4, (uint32_t)t);
    }
    c->pc = c->lr;
}

static uint64_t stamp(gaddr p) { return p ? (uint64_t)rd32(p) << 32 | rd32(p + 4) : 0; }
static void ot_ElapsedMilliseconds(ppc_cpu *c) { RETURN((uint32_t)((micros() - stamp(R(0))) / 1000)); }
static void ot_ElapsedMicroseconds(ppc_cpu *c) { RETURN((uint32_t)(micros() - stamp(R(0)))); }
static void ot_EnterNotifier(ppc_cpu *c) { RETURN(1); }
static void ot_Idle(ppc_cpu *c)
{
    opentransport_poll();
    c->pc = c->lr;
}

/* Lists: OTLIFO and OTLink are a pointer each (fHead, fNext). */
static void ot_LIFOEnqueue(ppc_cpu *c)
{
    if (R(0) && R(1)) {
        wr32(R(1), rd32(R(0)));
        wr32(R(0), R(1));
    }
    c->pc = c->lr;
}
static void ot_LIFODequeue(ppc_cpu *c)
{
    gaddr h = R(0) ? rd32(R(0)) : 0;
    if (h)
        wr32(R(0), rd32(h));
    RETURN(h);
}
static void ot_LIFOStealList(ppc_cpu *c)
{
    gaddr h = R(0) ? rd32(R(0)) : 0;
    if (R(0))
        wr32(R(0), 0);
    RETURN(h);
}
static void ot_ReverseList(ppc_cpu *c)
{
    gaddr l = R(0), prev = 0;
    while (l) {
        gaddr next = rd32(l);
        wr32(l, prev);
        prev = l;
        l = next;
    }
    RETURN(prev);
}

/* Atomic operations: one thread runs at a time.  Bits count from the low
 * end of the byte, as BSET's do. */
static void ot_AtomicSetBit(ppc_cpu *c)
{
    uint8_t v = R(0) ? rd8(R(0)) : 0, m = (uint8_t)(1u << (R(1) & 7));
    if (R(0))
        wr8(R(0), v | m);
    RETURN((v & m) != 0);
}
static void ot_AtomicClearBit(ppc_cpu *c)
{
    uint8_t v = R(0) ? rd8(R(0)) : 0, m = (uint8_t)(1u << (R(1) & 7));
    if (R(0))
        wr8(R(0), v & (uint8_t)~m);
    RETURN((v & m) != 0);
}
static void ot_AtomicTestBit(ppc_cpu *c) { RETURN(R(0) && (rd8(R(0)) >> (R(1) & 7)) & 1); }
static void ot_AtomicAdd32(ppc_cpu *c) /* (delta, SInt32 *): the new value */
{
    uint32_t v = R(1) ? rd32(R(1)) + R(0) : 0;
    if (R(1))
        wr32(R(1), v);
    RETURN(v);
}
static void ot_AtomicAdd16(ppc_cpu *c)
{
    uint32_t v = R(1) ? (uint32_t)(int16_t)(rd16(R(1)) + R(0)) : 0;
    if (R(1))
        wr16(R(1), (uint16_t)v);
    RETURN(v);
}
static void ot_AtomicAdd8(ppc_cpu *c)
{
    uint32_t v = R(1) ? (uint32_t)(int8_t)(rd8(R(1)) + R(0)) : 0;
    if (R(1))
        wr8(R(1), (uint8_t)v);
    RETURN(v);
}
static void ot_CompareAndSwap32(ppc_cpu *c) /* (old, new, UInt32 *) */
{
    bool same = R(2) && rd32(R(2)) == R(0);
    if (same)
        wr32(R(2), R(1));
    RETURN(same);
}
static void ot_TimeStampInMilliseconds(ppc_cpu *c) { RETURN((uint32_t)(stamp(R(0)) / 1000)); }
static void ot_Delay(ppc_cpu *c) { c->pc = c->lr; }

/* OTBuffer: fLink, fLink2, fNext, fData, fLen ... (we never hand them out,
 * but programs may walk ones they made). */
static void ot_BufferDataSize(ppc_cpu *c)
{
    uint32_t n = 0;
    for (gaddr b = R(0); b; b = rd32(b + 8))
        n += rd32(b + 16);
    RETURN(n);
}
/* OTReadBuffer(OTBufferInfo *, dest, OTByteCount *len): from fBuffer at fOffset */
static void ot_ReadBuffer(ppc_cpu *c)
{
    gaddr info = R(0), dest = R(1), lenp = R(2);
    uint32_t want = lenp ? rd32(lenp) : 0, got = 0;
    gaddr b = info ? rd32(info) : 0;
    uint32_t off = info ? rd32(info + 4) : 0;
    while (b && got < want) {
        uint32_t len = rd32(b + 16);
        if (off >= len) {
            b = rd32(b + 8);
            off = 0;
            continue;
        }
        uint32_t n = len - off < want - got ? len - off : want - got;
        gmemcpy(dest + got, rd32(b + 12) + off, n);
        got += n;
        off += n;
    }
    if (info) {
        wr32(info, b);
        wr32(info + 4, off);
    }
    if (lenp)
        wr32(lenp, got);
    c->pc = c->lr;
}

/* OTAlloc(ref, structType, fields, &err): an XTI structure with buffers. */
enum { T_BIND = 1, T_OPTMGMT, T_CALL, T_DIS, T_UNITDATA, T_UDERROR, T_INFO, T_REPLYDATA, T_REQUESTDATA };
static void alloc_netbuf(gaddr nb, uint32_t size)
{
    gaddr b = tb_NewPtr((int32_t)size, false, true);
    wr32(nb, b ? size : 0);
    wr32(nb + 4, 0);
    wr32(nb + 8, b);
}
static void ot_Alloc(ppc_cpu *c)
{
    uint32_t type = R(1), fields = R(2);
    static const uint32_t sizes[] = { 0, 16, 16, 40, 20, 36, 28, 32 };
    if (type < 1 || type > 7) {
        if (R(3))
            wr32(R(3), (uint32_t)kOTNotSupportedErr);
        RETURN(0);
        return;
    }
    gaddr t = tb_NewPtr((int32_t)sizes[type], true, true);
    bool addr = fields & 1, opt = fields & 2, udata = fields & 4;
    if (t) {
        switch (type) {
        case T_BIND: if (addr) alloc_netbuf(t, 16); break;
        case T_OPTMGMT: if (opt) alloc_netbuf(t, 1024); break;
        case T_CALL: case T_UNITDATA: case T_UDERROR:
            if (addr) alloc_netbuf(t, 16);
            if (opt) alloc_netbuf(t + 12, 1024);
            if (udata && type != T_UDERROR) alloc_netbuf(t + 24, 1024);
            break;
        case T_DIS: if (udata) alloc_netbuf(t, 1024); break;
        default: break;
        }
    }
    if (R(3))
        wr32(R(3), t ? 0 : (uint32_t)kOTOutOfMemoryErr);
    RETURN(t);
}
static void ot_Free(ppc_cpu *c) /* (ptr, structType) */
{
    gaddr t = R(0);
    uint32_t type = R(1);
    if (t) {
        int nbufs = type == T_CALL || type == T_UNITDATA || type == T_UDERROR ? 3 : type == T_INFO ? 0 : 1;
        for (int i = 0; i < nbufs; i++)
            if (rd32(t + 12 * (uint32_t)i + 8))
                tb_DisposePtr(rd32(t + 12 * (uint32_t)i + 8));
        tb_DisposePtr(t);
    }
    RETURN(kOTNoError);
}
/* OTOpenMapper(config, oflag, &err): no name registration here */
static void ot_OpenMapper(ppc_cpu *c)
{
    if (R(2))
        wr32(R(2), (uint32_t)kOTNotSupportedErr);
    RETURN(0);
}
static void ot_false(ppc_cpu *c) { RETURN(0); }

/* Any other Open Transport call: not supported. */
void ot_unsupported(ppc_cpu *c) { RETURN((uint32_t)kOTNotSupportedErr); }

const ppc_export ot_exports[] = {
    /* OTClientUtilLib */
    { "InitOpenTransport", ot_noErr }, { "InitOpenTransportPriv", ot_noErr },
    { "InitOpenTransportInContext", ot_noErr }, { "CloseOpenTransport", ot_void },
    { "CloseOpenTransportPriv", ot_void }, { "CloseOpenTransportInContext", ot_void },
    { "OTExitPatchPriv", ot_void }, { "OTCreateConfiguration", ot_CreateConfiguration },
    { "OTDestroyConfiguration", ot_void },
    /* OTClientLib */
    { "OTOpenEndpoint", ot_OpenEndpoint }, { "OTOpenEndpointPriv", ot_OpenEndpoint },
    { "OTOpenEndpointInContext", ot_OpenEndpoint }, { "OTAsyncOpenEndpoint", ot_AsyncOpenEndpoint },
    { "OTAsyncOpenEndpointPriv", ot_AsyncOpenEndpoint }, { "OTAsyncOpenEndpointInContext", ot_AsyncOpenEndpoint },
    { "OTCloseProvider", ot_CloseProvider }, { "OTCloseProviderPriv", ot_CloseProvider },
    { "OTSetSynchronous", ot_SetSynchronous }, { "OTSetAsynchronous", ot_SetAsynchronous },
    { "OTSetBlocking", ot_SetBlocking }, { "OTSetNonBlocking", ot_SetNonBlocking },
    { "OTInstallNotifier", ot_InstallNotifier }, { "OTRemoveNotifier", ot_RemoveNotifier },
    { "OTBind", ot_Bind }, { "OTUnbind", ot_Unbind }, { "OTConnect", ot_Connect },
    { "OTRcvConnect", ot_RcvConnect }, { "OTListen", ot_Listen }, { "OTAccept", ot_Accept },
    { "OTSnd", ot_Snd }, { "OTRcv", ot_Rcv }, { "OTLook", ot_Look }, { "OTCountDataBytes", ot_CountDataBytes },
    { "OTSndOrderlyDisconnect", ot_SndOrderlyDisconnect }, { "OTRcvOrderlyDisconnect", ot_RcvOrderlyDisconnect },
    { "OTSndDisconnect", ot_SndDisconnect }, { "OTRcvDisconnect", ot_RcvDisconnect },
    { "OTGetEndpointState", ot_GetEndpointState }, { "OTOptionManagement", ot_OptionManagement },
    { "OTGetProtAddress", ot_GetProtAddress }, { "OTSndUData", ot_SndUData }, { "OTRcvUData", ot_RcvUData },
    { "OTRcvUDErr", ot_RcvUDErr }, { "OTAckSends", ot_AckSends }, { "OTDontAckSends", ot_AckSends },
    { "OTIsAckingSends", ot_AckSends }, { "OTResolveAddress", ot_notSupported },
    /* OTInetClientLib */
    { "OTOpenInternetServices", ot_OpenInternetServices },
    { "OTOpenInternetServicesPriv", ot_OpenInternetServices },
    { "OTOpenInternetServicesInContext", ot_OpenInternetServices },
    { "OTAsyncOpenInternetServices", ot_AsyncOpenInternetServices },
    { "OTAsyncOpenInternetServicesPriv", ot_AsyncOpenInternetServices },
    { "OTAsyncOpenInternetServicesInContext", ot_AsyncOpenInternetServices },
    { "OTInetStringToAddress", ot_InetStringToAddress }, { "OTInetMailExchange", ot_InetMailExchange },
    { "OTInetHostToString", ot_InetHostToString }, { "OTInitInetAddress", ot_InitInetAddress },
    { "OTInetGetInterfaceInfo", ot_InetGetInterfaceInfo },
    /* OTUtilityLib */
    { "OTAllocMem", ot_AllocMem }, { "OTFreeMem", ot_FreeMem }, { "OTMemcpy", ot_MemcpyOrMove },
    { "OTMemmove", ot_MemcpyOrMove }, { "OTMemzero", ot_Memzero }, { "OTMemset", ot_Memset },
    { "OTStrLength", ot_StrLength }, { "OTGetTimeStamp", ot_GetTimeStamp },
    { "OTElapsedMilliseconds", ot_ElapsedMilliseconds }, { "OTElapsedMicroseconds", ot_ElapsedMicroseconds },
    { "OTEnterNotifier", ot_EnterNotifier }, { "OTLeaveNotifier", ot_void }, { "OTIdle", ot_Idle },
    { "OTAllocMemPriv", ot_AllocMem }, { "OTFreeMemPriv", ot_FreeMem }, { "OTAllocMemInContext", ot_AllocMem },
    { "OTFreeMemInContext", ot_FreeMem }, { "OTLIFOEnqueue", ot_LIFOEnqueue }, { "OTLIFODequeue", ot_LIFODequeue },
    { "OTLIFOStealList", ot_LIFOStealList }, { "OTReverseList", ot_ReverseList },
    { "OTAtomicSetBit", ot_AtomicSetBit }, { "OTAtomicClearBit", ot_AtomicClearBit },
    { "OTAtomicTestBit", ot_AtomicTestBit }, { "OTAtomicAdd32", ot_AtomicAdd32 }, { "OTAtomicAdd16", ot_AtomicAdd16 },
    { "OTAtomicAdd8", ot_AtomicAdd8 }, { "OTCompareAndSwap32", ot_CompareAndSwap32 },
    { "OTCompareAndSwapPtr", ot_CompareAndSwap32 }, { "OTTimeStampInMilliseconds", ot_TimeStampInMilliseconds },
    { "OTDelay", ot_Delay }, { "OTBufferDataSize", ot_BufferDataSize }, { "OTReadBuffer", ot_ReadBuffer },
    { "OTReleaseBuffer", ot_void }, { "OTAlloc", ot_Alloc }, { "OTAllocPriv", ot_Alloc },
    { "OTAllocInContext", ot_Alloc }, { "OTFree", ot_Free }, { "OTOpenMapper", ot_OpenMapper },
    { "OTOpenMapperPriv", ot_OpenMapper }, { "OTOpenMapperInContext", ot_OpenMapper },
    { "OTFindPortByRef", ot_false }, { "OTIoctl", ot_unsupported },
};
const unsigned ot_export_count = sizeof ot_exports / sizeof ot_exports[0];
