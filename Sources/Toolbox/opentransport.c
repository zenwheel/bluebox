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
 *
 * AppleTalk's providers (ddp, atp, adsp, NBP's mapper and AppleTalk
 * services) are ot_appletalk.c's: the calls here hand them on.
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
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "machine.h"
#include "ot_internal.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

enum { kOTAnyInetAddress = 0, AF_INET_OT = 2, AF_DNS = 42 };

#define REF_MAGIC 0x4F547072u /* 'OTpr' */
#define MAX_EVENTS 256

typedef struct {
    int provider;
    uint32_t code;
    int32_t result;
    gaddr cookie;
} ot_event;

void machine_pass_tick(void); /* trap.c: time passes while waiting */

provider ot_providers[MAX_PROVIDERS];
#define providers ot_providers
static ot_event events[MAX_EVENTS];
static int nevents;
static bool delivering;
static int notifier_depth; /* notifiers running (deferred task time) */

bool ot_in_notifier(void) { return delivering || notifier_depth > 0; }

static gaddr new_ref(int i)
{
    gaddr p = tb_NewPtr(8, true, true);
    if (p) {
        wr32(p, REF_MAGIC);
        wr32(p + 4, (uint32_t)i);
    }
    return p;
}

provider *ot_provider_of(gaddr ref)
{
    if (!ref || rd32(ref) != REF_MAGIC)
        return NULL;
    uint32_t i = rd32(ref + 4);
    return i < MAX_PROVIDERS && providers[i].used ? &providers[i] : NULL;
}

int ot_new_provider(provider_kind kind)
{
    for (int i = 0; i < MAX_PROVIDERS; i++) {
        if (!providers[i].used) {
            providers[i] = (provider){ .used = true, .kind = kind, .fd = -1, .pending_fd = -1, .state = T_UNBND,
                                       .m68k = ot_from_68k };
            if (kind == P_TCP || kind == P_UDP || kind == P_INET_SERVICES)
                ot_stack_loading(); /* the first use loads TCP/IP, and the registered clients hear of it */
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
    if (OT_ATALK(p))
        ota_close(p);
    if (p->fd >= 0)
        close(p->fd);
    if (p->pending_fd >= 0)
        close(p->pending_fd);
    int i = (int)(p - providers);
    for (int k = 0; k < nevents; k++)
        if (events[k].provider == i)
            events[k].provider = -1;
    ot_options_free(p);
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
            free(providers[i].at); /* AppleTalk went with the machine */
            ot_options_free(&providers[i]);
        }
    }
    memset(providers, 0, sizeof providers);
    nevents = 0;
    delivering = false;
    notifier_depth = 0;
    ot_util_reset();
    ot_ports_reset();
    ot_inet_reset();
    ot_68k_reset();
}

/* ---- Notifiers ---- */

void ot_post(provider *p, uint32_t code, int32_t result, gaddr cookie)
{
    if (!p->notifier || nevents >= MAX_EVENTS)
        return;
    events[nevents++] = (ot_event){ (int)(p - providers), code, result, cookie };
}

static void call_notifier(provider *p, uint32_t code, int32_t result, gaddr cookie)
{
    if (code == T_ORDREL)
        p->heard_ordrel = true;
    if (code != kOTSyncIdleEvent)
        LOGF(LOG_TRAPS, "OT: notifier %d event %08X result %d cookie %08X\n", (int)(p - providers), code, result, cookie);
    /* void (*OTNotifyProcPtr)(void *contextPtr, OTEventCode, OTResult, void *cookie):
     * a Pascal-convention routine for 68k code */
    uint32_t args[4] = { p->context, code, (uint32_t)result, cookie };
    notifier_depth++;
    ot_callproc_as(p->m68k, p->notifier, 0x00003FC0, args, 4);
    notifier_depth--;
}

/* Whether TCP urgent data is waiting on the socket (not read in line): the
 * socket has an exceptional condition. */
static bool has_urgent(int fd)
{
    if (fd < 0 || fd >= FD_SETSIZE)
        return false;
    fd_set ex;
    FD_ZERO(&ex);
    FD_SET(fd, &ex);
    struct timeval none = { 0, 0 };
    return select(fd + 1, NULL, NULL, &ex, &none) > 0 && FD_ISSET(fd, &ex);
}

static void poll_provider(provider *p);
void ot_poll_provider(provider *p) { poll_provider(p); }

/* Bring the providers' events up to date and hand the waiting ones to the
 * notifiers.  Called when the program asks for an event or yields. */
void opentransport_poll(void)
{
    if (delivering || g_cpu->halted)
        return;
    delivering = true;
    ot_ports_poll();
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
    if (a && len > 2 && len <= 2 + 255 && rd16(a) == AF_DNS) {
        /* "host" or "host:port", looked up as OTConnect and OTSndUData do */
        char name[256] = { 0 };
        gget(name, a + 2, len - 2);
        char *colon = strrchr(name, ':');
        if (colon) {
            *colon = 0;
            sa->sin_port = htons((uint16_t)atoi(colon + 1));
        }
        struct addrinfo hints = { 0 }, *res = NULL;
        hints.ai_family = AF_INET;
        if (getaddrinfo(name, NULL, &hints, &res) != 0 || !res)
            return false;
        sa->sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
        return true;
    }
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

bool ot_wait_slice(provider *p)
{
    if (p->notifier && !delivering)
        call_notifier(p, kOTSyncIdleEvent, 0, 0);
    machine_pass_tick();
    return !g_cpu->halted && p->used;
}

/* A blocked call cancelled by OTCancelSynchronousCalls: the error it
 * returns, once. */
static int32_t take_cancel(provider *p)
{
    int32_t e = p->cancel_err;
    p->cancel_err = 0;
    return e;
}

/* Wait for the socket in blocking mode: false if the machine stopped or the
 * call was cancelled (take_cancel says which). */
static bool wait_fd(provider *p, int fd, short what)
{
    for (;;) {
        struct pollfd pf = { fd, what, 0 };
        if (poll(&pf, 1, 10) > 0)
            return true;
        if (p->notifier && !delivering)
            call_notifier(p, kOTSyncIdleEvent, 0, 0);
        machine_pass_tick();
        if (g_cpu->halted || !p->used || p->cancel_err)
            return false;
    }
}

/* What has happened on the socket since we last looked. */
static void poll_provider(provider *p)
{
    if (OT_ATALK(p)) {
        ota_poll(p);
        return;
    }
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
        ot_post(p, T_CONNECT, 0, 0);
    }
    if (p->disconnected && !p->told_disc) {
        p->told_disc = true;
        ot_post(p, T_DISCONNECT, 0, 0);
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
        ot_post(p, T_LISTEN, 0, 0);
    }
    if (p->state == T_DATAXFER || p->state == T_OUTREL || p->kind == P_UDP) {
        struct pollfd pf = { p->fd, POLLIN, 0 };
        if (poll(&pf, 1, 0) > 0) {
            char b;
            ssize_t n = p->kind == P_UDP ? 1 : recv(p->fd, &b, 1, MSG_PEEK);
            if (n > 0 && !p->told_data) {
                p->told_data = true;
                ot_post(p, T_DATA, 0, 0);
            } else if (n == 0 && !p->eof) {
                p->eof = true;
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && !p->disconnected) {
                p->disconnected = true;
                p->disc_reason = errno;
            }
        }
        if (p->kind == P_TCP && has_urgent(p->fd)) {
            if (!p->told_exdata) {
                p->told_exdata = true;
                ot_post(p, T_EXDATA, 0, 0);
            }
        } else {
            p->told_exdata = false;
        }
        if (p->eof && !p->told_ordrel) {
            p->told_ordrel = true;
            ot_post(p, T_ORDREL, 0, 0);
        }
        if (p->disconnected && !p->told_disc) {
            p->told_disc = true;
            ot_post(p, T_DISCONNECT, 0, 0);
        }
        if (p->flow) {
            struct pollfd pw = { p->fd, POLLOUT, 0 };
            if (poll(&pw, 1, 0) > 0) {
                p->flow = false;
                ot_post(p, T_GODATA, 0, 0);
            }
        }
    }
}

/* ---- OTClientUtilLib ---- */

static void ot_noErr(ppc_cpu *c) { RETURN(kOTNoError); }
static void ot_void(ppc_cpu *c) { c->pc = c->lr; }

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

static void config_string(gaddr cfg, char s[256])
{
    memset(s, 0, 256);
    for (uint32_t i = 0; cfg && i < 255 && rd8(cfg + i); i++)
        s[i] = (char)rd8(cfg + i);
}

static provider_kind kind_of(gaddr cfg)
{
    char s[256];
    config_string(cfg, s);
    provider_kind k;
    if (ota_kind(s, &k))
        return k;
    return strstr(s, "udp") ? P_UDP : P_TCP;
}

/* TEndpointInfo: addr, options, tsdu, etsdu, connect, discon, servtype, flags. */
void ot_put_info(gaddr info, provider_kind kind);
static void put_info(gaddr info, provider_kind kind) { ot_put_info(info, kind); }
void ot_put_info(gaddr info, provider_kind kind)
{
    if (!info)
        return;
    if (kind >= P_DDP) {
        ota_info(info, kind);
        return;
    }
    wr32(info + 0, 16);
    wr32(info + 4, 1024);
    wr32(info + 8, kind == P_UDP ? 65535 : 0);
    wr32(info + 12, kind == P_UDP ? (uint32_t)-2 : 1);
    wr32(info + 16, (uint32_t)-2);
    wr32(info + 20, (uint32_t)-2);
    wr32(info + 24, kind == P_UDP ? 3 : 2); /* T_CLTS, T_COTS_ORD */
    wr32(info + 28, 0x2 | 0x40000000);     /* T_XPG4_1, T_CAN_RESOLVE_ADDR */
}

static int open_endpoint(gaddr cfg, gaddr info, int32_t *err)
{
    provider_kind kind = kind_of(cfg);
    int i = ot_new_provider(kind);
    if (i < 0) {
        *err = kOTOutOfMemoryErr;
        return -1;
    }
    if (kind >= P_DDP) {
        char s[256];
        config_string(cfg, s);
        *err = ota_open(&providers[i], s);
    } else
        providers[i].fd = make_socket(kind);
    if (kind >= P_DDP ? *err != kOTNoError : providers[i].fd < 0) {
        close_provider(&providers[i]);
        if (kind < P_DDP)
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
    ot_post(p, T_OPENCOMPLETE, kOTNoError, p->ref);
    RETURN(kOTNoError);
}

/* OTOpenInternetServicesPriv(config, oflag, &err, ...): InetSvcRef */
static void ot_OpenInternetServices(ppc_cpu *c)
{
    int i = ot_new_provider(P_INET_SERVICES);
    if (R(2))
        wr32(R(2), i < 0 ? (uint32_t)kOTOutOfMemoryErr : 0);
    RETURN(i >= 0 ? providers[i].ref : 0);
}

/* OTAsyncOpenInternetServicesPriv(config, oflag, notifier, context, ...) */
static void ot_AsyncOpenInternetServices(ppc_cpu *c)
{
    int i = ot_new_provider(P_INET_SERVICES);
    if (i < 0) {
        RETURN((uint32_t)kOTOutOfMemoryErr);
        return;
    }
    provider *p = &providers[i];
    p->async = true;
    p->notifier = R(2);
    p->context = R(3);
    ot_post(p, T_OPENCOMPLETE, kOTNoError, p->ref);
    RETURN(kOTNoError);
}

/* ---- OTClientLib: modes and notifiers ---- */

#define PROVIDER(p)                                    \
    provider *p = ot_provider_of(R(0));                \
    if (!p) {                                          \
        RETURN((uint32_t)kOTBadReferenceErr);          \
        return;                                        \
    }

/* An endpoint call AppleTalk's providers answer themselves. */
#define ENDPOINT(p, atalk)                             \
    PROVIDER(p);                                       \
    if (OT_ATALK(p)) {                                 \
        atalk(c, p);                                   \
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
    provider *p = ot_provider_of(R(0));
    if (p)
        p->notifier = 0;
    c->pc = c->lr;
}

/* A call that completed now: in asynchronous mode it says so later. */
void ot_complete(ppc_cpu *c, provider *p, uint32_t event, int32_t result, gaddr cookie)
{
    if (p->async) {
        ot_post(p, event, result, cookie);
        RETURN(kOTNoError);
    } else {
        RETURN((uint32_t)result);
    }
}

/* ---- Endpoints ---- */

/* OTBind(ep, reqAddr TBind *, retAddr TBind *) */
static void ot_Bind(ppc_cpu *c)
{
    ENDPOINT(p, ota_Bind);
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
    ot_complete(c, p, T_BINDCOMPLETE, result, ret);
}

static void ot_Unbind(ppc_cpu *c)
{
    ENDPOINT(p, ota_Unbind);
    p->state = T_UNBND;
    ot_complete(c, p, T_UNBINDCOMPLETE, kOTNoError, 0);
}

/* OTConnect(ep, sndCall TCall *, rcvCall TCall *) */
static void ot_Connect(ppc_cpu *c)
{
    ENDPOINT(p, ota_Connect);
    gaddr snd = R(1), rcv = R(2);
    struct sockaddr_in sa;
    if (!snd || !get_inet(rd32(snd + 8), rd32(snd + 4), &sa)) {
        LOGF(LOG_TRAPS, "OT: connect %d: address type %d, %u bytes\n", (int)(p - providers),
             snd && rd32(snd + 8) ? rd16(rd32(snd + 8)) : -1, snd ? rd32(snd + 4) : 0);
        RETURN((uint32_t)kOTBadAddressErr);
        return;
    }
    if (p->state != T_IDLE) {
        RETURN((uint32_t)kOTOutStateErr);
        return;
    }
    ot_options_apply(p, snd + 12); /* the options the connection asks for */
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
            int32_t ce = p->used ? take_cancel(p) : 0;
            if (ce) { /* the attempt is given up: a fresh socket */
                close(p->fd);
                p->fd = make_socket(p->kind);
                p->state = T_IDLE;
                p->connecting = p->connected = false;
            }
            RETURN((uint32_t)(ce ? ce : kETIMEDOUTErr));
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
    ENDPOINT(p, ota_RcvConnect);
    if (p->state != T_OUTCON) {
        RETURN((uint32_t)kOTOutStateErr);
        return;
    }
    poll_provider(p);
    while (p->connecting && p->blocking && !p->async) {
        if (!wait_fd(p, p->fd, POLLOUT)) {
            int32_t ce = p->used ? take_cancel(p) : 0;
            if (ce) {
                RETURN((uint32_t)ce);
                return;
            }
            break;
        }
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
    ENDPOINT(p, ota_Listen);
    poll_provider(p);
    while (p->pending_fd < 0 && p->blocking && !p->async && p->qlen > 0) {
        if (!wait_fd(p, p->fd, POLLIN)) {
            int32_t ce = p->used ? take_cancel(p) : 0;
            if (ce) {
                RETURN((uint32_t)ce);
                return;
            }
            break;
        }
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
    provider *w = ot_provider_of(R(1));
    if (OT_ATALK(p)) {
        ota_Accept(c, p, w);
        return;
    }
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
        ot_options_reapply(w); /* what the worker had negotiated goes on the new socket */
        if (R(2))
            ot_options_apply(w, R(2) + 12);
    }
    p->pending_fd = -1;
    p->told_listen = false;
    ot_complete(c, p, T_ACCEPTCOMPLETE, kOTNoError, R(2));
}

uint32_t ot_data_len(gaddr buf, uint32_t len)
{
    if (len != kNetbufDataIsOTData)
        return len;
    uint32_t n = 0;
    for (int i = 0; buf && i < 1024; buf = rd32(buf), i++)
        n += rd32(buf + 8);
    return n;
}

uint32_t ot_data_get(uint8_t *out, gaddr buf, uint32_t len, uint32_t max)
{
    if (len != kNetbufDataIsOTData) {
        uint32_t n = len < max ? len : max;
        if (n)
            gget(out, buf, n);
        return n;
    }
    uint32_t n = 0;
    for (int i = 0; buf && i < 1024 && n < max; buf = rd32(buf), i++) {
        uint32_t k = rd32(buf + 8);
        k = k < max - n ? k : max - n;
        if (k)
            gget(out + n, rd32(buf + 4), k);
        n += k;
    }
    return n;
}

/* OTSnd(ep, buf, nbytes, flags): bytes sent */
static void ot_Snd(ppc_cpu *c)
{
    ENDPOINT(p, ota_Snd);
    gaddr buf = R(1);
    uint32_t n = R(2), done = 0;
    if (p->state != T_DATAXFER && p->state != T_INREL) {
        RETURN((uint32_t)(p->disconnected ? kOTLookErr : kOTOutStateErr));
        return;
    }
    uint8_t *gathered = NULL; /* an OTData chain, in one piece */
    if (n == kNetbufDataIsOTData) {
        n = ot_data_len(buf, kNetbufDataIsOTData);
        gathered = malloc(n ? n : 1);
        if (!gathered) {
            RETURN((uint32_t)kOTOutOfMemoryErr);
            return;
        }
        ot_data_get(gathered, buf, kNetbufDataIsOTData, n);
    }
    int sendflags = R(3) & T_EXPEDITED ? MSG_OOB : 0; /* expedited data goes as TCP urgent data */
    /* The host's stack loses urgent data sent on a non-blocking socket. */
    int oldfl = sendflags ? fcntl(p->fd, F_GETFL) : 0;
    if (sendflags)
        fcntl(p->fd, F_SETFL, oldfl & ~O_NONBLOCK);
    while (done < n) {
        ssize_t r = send(p->fd, gathered ? gathered + done : gptr(buf + done), n - done, sendflags);
        LOGF(LOG_TRAPS, "OT: snd %d: %zd of %u\n", (int)(p - providers), r, n - done);
        if (r > 0) {
            done += (uint32_t)r;
            continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (p->blocking && !p->async && wait_fd(p, p->fd, POLLOUT))
                continue;
            if (p->used && p->cancel_err) {
                int32_t ce = take_cancel(p);
                free(gathered);
                if (sendflags)
                    fcntl(p->fd, F_SETFL, oldfl);
                RETURN((uint32_t)(done ? done : (uint32_t)ce));
                return;
            }
            if (!done) {
                p->flow = true;
                free(gathered);
                if (sendflags)
                    fcntl(p->fd, F_SETFL, oldfl);
                RETURN((uint32_t)kOTFlowErr);
                return;
            }
            break;
        }
        p->disconnected = true;
        p->disc_reason = errno;
        free(gathered);
        if (sendflags)
            fcntl(p->fd, F_SETFL, oldfl);
        RETURN((uint32_t)(done ? done : (uint32_t)kOTLookErr));
        return;
    }
    free(gathered);
    if (sendflags)
        fcntl(p->fd, F_SETFL, oldfl);
    RETURN(done);
}

/* OTRcv(ep, buf, nbytes, &flags): bytes received */
static void ot_Rcv(ppc_cpu *c)
{
    ENDPOINT(p, ota_Rcv);
    gaddr buf = R(1), flags = R(3);
    uint32_t n = R(2);
    bool nocopy = n == kNetbufDataIsOTBufferStar; /* buf is the address of an OTBuffer * to fill */
    static uint8_t nocopy_bytes[65536];
    uint8_t *bounce = nocopy ? nocopy_bytes : NULL;
    if (nocopy)
        n = sizeof nocopy_bytes;
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
        if (g_code_map && n && !nocopy)
            mem_code_range(buf, n);
        ssize_t r;
        int rflags = 0;
        if (has_urgent(p->fd)) /* urgent data (not in line with the rest) is read first and flagged */
            rflags = MSG_OOB;
        r = recv(p->fd, bounce ? (void *)bounce : gptr(buf), n, rflags);
        if (r < 0 && rflags) { /* nothing to read as urgent after all */
            rflags = 0;
            r = recv(p->fd, bounce ? (void *)bounce : gptr(buf), n, 0);
        }
        if (r > 0 && rflags && flags)
            wr32(flags, T_EXPEDITED);
        if (r > 0 && bounce) { /* the bytes as a chain of OTBuffer */
            gaddr head = ot_buffer_new(bounce, (uint32_t)r);
            wr32(buf, head);
            RETURN(head ? (uint32_t)r : (uint32_t)kOTOutOfMemoryErr);
            return;
        }
        LOGF(LOG_TRAPS, "OT: rcv %d: %zd of %u%s%s\n", (int)(p - providers), r, n, r < 0 ? ", " : "",
             r < 0 ? strerror(errno) : "");
        if (r > 0) {
            RETURN((uint32_t)r);
            return;
        }
        if (r == 0) {
            p->eof = true;
            /* Found here before the notifier heard of it: Open Transport
             * would have told it T_ORDREL as the release came in, and an
             * asynchronous program takes kOTLookErr before that as the
             * connection failing (Internet Explorer 3 reports pages as not
             * loaded).  So the release is told, and until the notifier has
             * had it the call just has no data. */
            if (p->notifier && !p->heard_ordrel && !(p->blocking && !p->async)) {
                if (!p->told_ordrel) {
                    p->told_ordrel = true;
                    ot_post(p, T_ORDREL, 0, 0);
                }
                p->told_data = false;
                RETURN((uint32_t)kOTNoDataErr);
                return;
            }
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
        if (p->used && p->cancel_err) {
            RETURN((uint32_t)take_cancel(p));
            return;
        }
        p->told_data = false; /* the next data says so again */
        RETURN((uint32_t)kOTNoDataErr);
        return;
    }
}

/* OTLook(ep): the event waiting */
static void ot_Look(ppc_cpu *c)
{
    ENDPOINT(p, ota_Look);
    poll_provider(p);
    if (p->disconnected)
        RETURN(T_DISCONNECT);
    else if (p->state == T_OUTCON && p->connected)
        RETURN(T_CONNECT);
    else if (p->pending_fd >= 0)
        RETURN(T_LISTEN);
    else if (p->kind == P_TCP && p->fd >= 0 && p->told_exdata)
        RETURN(T_EXDATA);
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
    ENDPOINT(p, ota_CountDataBytes);
    int n = 0;
    if (p->fd >= 0)
        ioctl(p->fd, FIONREAD, &n);
    if (R(1))
        wr32(R(1), (uint32_t)n);
    RETURN(n > 0 ? kOTNoError : kOTNoDataErr);
}

static void ot_SndOrderlyDisconnect(ppc_cpu *c)
{
    ENDPOINT(p, ota_SndOrderlyDisconnect);
    if (p->fd >= 0)
        shutdown(p->fd, SHUT_WR);
    p->state = p->state == T_INREL ? T_IDLE : T_OUTREL;
    RETURN(kOTNoError);
}

static void ot_RcvOrderlyDisconnect(ppc_cpu *c)
{
    ENDPOINT(p, ota_RcvOrderlyDisconnect);
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
    ENDPOINT(p, ota_SndDisconnect);
    if (p->fd >= 0) {
        struct linger l = { 1, 0 };
        setsockopt(p->fd, SOL_SOCKET, SO_LINGER, &l, sizeof l);
        close(p->fd);
    }
    p->fd = make_socket(p->kind);
    p->state = T_IDLE;
    p->connecting = p->connected = p->eof = p->disconnected = false;
    p->told_connect = p->told_data = p->told_ordrel = p->told_disc = p->heard_ordrel = false;
    ot_complete(c, p, T_DISCONNECTCOMPLETE, kOTNoError, R(1));
}

/* OTRcvDisconnect(ep, discon TDiscon *) */
static void ot_RcvDisconnect(ppc_cpu *c)
{
    ENDPOINT(p, ota_RcvDisconnect);
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
    p->told_connect = p->told_data = p->told_ordrel = p->told_disc = p->heard_ordrel = false;
    RETURN(kOTNoError);
}

/* OTResolveAddress(ep, reqAddr, retAddr, timeout): a DNS address
 * ("host:port") as the InetAddress it names. */
static void ot_ResolveAddress(ppc_cpu *c)
{
    ENDPOINT(p, ota_ResolveAddress);
    gaddr req = R(1), ret = R(2);
    struct sockaddr_in sa;
    if (!req || !ret || !get_inet(rd32(req + 8), rd32(req + 4), &sa)) {
        RETURN((uint32_t)kOTBadAddressErr);
        return;
    }
    netbuf_put_inet(ret, &sa);
    ot_complete(c, p, T_RESOLVEADDRCOMPLETE, kOTNoError, ret);
}

static void ot_GetEndpointState(ppc_cpu *c)
{
    PROVIDER(p);
    poll_provider(p);
    RETURN((uint32_t)p->state);
}

/* OTGetProtAddress(ep, boundAddr TBind *, peerAddr TBind *) */
static void ot_GetProtAddress(ppc_cpu *c)
{
    ENDPOINT(p, ota_GetProtAddress);
    struct sockaddr_in local;
    socklen_t len = sizeof local;
    memset(&local, 0, sizeof local);
    if (p->fd >= 0)
        getsockname(p->fd, (struct sockaddr *)&local, &len);
    if (R(1))
        netbuf_put_inet(R(1), &local);
    if (R(2))
        netbuf_put_inet(R(2), &p->peer);
    ot_complete(c, p, T_GETPROTADDRCOMPLETE, kOTNoError, R(2));
}

/* ---- Datagrams: TUnitData is addr, opt, udata (TNetbufs) ---- */

static void ot_SndUData(ppc_cpu *c)
{
    ENDPOINT(p, ota_SndUData);
    gaddr ud = R(1);
    struct sockaddr_in sa;
    if (!ud || !get_inet(rd32(ud + 8), rd32(ud + 4), &sa)) {
        RETURN((uint32_t)kOTBadAddressErr);
        return;
    }
    ot_options_apply(p, ud + 12); /* per-datagram options (TTL, TOS, broadcast ...) */
    static uint8_t data[65536];
    uint32_t n = ot_data_get(data, rd32(ud + 32), rd32(ud + 28), sizeof data);
    ssize_t r = sendto(p->fd, data, n, 0, (struct sockaddr *)&sa, sizeof sa);
    RETURN(r < 0 ? (uint32_t)(errno == EAGAIN ? kOTFlowErr : errno_result(errno)) : kOTNoError);
}

static void ot_RcvUData(ppc_cpu *c)
{
    ENDPOINT(p, ota_RcvUData);
    gaddr ud = R(1);
    if (R(2))
        wr32(R(2), 0);
    for (;;) {
        struct sockaddr_in sa;
        socklen_t len = sizeof sa;
        bool nocopy = rd32(ud + 24) == kNetbufDataIsOTBufferStar; /* the buffer is an OTBuffer * to fill */
        static uint8_t nocopy_bytes[65536];
        if (g_code_map && rd32(ud + 24) && !nocopy)
            mem_code_range(rd32(ud + 32), rd32(ud + 24));
        ssize_t r = recvfrom(p->fd, nocopy ? (void *)nocopy_bytes : gptr(rd32(ud + 32)),
                             nocopy ? sizeof nocopy_bytes : rd32(ud + 24), 0, (struct sockaddr *)&sa, &len);
        if (r >= 0) {
            if (nocopy) {
                gaddr head = ot_buffer_new(nocopy_bytes, (uint32_t)r);
                wr32(rd32(ud + 32), head);
                wr32(ud + 28, (uint32_t)r);
            } else {
                wr32(ud + 28, (uint32_t)r);
            }
            netbuf_put_inet(ud, &sa);
            RETURN(kOTNoError);
            return;
        }
        if ((errno == EAGAIN || errno == EWOULDBLOCK) && p->blocking && !p->async && wait_fd(p, p->fd, POLLIN))
            continue;
        if (p->used && p->cancel_err) {
            RETURN((uint32_t)take_cancel(p));
            return;
        }
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
        ot_post(p, T_DNRSTRINGTOADDRCOMPLETE, r, R(2));
        RETURN(kOTNoError);
    } else {
        RETURN((uint32_t)r);
    }
}

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

/* OTInetStringToHost(str, &host): a dotted address (NetSprocket's, from
 * Spades Deluxe 2.0, reading its own address) to an InetHost. */
static void ot_InetStringToHost(ppc_cpu *c)
{
    char s[64] = "";
    for (int i = 0; i < 63 && R(0); i++)
        if (!(s[i] = (char)rd8(R(0) + (uint32_t)i)))
            break;
    struct in_addr a;
    if (inet_aton(s, &a) == 0) {
        RETURN((uint32_t)(int32_t)-3150); /* kOTBadAddressErr */
        return;
    }
    if (R(1))
        wr32(R(1), ntohl(a.s_addr));
    RETURN(0);
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

/* OTInitDNSAddress(DNSAddress *, char *str): the address's length */
static void ot_InitDNSAddress(ppc_cpu *c)
{
    gaddr a = R(0), str = R(1);
    uint32_t n = 0;
    while (str && n < 255 && rd8(str + n))
        n++;
    if (a) {
        wr16(a, AF_DNS);
        if (n)
            gmemcpy(a + 2, str, n);
        wr8(a + 2 + n, 0);
    }
    RETURN(2 + n);
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

/* OTStrCopy(dest, src), OTStrCat(dest, src): C strings. */
static void ot_StrCopy(ppc_cpu *c)
{
    gaddr d = R(0), s = R(1);
    uint32_t i = 0;
    if (d && s)
        do
            wr8(d + i, rd8(s + i));
        while (rd8(s + i++));
    RETURN(0);
}

static void ot_StrCat(ppc_cpu *c)
{
    gaddr d = R(0), s = R(1);
    uint32_t n = 0, i = 0;
    if (d && s) {
        while (rd8(d + n))
            n++;
        do
            wr8(d + n + i, rd8(s + i));
        while (rd8(s + i++));
    }
    RETURN(0);
}

/* OTStrEqual(a, b) and OTMemcmp(a, b, n): true when they are the same. */
static void ot_StrEqual(ppc_cpu *c)
{
    gaddr a = R(0), b = R(1);
    uint32_t i = 0;
    if (!a || !b)
        RETURN(a == b);
    else {
        while (rd8(a + i) == rd8(b + i) && rd8(a + i))
            i++;
        RETURN(rd8(a + i) == rd8(b + i));
    }
}

static void ot_Memcmp(ppc_cpu *c)
{
    gaddr a = R(0), b = R(1);
    uint32_t n = R(2), i = 0;
    while (i < n && rd8(a + i) == rd8(b + i))
        i++;
    RETURN(i == n);
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
/* OTList: a pointer to the first OTLink (fHead), each link pointing on
 * (fNext); the head of the list is first.  NetSprocket keeps its endpoints
 * and messages in these. */
static gaddr list_last(gaddr list)
{
    gaddr l = rd32(list);
    while (l && rd32(l))
        l = rd32(l);
    return l;
}
static bool list_remove(gaddr list, gaddr link)
{
    for (gaddr at = list; rd32(at); at = rd32(at))
        if (rd32(at) == link) {
            wr32(at, rd32(link));
            wr32(link, 0);
            return true;
        }
    return false;
}
static void ot_AddFirst(ppc_cpu *c)
{
    if (R(0) && R(1)) {
        wr32(R(1), rd32(R(0)));
        wr32(R(0), R(1));
    }
    c->pc = c->lr;
}
static void ot_AddLast(ppc_cpu *c)
{
    if (R(0) && R(1)) {
        wr32(R(1), 0);
        gaddr last = list_last(R(0));
        wr32(last ? last : R(0), R(1));
    }
    c->pc = c->lr;
}
static void ot_RemoveFirst(ppc_cpu *c)
{
    gaddr h = R(0) ? rd32(R(0)) : 0;
    if (h) {
        wr32(R(0), rd32(h));
        wr32(h, 0);
    }
    RETURN(h);
}
static void ot_RemoveLast(ppc_cpu *c)
{
    gaddr last = R(0) ? list_last(R(0)) : 0;
    if (last)
        list_remove(R(0), last);
    RETURN(last);
}
static void ot_GetFirst(ppc_cpu *c) { RETURN(R(0) ? rd32(R(0)) : 0); }
static void ot_GetLast(ppc_cpu *c) { RETURN(R(0) ? list_last(R(0)) : 0); }
static void ot_IsInList(ppc_cpu *c)
{
    gaddr l = R(0) ? rd32(R(0)) : 0;
    while (l && l != R(1))
        l = rd32(l);
    RETURN(l != 0);
}
static void ot_RemoveLink(ppc_cpu *c) { RETURN(R(0) && R(1) && list_remove(R(0), R(1))); }
static void ot_GetIndexedLink(ppc_cpu *c)
{
    gaddr l = R(0) ? rd32(R(0)) : 0;
    for (uint32_t i = R(1); l && i; i--)
        l = rd32(l);
    RETURN(l);
}
/* OTFindLink(list, proc, ref): the first link proc(ref, link) says yes to. */
static gaddr list_find(gaddr list, gaddr proc, gaddr ref)
{
    for (gaddr l = list ? rd32(list) : 0; l; l = rd32(l)) {
        uint32_t args[2] = { ref, l };
        if (proc && (ppc_call(proc, args, 2) & 0xFF))
            return l;
    }
    return 0;
}
static void ot_FindLink(ppc_cpu *c)
{
    gaddr list = R(0), proc = R(1), ref = R(2);
    gaddr l = list_find(list, proc, ref);
    RETURN(l);
}
static void ot_FindAndRemoveLink(ppc_cpu *c)
{
    gaddr list = R(0), proc = R(1), ref = R(2);
    gaddr l = list_find(list, proc, ref);
    if (l)
        list_remove(list, l);
    RETURN(l);
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

/* OTAlloc(ref, structType, fields, &err): an XTI structure with the buffers
 * its fields ask for, sized from the endpoint's TEndpointInfo. */
enum { T_BIND = 1, T_OPTMGMT, T_CALL, T_DIS, T_UNITDATA, T_UDERROR, T_INFO, T_REPLYDATA, T_REQUESTDATA, T_UNITREQUEST,
       T_UNITREPLY };
enum { F_ADDR = 1, F_OPT = 2, F_UDATA = 4 };

/* The structure's size and the netbufs it begins with, each with the field
 * that asks for it: 0 ends the list. */
typedef struct {
    uint32_t size;
    uint8_t nbufs[4]; /* F_ADDR, F_OPT, F_UDATA per netbuf, in order */
} structdef;

static const structdef *struct_def(uint32_t type)
{
    static const structdef defs[] = {
        [T_BIND] = { 16, { F_ADDR } },
        [T_OPTMGMT] = { 16, { F_OPT } },
        [T_CALL] = { 40, { F_ADDR, F_OPT, F_UDATA } },
        [T_DIS] = { 20, { F_UDATA } },
        [T_UNITDATA] = { 36, { F_ADDR, F_OPT, F_UDATA } },
        [T_UDERROR] = { 28, { F_ADDR, F_OPT } },
        [T_INFO] = { 32, { 0 } },
        [T_REPLYDATA] = { 28, { F_UDATA, F_OPT } },
        [T_REQUESTDATA] = { 28, { F_UDATA, F_OPT } },
        [T_UNITREQUEST] = { 40, { F_ADDR, F_OPT, F_UDATA } },
        [T_UNITREPLY] = { 28, { F_OPT, F_UDATA } },
    };
    return type >= 1 && type <= T_UNITREPLY ? &defs[type] : NULL;
}

static void alloc_netbuf(gaddr nb, uint32_t size)
{
    gaddr b = tb_NewPtr((int32_t)size, false, true);
    wr32(nb, b ? size : 0);
    wr32(nb + 4, 0);
    wr32(nb + 8, b);
}

/* The size of the buffer for a field, from the endpoint's own limits. */
static uint32_t field_size(provider *p, int field)
{
    uint8_t info[32] = { 0 };
    gaddr tmp = tb_NewPtr(32, true, true);
    uint32_t size = field == F_ADDR ? 16 : field == F_OPT ? 1024 : 1024;
    if (tmp && p) {
        ot_put_info(tmp, p->kind);
        gget(info, tmp, 32);
        int32_t v = (int32_t)(info[field == F_ADDR ? 0 : field == F_OPT ? 4 : 8] << 24 |
                              info[(field == F_ADDR ? 0 : field == F_OPT ? 4 : 8) + 1] << 16 |
                              info[(field == F_ADDR ? 0 : field == F_OPT ? 4 : 8) + 2] << 8 |
                              info[(field == F_ADDR ? 0 : field == F_OPT ? 4 : 8) + 3]);
        if (field == F_UDATA && v <= 0) /* a stream or no limit: a page's worth */
            v = 4096;
        if (v > 0)
            size = (uint32_t)v > 65536 ? 65536 : (uint32_t)v;
    }
    if (tmp)
        tb_DisposePtr(tmp);
    return size;
}

static void ot_Alloc(ppc_cpu *c)
{
    provider *p = ot_provider_of(R(0));
    uint32_t type = R(1), fields = R(2);
    const structdef *d = struct_def(type);
    if (!d) {
        if (R(3))
            wr32(R(3), (uint32_t)(-3149 - 20)); /* TNOSTRUCTYPE */
        RETURN(0);
        return;
    }
    gaddr t = tb_NewPtr((int32_t)d->size, true, true);
    if (t)
        for (int i = 0; i < 4 && d->nbufs[i]; i++)
            if (fields & d->nbufs[i])
                alloc_netbuf(t + 12 * (uint32_t)i, field_size(p, d->nbufs[i]));
    if (R(3))
        wr32(R(3), t ? 0 : (uint32_t)kOTOutOfMemoryErr);
    RETURN(t);
}

static void ot_Free(ppc_cpu *c) /* (ptr, structType) */
{
    gaddr t = R(0);
    const structdef *d = struct_def(R(1));
    if (!d) {
        RETURN((uint32_t)(-3149 - 20)); /* TNOSTRUCTYPE */
        return;
    }
    if (t) {
        for (int i = 0; i < 4 && d->nbufs[i]; i++)
            if (rd32(t + 12 * (uint32_t)i + 8))
                tb_DisposePtr(rd32(t + 12 * (uint32_t)i + 8));
        tb_DisposePtr(t);
    }
    RETURN(kOTNoError);
}

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
    { "OTGetEndpointState", ot_GetEndpointState },
    { "OTGetProtAddress", ot_GetProtAddress }, { "OTSndUData", ot_SndUData }, { "OTRcvUData", ot_RcvUData },
    { "OTRcvUDErr", ot_RcvUDErr }, { "OTAckSends", ot_AckSends }, { "OTDontAckSends", ot_AckSends },
    { "OTIsAckingSends", ot_AckSends }, { "OTResolveAddress", ot_ResolveAddress },
    /* OTInetClientLib */
    { "OTOpenInternetServices", ot_OpenInternetServices },
    { "OTOpenInternetServicesPriv", ot_OpenInternetServices },
    { "OTOpenInternetServicesInContext", ot_OpenInternetServices },
    { "OTAsyncOpenInternetServices", ot_AsyncOpenInternetServices },
    { "OTAsyncOpenInternetServicesPriv", ot_AsyncOpenInternetServices },
    { "OTAsyncOpenInternetServicesInContext", ot_AsyncOpenInternetServices },
    { "OTInetStringToAddress", ot_InetStringToAddress },
    { "OTInetHostToString", ot_InetHostToString }, { "OTInetStringToHost", ot_InetStringToHost }, { "OTInitInetAddress", ot_InitInetAddress },
    { "OTInitDNSAddress", ot_InitDNSAddress },
    /* OTUtilityLib */
    { "OTAllocMem", ot_AllocMem }, { "OTFreeMem", ot_FreeMem }, { "OTMemcpy", ot_MemcpyOrMove },
    { "OTMemmove", ot_MemcpyOrMove }, { "OTMemzero", ot_Memzero }, { "OTMemset", ot_Memset },
    { "OTStrLength", ot_StrLength }, { "OTStrCopy", ot_StrCopy }, { "OTStrCat", ot_StrCat },
    { "OTStrEqual", ot_StrEqual }, { "OTMemcmp", ot_Memcmp }, { "OTGetTimeStamp", ot_GetTimeStamp },
    { "OTElapsedMilliseconds", ot_ElapsedMilliseconds }, { "OTElapsedMicroseconds", ot_ElapsedMicroseconds },
    { "OTEnterNotifier", ot_EnterNotifier }, { "OTLeaveNotifier", ot_void }, { "OTIdle", ot_Idle },
    { "OTAllocMemPriv", ot_AllocMem }, { "OTFreeMemPriv", ot_FreeMem }, { "OTAllocMemInContext", ot_AllocMem },
    { "OTFreeMemInContext", ot_FreeMem }, { "OTLIFOEnqueue", ot_LIFOEnqueue }, { "OTLIFODequeue", ot_LIFODequeue },
    { "OTLIFOStealList", ot_LIFOStealList }, { "OTReverseList", ot_ReverseList },
    { "OTAddFirst", ot_AddFirst }, { "OTAddLast", ot_AddLast }, { "OTRemoveFirst", ot_RemoveFirst },
    { "OTRemoveLast", ot_RemoveLast }, { "OTGetFirst", ot_GetFirst }, { "OTGetLast", ot_GetLast },
    { "OTIsInList", ot_IsInList }, { "OTRemoveLink", ot_RemoveLink }, { "OTGetIndexedLink", ot_GetIndexedLink },
    { "OTFindLink", ot_FindLink }, { "OTFindAndRemoveLink", ot_FindAndRemoveLink },
    { "OTAtomicSetBit", ot_AtomicSetBit }, { "OTAtomicClearBit", ot_AtomicClearBit },
    { "OTAtomicTestBit", ot_AtomicTestBit }, { "OTAtomicAdd32", ot_AtomicAdd32 }, { "OTAtomicAdd16", ot_AtomicAdd16 },
    { "OTAtomicAdd8", ot_AtomicAdd8 }, { "OTCompareAndSwap32", ot_CompareAndSwap32 },
    { "OTCompareAndSwapPtr", ot_CompareAndSwap32 }, { "OTTimeStampInMilliseconds", ot_TimeStampInMilliseconds },
    { "OTDelay", ot_Delay }, { "OTBufferDataSize", ot_BufferDataSize }, { "OTReadBuffer", ot_ReadBuffer },
    { "OTAlloc", ot_Alloc }, { "OTAllocPriv", ot_Alloc },
    { "OTAllocInContext", ot_Alloc }, { "OTFree", ot_Free },
    { "OTIoctl", ot_unsupported },
};
const unsigned ot_export_count = sizeof ot_exports / sizeof ot_exports[0];
