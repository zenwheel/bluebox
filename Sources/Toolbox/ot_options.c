/*
 * Open Transport's option management: OTOptionManagement on an endpoint,
 * the option buffers (OTNextOption, OTFindOption) and their text form
 * (OTCreateOptions, OTCreateOptionString), and the table of options each
 * provider has, with what is known about them (OTHasOptionInfo,
 * OTGetOptionInfoByID, OTGetOptionInfoByName, OTGetProtocolLevel).
 *
 * The options are the host socket's where it has an equivalent (keep-alive,
 * reuse-address, TCP_NODELAY, TTL, TOS, broadcast, linger, buffer sizes,
 * multicast interface, TTL, loopback and group membership ...); the others
 * (thresholds, checksum switches, the AppleTalk ones) are kept as set and
 * read back, and AppleTalk's endpoints take theirs from the same store.
 * What was negotiated stays with the endpoint, and is applied again to the
 * socket an OTAccept hands it.
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

#include "ot_internal.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

enum {
    XTI_GENERIC = 0xFFFF, INET_IP = 0, INET_TCP = 6, INET_UDP = 0x11,
    ATK_DDP = 0x44445020, ATK_ATP = 0x41545020, ATK_ADSP = 0x41445350, ATK_ASP = 0x41535020, ATK_PAP = 0x50415020,
};
enum {
    T_NEGOTIATE = 0x0004, T_CHECK = 0x0008, T_DEFAULT = 0x0010, T_CURRENT = 0x0080, T_ABSREQ = 0x8000,
    T_SUCCESS = 0x0020, T_FAILURE = 0x0040, T_PARTSUCCESS = 0x0100, T_READONLY = 0x0200, T_NOTSUPPORT = 0x0400,
    T_ALLOPT = 0, kT_UNSPEC = 0xFFFFFFFD,
};
enum { OPT_HEADER = 16, MAX_OPT_VALUE = 40, MAX_STORED = 32 };

/* Which providers an option applies to. */
enum { K_TCP = 1, K_UDP = 2, K_DDP = 4, K_ATP = 8, K_ADSP = 16, K_PAP = 32, K_ASP = 64, K_ATALK = 124, K_INET = 3 };

/* How a value reads and prints. */
typedef enum { V_BOOL, V_BYTE, V_LONG, V_HOST, V_LINGER, V_KPALIVE, V_MCAST, V_BYTES } vtype;

typedef struct {
    uint32_t level, name;
    const char *label, *alias;
    vtype type;
    uint32_t size;     /* the value's size in bytes (V_BYTES: the most) */
    uint32_t kinds;
    bool readonly, writeonly;
    uint32_t def[2];   /* the default, as longs (V_BYTE: the low byte) */
} optdef;

#define Y(a, b) { a, b }
static const optdef optdefs[] = {
    /* XTI_GENERIC */
    { XTI_GENERIC, 0x0001, "XTI_DEBUG", "Debug", V_LONG, 4, K_INET | K_ATALK, false, false, Y(0, 0) },
    { XTI_GENERIC, 0x0080, "XTI_LINGER", "Linger", V_LINGER, 8, K_TCP, false, false, Y(0, kT_UNSPEC) },
    { XTI_GENERIC, 0x1001, "XTI_SNDBUF", "SndBuf", V_LONG, 4, K_INET, false, false, Y(32768, 0) },
    { XTI_GENERIC, 0x1002, "XTI_RCVBUF", "RcvBuf", V_LONG, 4, K_INET, false, false, Y(32768, 0) },
    { XTI_GENERIC, 0x1003, "XTI_SNDLOWAT", "SndLowat", V_LONG, 4, K_INET, false, false, Y(1, 0) },
    { XTI_GENERIC, 0x1004, "XTI_RCVLOWAT", "RcvLowat", V_LONG, 4, K_INET, false, false, Y(1, 0) },
    { XTI_GENERIC, 0x1005, "XTI_PROTOTYPE", "ProtoType", V_LONG, 4, K_INET, true, false, Y(0, 0) },
    { XTI_GENERIC, 0x0008, "OPT_KEEPALIVE", "KeepAlive", V_KPALIVE, 8, K_TCP, false, false, Y(0, kT_UNSPEC) },
    { XTI_GENERIC, 0x0600, "OPT_CHECKSUM", "Checksum", V_BOOL, 4, K_ATALK, false, false, Y(1, 0) },
    { XTI_GENERIC, 0x0601, "OPT_RETRYCNT", "RetryCnt", V_LONG, 4, K_ATP | K_PAP | K_ASP, false, false, Y(8, 0) },
    { XTI_GENERIC, 0x0602, "OPT_INTERVAL", "Interval", V_LONG, 4, K_ATP | K_PAP | K_ASP, false, false, Y(2000, 0) },
    { XTI_GENERIC, 0x0603, "OPT_ENABLEEOM", "EnableEOM", V_BYTE, 1, K_ADSP, false, false, Y(0, 0) },
    { XTI_GENERIC, 0x0604, "OPT_SELFSEND", "SelfSend", V_BOOL, 4, K_DDP | K_ATP | K_ADSP, false, false, Y(0, 0) },
    { XTI_GENERIC, 0x0605, "OPT_SERVERSTATUS", "ServerStatus", V_BYTES, 255, K_PAP | K_ASP, false, false, Y(0, 0) },
    { XTI_GENERIC, 0x0606, "OPT_ALERTENABLE", "AlertEnable", V_LONG, 4, K_ATALK, false, false, Y(0, 0) },
    /* INET_IP */
    { INET_IP, 0x01, "IP_OPTIONS", "Options", V_BYTES, 40, K_INET, false, false, Y(0, 0) },
    { INET_IP, 0x02, "IP_TOS", "TOS", V_BYTE, 1, K_INET, false, false, Y(0, 0) },
    { INET_IP, 0x03, "IP_TTL", "TTL", V_BYTE, 1, K_INET, false, false, Y(64, 0) },
    { INET_IP, 0x04, "IP_REUSEADDR", "ReuseAddr", V_BOOL, 4, K_INET, false, false, Y(0, 0) },
    { INET_IP, 0x10, "IP_DONTROUTE", "DontRoute", V_BOOL, 4, K_INET, false, false, Y(0, 0) },
    { INET_IP, 0x20, "IP_BROADCAST", "Broadcast", V_BOOL, 4, K_INET, false, false, Y(0, 0) },
    { INET_IP, 0x0200, "IP_REUSEPORT", "ReusePort", V_BOOL, 4, K_INET, false, false, Y(0, 0) },
    { INET_IP, 0x1002, "IP_HDRINCL", "HdrIncl", V_BOOL, 4, 0, false, false, Y(0, 0) },
    { INET_IP, 0x1005, "IP_RCVOPTS", "RcvOpts", V_BOOL, 4, K_UDP, false, false, Y(0, 0) },
    { INET_IP, 0x1007, "IP_RCVDSTADDR", "RcvDstAddr", V_BOOL, 4, K_UDP, false, false, Y(0, 0) },
    { INET_IP, 0x1010, "IP_MULTICAST_IF", "MulticastIf", V_HOST, 4, K_UDP, false, false, Y(0, 0) },
    { INET_IP, 0x1011, "IP_MULTICAST_TTL", "MulticastTTL", V_BYTE, 1, K_UDP, false, false, Y(1, 0) },
    { INET_IP, 0x1012, "IP_MULTICAST_LOOP", "MulticastLoop", V_BYTE, 1, K_UDP, false, false, Y(1, 0) },
    { INET_IP, 0x1013, "IP_ADD_MEMBERSHIP", "AddMembership", V_MCAST, 8, K_UDP, false, true, Y(0, 0) },
    { INET_IP, 0x1014, "IP_DROP_MEMBERSHIP", "DropMembership", V_MCAST, 8, K_UDP, false, true, Y(0, 0) },
    { INET_IP, 0x1015, "IP_BROADCAST_IFNAME", "BroadcastIfName", V_BYTES, 16, K_UDP, false, false, Y(0, 0) },
    { INET_IP, 0x1016, "IP_RCVIFADDR", "RcvIfAddr", V_BOOL, 4, K_UDP, false, false, Y(0, 0) },
    /* INET_TCP */
    { INET_TCP, 0x01, "TCP_NODELAY", "NoDelay", V_BOOL, 4, K_TCP, false, false, Y(0, 0) },
    { INET_TCP, 0x02, "TCP_MAXSEG", "MaxSeg", V_LONG, 4, K_TCP, false, false, Y(536, 0) },
    { INET_TCP, 0x08, "TCP_KEEPALIVE", "TCPKeepAlive", V_KPALIVE, 8, K_TCP, false, false, Y(0, kT_UNSPEC) },
    { INET_TCP, 0x10, "TCP_NOTIFY_THRESHOLD", "NotifyThreshold", V_LONG, 4, K_TCP, false, false, Y(0, 0) },
    { INET_TCP, 0x11, "TCP_ABORT_THRESHOLD", "AbortThreshold", V_LONG, 4, K_TCP, false, false, Y(0, 0) },
    { INET_TCP, 0x12, "TCP_CONN_NOTIFY_THRESHOLD", "ConnNotifyThreshold", V_LONG, 4, K_TCP, false, false, Y(0, 0) },
    { INET_TCP, 0x13, "TCP_CONN_ABORT_THRESHOLD", "ConnAbortThreshold", V_LONG, 4, K_TCP, false, false, Y(0, 0) },
    { INET_TCP, 0x14, "TCP_OOBINLINE", "OOBInline", V_BOOL, 4, K_TCP, false, false, Y(0, 0) },
    { INET_TCP, 0x15, "TCP_URGENT_PTR_TYPE", "UrgentPtrType", V_LONG, 4, K_TCP, false, false, Y(1, 0) },
    /* INET_UDP */
    { INET_UDP, 0x0600, "UDP_CHECKSUM", "UDPChecksum", V_BOOL, 4, K_UDP, false, false, Y(1, 0) },
    { INET_UDP, 0x02, "UDP_RX_ICMP", "RxICMP", V_BOOL, 4, K_UDP, false, false, Y(0, 0) },
    /* AppleTalk */
    { ATK_DDP, 0x0600, "DDP_OPT_CHECKSUM", "DDPChecksum", V_BOOL, 4, K_DDP, false, false, Y(1, 0) },
    { ATK_DDP, 0x2101, "DDP_OPT_SRCADDR", "SrcAddr", V_BYTES, 8, K_DDP, false, false, Y(0, 0) },
    { ATK_ATP, 0x2110, "ATP_OPT_REPLYCNT", "ReplyCnt", V_LONG, 4, K_ATP, false, false, Y(0, 0) },
    { ATK_ATP, 0x2111, "ATP_OPT_DATALEN", "DataLen", V_LONG, 4, K_ATP, false, false, Y(0, 0) },
    { ATK_ATP, 0x2112, "ATP_OPT_RELTIMER", "RelTimer", V_LONG, 4, K_ATP, false, false, Y(0, 0) },
    { ATK_ATP, 0x2113, "ATP_OPT_TRANID", "TranID", V_LONG, 4, K_ATP, false, false, Y(0, 0) },
    { ATK_PAP, 0x2120, "PAP_OPT_OPENRETRY", "OpenRetry", V_BOOL, 4, K_PAP, false, false, Y(0, 0) },
};
#define NOPTDEFS (sizeof optdefs / sizeof optdefs[0])

/* What a provider has negotiated: the values that differ from the default,
 * or that the host can't hold. */
typedef struct {
    uint32_t level, name, len;
    uint8_t val[MAX_OPT_VALUE];
} stored;
typedef struct ot_optstore {
    int n;
    stored e[MAX_STORED];
} optstore;

static uint32_t kind_mask(provider_kind k)
{
    switch (k) {
    case P_TCP: return K_TCP;
    case P_UDP: return K_UDP;
    case P_DDP: return K_DDP;
    case P_ATP: return K_ATP;
    case P_ADSP: return K_ADSP;
    case P_PAP: return K_PAP;
    case P_ASP: return K_ASP;
    default: return 0;
    }
}

static const optdef *def_for(uint32_t kinds, uint32_t level, uint32_t name)
{
    for (unsigned i = 0; i < NOPTDEFS; i++)
        if (optdefs[i].level == level && optdefs[i].name == name && (optdefs[i].kinds & kinds))
            return &optdefs[i];
    return NULL;
}

/* ---- The stored values ---- */

static stored *store_find(provider *p, uint32_t level, uint32_t name)
{
    if (!p->opts)
        return NULL;
    for (int i = 0; i < p->opts->n; i++)
        if (p->opts->e[i].level == level && p->opts->e[i].name == name)
            return &p->opts->e[i];
    return NULL;
}

static void store_put(provider *p, const optdef *d, const uint8_t *v, uint32_t len)
{
    if (!p->opts) {
        p->opts = calloc(1, sizeof *p->opts);
        if (!p->opts)
            return;
    }
    stored *s = store_find(p, d->level, d->name);
    if (!s) {
        if (p->opts->n >= MAX_STORED)
            return;
        s = &p->opts->e[p->opts->n++];
        s->level = d->level;
        s->name = d->name;
    }
    s->len = len > MAX_OPT_VALUE ? MAX_OPT_VALUE : len;
    memcpy(s->val, v, s->len);
}

void ot_options_free(provider *p)
{
    free(p->opts);
    p->opts = NULL;
}

/* ---- Values as bytes in guest (big-endian) order ---- */

static uint32_t get_long(const uint8_t *v) { return (uint32_t)v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3]; }

static void put_long(uint8_t *v, uint32_t x)
{
    v[0] = (uint8_t)(x >> 24);
    v[1] = (uint8_t)(x >> 16);
    v[2] = (uint8_t)(x >> 8);
    v[3] = (uint8_t)x;
}

static void default_value(const optdef *d, uint8_t *v)
{
    memset(v, 0, MAX_OPT_VALUE);
    switch (d->type) {
    case V_BYTE: v[0] = (uint8_t)d->def[0]; break;
    case V_BOOL: case V_LONG: case V_HOST: put_long(v, d->def[0]); break;
    case V_LINGER: case V_KPALIVE: put_long(v, d->def[0]); put_long(v + 4, d->def[1]); break;
    default: break;
    }
    if (d->level == XTI_GENERIC && d->name == 0x1005) /* XTI_PROTOTYPE */
        put_long(v, 0);
}

/* ---- The host socket behind an option ---- */

/* Negotiate the value onto the host socket: true if it took (or the option
 * lives only in the store). */
static bool host_set(provider *p, const optdef *d, const uint8_t *v)
{
    int fd = p->fd;
    if (fd < 0 || OT_ATALK(p))
        return true;
    int x = (int)get_long(v);
    int r = 0;
    bool kept = false;
    switch (d->level << 16 | d->name) {
    case XTI_GENERIC << 16 | 0x0080: { /* XTI_LINGER */
        struct linger l = { get_long(v) == 1, (int)get_long(v + 4) == (int)kT_UNSPEC ? 0 : (int)get_long(v + 4) };
        r = setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof l);
        break;
    }
    case XTI_GENERIC << 16 | 0x1001: r = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &x, sizeof x); break;
    case XTI_GENERIC << 16 | 0x1002: r = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &x, sizeof x); break;
    case XTI_GENERIC << 16 | 0x1003: r = setsockopt(fd, SOL_SOCKET, SO_SNDLOWAT, &x, sizeof x); kept = true; break;
    case XTI_GENERIC << 16 | 0x1004: r = setsockopt(fd, SOL_SOCKET, SO_RCVLOWAT, &x, sizeof x); kept = true; break;
    case XTI_GENERIC << 16 | 0x0008: case INET_TCP << 16 | 0x08: { /* keep-alive, the timeout in minutes */
        int on = get_long(v) == 1;
        r = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
        uint32_t mins = get_long(v + 4);
        if (r == 0 && on && mins != kT_UNSPEC && mins > 0) {
            int secs = (int)(mins > 35791394 ? 2147483640u : mins * 60);
#ifdef TCP_KEEPALIVE
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &secs, sizeof secs);
#elif defined(TCP_KEEPIDLE)
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &secs, sizeof secs);
#endif
        }
        break;
    }
    case INET_IP << 16 | 0x01: r = setsockopt(fd, IPPROTO_IP, IP_OPTIONS, v, d->size); break;
    case INET_IP << 16 | 0x02: { int t = v[0]; r = setsockopt(fd, IPPROTO_IP, IP_TOS, &t, sizeof t); break; }
    case INET_IP << 16 | 0x03: { int t = v[0]; r = setsockopt(fd, IPPROTO_IP, IP_TTL, &t, sizeof t); break; }
    case INET_IP << 16 | 0x04: r = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &x, sizeof x); break;
    case INET_IP << 16 | 0x10: r = setsockopt(fd, SOL_SOCKET, SO_DONTROUTE, &x, sizeof x); break;
    case INET_IP << 16 | 0x20: r = setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &x, sizeof x); break;
    case INET_IP << 16 | 0x0200: r = setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &x, sizeof x); break;
    case INET_IP << 16 | 0x1010: { /* the multicast interface */
        struct in_addr a = { htonl(get_long(v)) };
        r = setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &a, sizeof a);
        break;
    }
    case INET_IP << 16 | 0x1011: { unsigned char t = v[0]; r = setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &t, 1); break; }
    case INET_IP << 16 | 0x1012: { unsigned char t = v[0]; r = setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &t, 1); break; }
    case INET_IP << 16 | 0x1013: case INET_IP << 16 | 0x1014: { /* TIPAddMulticast: group, interface */
        struct ip_mreq m;
        m.imr_multiaddr.s_addr = htonl(get_long(v));
        m.imr_interface.s_addr = htonl(get_long(v + 4));
        r = setsockopt(fd, IPPROTO_IP, d->name == 0x1013 ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, &m, sizeof m);
        break;
    }
    case INET_TCP << 16 | 0x01: r = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &x, sizeof x); break;
    case INET_TCP << 16 | 0x02: r = setsockopt(fd, IPPROTO_TCP, TCP_MAXSEG, &x, sizeof x); kept = true; break;
    case INET_TCP << 16 | 0x14: r = setsockopt(fd, SOL_SOCKET, SO_OOBINLINE, &x, sizeof x); break;
    default: return true; /* only kept */
    }
    if (r != 0 && kept && (errno == ENOPROTOOPT || errno == EINVAL || errno == ENOTSUP))
        return true; /* the host doesn't hold it; the endpoint remembers it */
    return r == 0;
}

/* Read the current value from the socket: true if the host had it. */
static bool host_get(provider *p, const optdef *d, uint8_t *v)
{
    int fd = p->fd;
    if (fd < 0 || OT_ATALK(p))
        return false;
    int x = 0;
    socklen_t len = sizeof x;
    memset(v, 0, MAX_OPT_VALUE);
    switch (d->level << 16 | d->name) {
    case XTI_GENERIC << 16 | 0x0080: {
        struct linger l;
        len = sizeof l;
        if (getsockopt(fd, SOL_SOCKET, SO_LINGER, &l, &len))
            return false;
        put_long(v, l.l_onoff ? 1 : 0);
        put_long(v + 4, l.l_onoff ? (uint32_t)l.l_linger : kT_UNSPEC);
        return true;
    }
    case XTI_GENERIC << 16 | 0x1001: if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &x, &len)) return false; break;
    case XTI_GENERIC << 16 | 0x1002: if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &x, &len)) return false; break;
    case XTI_GENERIC << 16 | 0x1003: if (getsockopt(fd, SOL_SOCKET, SO_SNDLOWAT, &x, &len)) return false; break;
    case XTI_GENERIC << 16 | 0x1004: if (getsockopt(fd, SOL_SOCKET, SO_RCVLOWAT, &x, &len)) return false; break;
    case XTI_GENERIC << 16 | 0x1005: x = d->kinds & K_TCP && p->kind == P_TCP ? 6 : 17; break;
    case XTI_GENERIC << 16 | 0x0008: case INET_TCP << 16 | 0x08: {
        if (getsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &x, &len))
            return false;
        put_long(v, x ? 1 : 0);
        int secs = 0;
        len = sizeof secs;
        bool have = false;
#ifdef TCP_KEEPALIVE
        have = getsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &secs, &len) == 0;
#elif defined(TCP_KEEPIDLE)
        have = getsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &secs, &len) == 0;
#endif
        put_long(v + 4, x && have && secs > 0 ? (uint32_t)((secs + 59) / 60) : x ? 120 : kT_UNSPEC);
        return true;
    }
    case INET_IP << 16 | 0x01: { socklen_t n = MAX_OPT_VALUE; return getsockopt(fd, IPPROTO_IP, IP_OPTIONS, v, &n) == 0; }
    case INET_IP << 16 | 0x02: if (getsockopt(fd, IPPROTO_IP, IP_TOS, &x, &len)) return false; v[0] = (uint8_t)x; return true;
    case INET_IP << 16 | 0x03: if (getsockopt(fd, IPPROTO_IP, IP_TTL, &x, &len)) return false; v[0] = (uint8_t)x; return true;
    case INET_IP << 16 | 0x04: if (getsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &x, &len)) return false; x = x != 0; break;
    case INET_IP << 16 | 0x10: if (getsockopt(fd, SOL_SOCKET, SO_DONTROUTE, &x, &len)) return false; x = x != 0; break;
    case INET_IP << 16 | 0x20: if (getsockopt(fd, SOL_SOCKET, SO_BROADCAST, &x, &len)) return false; x = x != 0; break;
    case INET_IP << 16 | 0x0200: if (getsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &x, &len)) return false; x = x != 0; break;
    case INET_IP << 16 | 0x1010: {
        struct in_addr a;
        socklen_t n = sizeof a;
        if (getsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &a, &n))
            return false;
        put_long(v, ntohl(a.s_addr));
        return true;
    }
    case INET_IP << 16 | 0x1011: { unsigned char t; socklen_t n = 1; if (getsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &t, &n)) return false; v[0] = t; return true; }
    case INET_IP << 16 | 0x1012: { unsigned char t; socklen_t n = 1; if (getsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &t, &n)) return false; v[0] = t; return true; }
    case INET_TCP << 16 | 0x01: if (getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &x, &len)) return false; x = x != 0; break;
    case INET_TCP << 16 | 0x02: if (getsockopt(fd, IPPROTO_TCP, TCP_MAXSEG, &x, &len)) return false; break;
    case INET_TCP << 16 | 0x14: if (getsockopt(fd, SOL_SOCKET, SO_OOBINLINE, &x, &len)) return false; x = x != 0; break;
    default: return false;
    }
    put_long(v, (uint32_t)x);
    return true;
}

/* The current value: the host's, else what the endpoint was told, else the
 * default. */
static void current_value(provider *p, const optdef *d, uint8_t *v)
{
    stored *s = store_find(p, d->level, d->name);
    if (host_get(p, d, v))
        return;
    default_value(d, v);
    if (s)
        memcpy(v, s->val, s->len);
}

/* ---- OTOptionManagement ---- */

/* The status an option's value earns negotiating: T_SUCCESS, or why not. */
static uint32_t check_value(const optdef *d, const uint8_t *v, uint32_t len)
{
    if (d->readonly)
        return T_READONLY;
    if (d->type == V_BYTES ? len > d->size : len != d->size)
        return T_FAILURE;
    switch (d->type) {
    case V_BOOL: {
        uint32_t x = get_long(v);
        return x == 0 || x == 1 || x == kT_UNSPEC ? T_SUCCESS : T_FAILURE;
    }
    case V_LINGER: case V_KPALIVE: {
        uint32_t on = get_long(v);
        return on == 0 || on == 1 ? T_SUCCESS : T_FAILURE;
    }
    default: return T_SUCCESS;
    }
}

/* The value that stands for "the protocol's default" is resolved. */
static void resolve_unspec(const optdef *d, uint8_t *v)
{
    uint8_t dv[MAX_OPT_VALUE];
    default_value(d, dv);
    if (d->type == V_BYTE ? false : d->type == V_BOOL || d->type == V_LONG || d->type == V_HOST)
        if (get_long(v) == kT_UNSPEC)
            memcpy(v, dv, 4);
}

typedef struct {
    gaddr buf;
    uint32_t used, max;
    bool overflow;
} outbuf;

static void out_option(outbuf *o, uint32_t level, uint32_t name, uint32_t status, const uint8_t *v, uint32_t vlen)
{
    uint32_t total = OPT_HEADER + vlen, padded = (total + 3) & ~3u;
    if (o->used + padded > o->max) {
        o->overflow = true;
        return;
    }
    gaddr a = o->buf + o->used;
    wr32(a, total);
    wr32(a + 4, level);
    wr32(a + 8, name);
    wr32(a + 12, status);
    if (vlen)
        gput(a + OPT_HEADER, v, vlen);
    for (uint32_t i = total; i < padded; i++)
        wr8(a + i, 0);
    o->used += padded;
}

static uint32_t worse(uint32_t a, uint32_t b)
{
    static const uint32_t order[] = { T_SUCCESS, T_PARTSUCCESS, T_FAILURE, T_READONLY, T_NOTSUPPORT };
    int ia = 0, ib = 0;
    for (int i = 0; i < 5; i++) {
        if (order[i] == a)
            ia = i;
        if (order[i] == b)
            ib = i;
    }
    return ia >= ib ? a : b;
}

/* One option of a request on endpoint p (NULL: no endpoint to hold it). */
static uint32_t handle_option(provider *p, uint32_t flags, uint32_t level, uint32_t name, gaddr value, uint32_t vlen,
                              outbuf *o, bool apply)
{
    uint32_t kinds = p ? kind_mask(p->kind) : 0xFFFF;
    uint32_t cmd = flags & (T_NEGOTIATE | T_CHECK | T_DEFAULT | T_CURRENT);
    uint32_t overall = T_SUCCESS;
    if (name == T_ALLOPT && (cmd == T_DEFAULT || cmd == T_CURRENT)) { /* every option at the level */
        for (unsigned i = 0; i < NOPTDEFS; i++) {
            const optdef *d = &optdefs[i];
            if (d->level != level || !(d->kinds & kinds) || d->writeonly)
                continue;
            uint8_t v[MAX_OPT_VALUE];
            if (cmd == T_DEFAULT)
                default_value(d, v);
            else if (p)
                current_value(p, d, v);
            else
                default_value(d, v);
            out_option(o, level, d->name, T_SUCCESS, v, d->size);
        }
        return T_SUCCESS;
    }
    const optdef *d = def_for(kinds, level, name);
    if (!d || (d->writeonly && (cmd == T_DEFAULT || cmd == T_CURRENT))) {
        uint8_t v[MAX_OPT_VALUE];
        uint32_t n = vlen > MAX_OPT_VALUE ? MAX_OPT_VALUE : vlen;
        if (n)
            gget(v, value, n);
        out_option(o, level, name, T_NOTSUPPORT, v, n);
        return T_NOTSUPPORT;
    }
    uint8_t v[MAX_OPT_VALUE];
    memset(v, 0, sizeof v);
    uint32_t status = T_SUCCESS, outlen = d->size;
    if (cmd == T_DEFAULT) {
        default_value(d, v);
    } else if (cmd == T_CURRENT) {
        if (p)
            current_value(p, d, v);
        else
            default_value(d, v);
    } else { /* T_NEGOTIATE or T_CHECK */
        uint32_t n = vlen > MAX_OPT_VALUE ? MAX_OPT_VALUE : vlen;
        if (n)
            gget(v, value, n);
        status = check_value(d, v, vlen);
        outlen = d->type == V_BYTES ? vlen : d->size;
        if (status == T_SUCCESS) {
            resolve_unspec(d, v);
            if (cmd == T_NEGOTIATE && apply) {
                if (p && !host_set(p, d, v))
                    status = T_FAILURE;
                else if (p)
                    store_put(p, d, v, outlen);
            }
        } else if (status == T_READONLY && cmd == T_NEGOTIATE && p) {
            current_value(p, d, v);
        }
    }
    out_option(o, level, name, status, v, outlen);
    return worse(overall, status);
}

/* Walk an option buffer: false if it is malformed. */
static bool next_option(gaddr buf, uint32_t buflen, gaddr *opt)
{
    gaddr at = *opt ? *opt + ((rd32(*opt) + 3) & ~3u) : buf;
    if (at + OPT_HEADER > buf + buflen || (*opt && at >= buf + buflen))
        return false;
    uint32_t len = rd32(at);
    if (len < OPT_HEADER || at + len > buf + buflen)
        return false;
    *opt = at;
    return true;
}

/* Negotiate, check or read the options in a TOptMgmt request, writing the
 * results to the reply.  Returns an OSStatus; the reply's flags get the
 * worst status. */
int32_t ot_option_mgmt(provider *p, gaddr req, gaddr ret)
{
    if (!req || !ret || (!rd32(ret + 8) && rd32(req + 4)))
        return kOTBadOptionErr;
    uint32_t flags = rd32(req + 12);
    uint32_t cmd = flags & (T_NEGOTIATE | T_CHECK | T_DEFAULT | T_CURRENT);
    if (cmd != T_NEGOTIATE && cmd != T_CHECK && cmd != T_DEFAULT && cmd != T_CURRENT)
        return kOTBadOptionErr;
    gaddr inbuf = rd32(req + 8);
    uint32_t inlen = rd32(req + 4);
    LOGF(LOG_TRAPS, "OT: options flags %04X, %u bytes in, room %u\n", flags, inlen, rd32(ret));
    /* Asked and answered in the same buffer (Easy Serve's), it holds what it held, whatever
     * maxlen says. */
    uint32_t room = rd32(ret);
    if (rd32(ret + 8) == inbuf && room < inlen)
        room = inlen;
    outbuf o = { rd32(ret + 8), 0, room, false };
    uint32_t overall = T_SUCCESS;
    /* An absolute request is all or nothing: check first. */
    bool apply = true;
    if ((flags & T_ABSREQ) && cmd == T_NEGOTIATE) {
        for (gaddr opt = 0; inlen && next_option(inbuf, inlen, &opt);) {
            const optdef *d = def_for(p ? kind_mask(p->kind) : 0xFFFF, rd32(opt + 4), rd32(opt + 8));
            uint8_t v[MAX_OPT_VALUE] = { 0 };
            uint32_t n = rd32(opt) - OPT_HEADER;
            if (n > MAX_OPT_VALUE)
                n = MAX_OPT_VALUE;
            if (n)
                gget(v, opt + OPT_HEADER, n);
            if (!d || check_value(d, v, rd32(opt) - OPT_HEADER) != T_SUCCESS)
                apply = false;
        }
    }
    for (gaddr opt = 0; inlen && next_option(inbuf, inlen, &opt);) {
        uint32_t st = handle_option(p, flags, rd32(opt + 4), rd32(opt + 8), opt + OPT_HEADER, rd32(opt) - OPT_HEADER,
                                    &o, apply);
        overall = worse(overall, st);
    }
    if (o.overflow)
        return kOTBufferOverflowErr;
    wr32(ret + 4, o.used);
    wr32(ret + 12, overall);
    return kOTNoError;
}

/* Apply the options in a call's or datagram's option netbuf to an endpoint
 * (OTConnect, OTAccept, OTSndUData), writing the results back into place. */
void ot_options_apply(provider *p, gaddr nb)
{
    if (!nb || !rd32(nb + 8) || !rd32(nb + 4))
        return;
    uint32_t len = rd32(nb + 4);
    gaddr buf = rd32(nb + 8);
    for (gaddr opt = 0; next_option(buf, len, &opt);) {
        outbuf o = { 0, 0, 0, true }; /* the reply isn't kept */
        uint32_t st = handle_option(p, T_NEGOTIATE, rd32(opt + 4), rd32(opt + 8), opt + OPT_HEADER,
                                    rd32(opt) - OPT_HEADER, &o, true);
        wr32(opt + 12, st);
    }
}

/* Put the stored options on a new host socket (the one OTAccept gave an
 * endpoint). */
void ot_options_reapply(provider *p)
{
    if (!p->opts)
        return;
    uint32_t kinds = kind_mask(p->kind);
    for (int i = 0; i < p->opts->n; i++) {
        const optdef *d = def_for(kinds, p->opts->e[i].level, p->opts->e[i].name);
        if (d && d->name != 0x1013 && d->name != 0x1014)
            host_set(p, d, p->opts->e[i].val);
    }
}

/* AppleTalk's endpoints read what was negotiated: the long at (level, name),
 * or the default; false if the endpoint has no such option. */
bool ot_option_long(provider *p, uint32_t level, uint32_t name, uint32_t *out)
{
    const optdef *d = def_for(kind_mask(p->kind), level, name);
    if (!d)
        return false;
    uint8_t v[MAX_OPT_VALUE];
    current_value(p, d, v);
    *out = d->type == V_BYTE ? v[0] : get_long(v);
    return true;
}

/* The bytes stored for a variable-length option, if any. */
uint32_t ot_option_bytes(provider *p, uint32_t level, uint32_t name, uint8_t *out, uint32_t max)
{
    stored *s = store_find(p, level, name);
    if (!s)
        return 0;
    uint32_t n = s->len < max ? s->len : max;
    memcpy(out, s->val, n);
    return n;
}

/* OTOptionManagement(ep, req, ret) */
static void o_OptionManagement(ppc_cpu *c)
{
    provider *p = ot_provider_of(R(0));
    if (!p) {
        RETURN((uint32_t)kOTBadReferenceErr);
        return;
    }
    int32_t err = ot_option_mgmt(p, R(1), R(2));
    ot_complete(c, p, T_OPTMGMTCOMPLETE, err, R(2));
}

/* ---- OTNextOption, OTFindOption ---- */

/* OTNextOption(buffer, buflen, &opt): *opt the next option (the first when
 * it starts out NULL); kOTNoDataErr when there is none, kOTBadOptionErr when
 * the buffer is malformed. */
static void o_NextOption(ppc_cpu *c)
{
    gaddr buf = R(0), pp = R(2);
    uint32_t buflen = R(1);
    gaddr at = pp ? rd32(pp) : 0;
    if (!buf || !pp) {
        RETURN((uint32_t)kOTBadOptionErr);
        return;
    }
    gaddr next = at ? at + ((rd32(at) + 3) & ~3u) : buf;
    if (next >= buf + buflen || (at && at + OPT_HEADER > buf + buflen)) {
        RETURN((uint32_t)kOTNoDataErr);
        return;
    }
    if (next + OPT_HEADER > buf + buflen || rd32(next) < OPT_HEADER || next + rd32(next) > buf + buflen) {
        RETURN((uint32_t)kOTBadOptionErr);
        return;
    }
    wr32(pp, next);
    RETURN(kOTNoError);
}

/* OTFindOption(buffer, buflen, level, name): the option, or NULL. */
static void o_FindOption(ppc_cpu *c)
{
    gaddr buf = R(0);
    uint32_t buflen = R(1);
    for (gaddr opt = 0; buf && next_option(buf, buflen, &opt);)
        if (rd32(opt + 4) == R(2) && rd32(opt + 8) == R(3)) {
            RETURN(opt);
            return;
        }
    RETURN(0);
}

/* ---- Options as text ---- */

static void upcase(char *s)
{
    for (; *s; s++)
        *s = (char)toupper((unsigned char)*s);
}

/* The option a name means for an endpoint of the given name ("tcp", "udp",
 * "ddp" ...): the header's name (TCP_NODELAY, kIP_TTL) or the short one
 * (NoDelay, TTL), in any case. */
static uint32_t kinds_for_name(const char *endpt)
{
    provider_kind k;
    if (!endpt || !*endpt)
        return 0xFFFF;
    if (ota_kind(endpt, &k))
        return kind_mask(k);
    if (strstr(endpt, "udp"))
        return K_UDP;
    if (strstr(endpt, "tcp"))
        return K_TCP;
    return 0xFFFF;
}

static const optdef *def_by_name(uint32_t kinds, const char *name)
{
    char want[48];
    snprintf(want, sizeof want, "%s", name);
    upcase(want);
    char *w = want;
    if (w[0] == 'K' && !strncmp(w, "KIP_", 4))
        w++;
    for (int pass = 0; pass < 2; pass++)
        for (unsigned i = 0; i < NOPTDEFS; i++) {
            if (!(optdefs[i].kinds & kinds) && pass == 0)
                continue;
            char have[48];
            snprintf(have, sizeof have, "%s", pass == 0 ? optdefs[i].label : optdefs[i].alias);
            upcase(have);
            if (!strcmp(have, w))
                return &optdefs[i];
            /* the label without its protocol prefix */
            if (pass == 0) {
                const char *us = strchr(have, '_');
                if (us && !strcmp(us + 1, w) && (!strncmp(have, "TCP_", 4) || !strncmp(have, "IP_", 3) ||
                                                 !strncmp(have, "XTI_", 4) || !strncmp(have, "UDP_", 4) ||
                                                 !strncmp(have, "OPT_", 4)))
                    return &optdefs[i];
            }
        }
    return NULL;
}

static bool parse_bool(const char *s, uint32_t *out)
{
    if (!strcasecmp(s, "yes") || !strcasecmp(s, "on") || !strcasecmp(s, "true") || !strcasecmp(s, "T_YES")) {
        *out = 1;
        return true;
    }
    if (!strcasecmp(s, "no") || !strcasecmp(s, "off") || !strcasecmp(s, "false") || !strcasecmp(s, "T_NO")) {
        *out = 0;
        return true;
    }
    return false;
}

static bool parse_long(const char *s, uint32_t *out)
{
    if (parse_bool(s, out))
        return true;
    if (!strcasecmp(s, "T_UNSPEC")) {
        *out = kT_UNSPEC;
        return true;
    }
    char *end;
    unsigned long v = strtoul(s, &end, 0);
    if (end == s || *end)
        return false;
    *out = (uint32_t)v;
    return true;
}

static bool parse_host(const char *s, uint32_t *out)
{
    struct in_addr a;
    if (inet_aton(s, &a)) {
        *out = ntohl(a.s_addr);
        return true;
    }
    return parse_long(s, out);
}

/* One value into bytes: the number of bytes, or -1. */
static int parse_value(const optdef *d, const char *s, uint8_t *v)
{
    char tmp[96];
    snprintf(tmp, sizeof tmp, "%s", s);
    char *colon = strchr(tmp, ':');
    uint32_t a = 0, b = 0;
    memset(v, 0, MAX_OPT_VALUE);
    switch (d->type) {
    case V_BOOL: case V_LONG:
        if (!parse_long(tmp, &a)) return -1;
        put_long(v, a);
        return 4;
    case V_BYTE:
        if (!parse_long(tmp, &a) || a > 255) return -1;
        v[0] = (uint8_t)a;
        return 1;
    case V_HOST:
        if (!parse_host(tmp, &a)) return -1;
        put_long(v, a);
        return 4;
    case V_LINGER: case V_KPALIVE:
        b = kT_UNSPEC;
        if (colon) {
            *colon = 0;
            if (!parse_long(colon + 1, &b)) return -1;
        }
        if (!parse_long(tmp, &a)) return -1;
        put_long(v, a);
        put_long(v + 4, b);
        return 8;
    case V_MCAST:
        if (colon) {
            *colon = 0;
            if (!parse_host(colon + 1, &b)) return -1;
        }
        if (!parse_host(tmp, &a)) return -1;
        put_long(v, a);
        put_long(v + 4, b);
        return 8;
    case V_BYTES: {
        size_t n = strlen(s);
        if (n > d->size) return -1;
        memcpy(v, s, n);
        return (int)n;
    }
    }
    return -1;
}

/* OTCreateOptions(endPtName, &str, &netbuf): the "name=value" pairs, comma
 * or space apart, at *str, as options in the netbuf (its len set).  *str is
 * left where the parse stopped: at the end, at a ')', or at the pair that
 * wasn't an option (kOTBadOptionErr). */
static void o_CreateOptions(ppc_cpu *c)
{
    char endpt[64] = "";
    for (uint32_t i = 0; R(0) && i < 63 && rd8(R(0) + i); i++)
        endpt[i] = (char)rd8(R(0) + i);
    uint32_t kinds = kinds_for_name(endpt);
    gaddr strp = R(1), nb = R(2);
    if (!strp || !rd32(strp) || !nb) {
        RETURN((uint32_t)kOTBadOptionErr);
        return;
    }
    gaddr s = rd32(strp);
    outbuf o = { rd32(nb + 8), 0, rd32(nb), false };
    int32_t err = kOTNoError;
    for (;;) {
        while (rd8(s) == ' ' || rd8(s) == ',' || rd8(s) == '\t')
            s++;
        if (!rd8(s) || rd8(s) == ')')
            break;
        gaddr start = s;
        char name[48] = "", value[96] = "";
        int n = 0;
        while (rd8(s) && rd8(s) != '=' && rd8(s) != ',' && rd8(s) != ')' && rd8(s) != ' ' && n < 47)
            name[n++] = (char)rd8(s++);
        name[n] = 0;
        bool have_value = false;
        if (rd8(s) == '=') {
            s++;
            have_value = true;
            n = 0;
            while (rd8(s) && rd8(s) != ',' && rd8(s) != ')' && rd8(s) != ' ' && n < 95)
                value[n++] = (char)rd8(s++);
            value[n] = 0;
        }
        const optdef *d = def_by_name(kinds, name);
        uint8_t v[MAX_OPT_VALUE];
        int len = d ? (have_value ? parse_value(d, value, v) : (d->type == V_BOOL ? parse_value(d, "1", v) : -1)) : -1;
        if (!d || len < 0) {
            err = kOTBadOptionErr;
            s = start;
            break;
        }
        out_option(&o, d->level, d->name, T_SUCCESS, v, (uint32_t)len);
    }
    wr32(strp, s);
    if (o.overflow && err == kOTNoError)
        err = kOTBufferOverflowErr;
    wr32(nb + 4, o.used);
    RETURN((uint32_t)err);
}

/* The value of an option as text. */
static void format_value(const optdef *d, const uint8_t *v, uint32_t len, char *out, size_t size)
{
    struct in_addr a;
    switch (d->type) {
    case V_BOOL: snprintf(out, size, "%s", get_long(v) == 1 ? "yes" : get_long(v) == 0 ? "no" : "unspec"); break;
    case V_BYTE: snprintf(out, size, "%u", v[0]); break;
    case V_LONG: snprintf(out, size, get_long(v) == kT_UNSPEC ? "unspec" : "%u", get_long(v)); break;
    case V_HOST:
        a.s_addr = htonl(get_long(v));
        snprintf(out, size, "%s", inet_ntoa(a));
        break;
    case V_LINGER: case V_KPALIVE:
        if (get_long(v + 4) == kT_UNSPEC)
            snprintf(out, size, "%u:unspec", get_long(v));
        else
            snprintf(out, size, "%u:%u", get_long(v), get_long(v + 4));
        break;
    case V_MCAST: {
        char g[20];
        a.s_addr = htonl(get_long(v));
        snprintf(g, sizeof g, "%s", inet_ntoa(a));
        a.s_addr = htonl(get_long(v + 4));
        snprintf(out, size, "%s:%s", g, inet_ntoa(a));
        break;
    }
    case V_BYTES: snprintf(out, size, "%.*s", (int)len, (const char *)v); break;
    }
}

/* OTCreateOptionString(endPtName, &opt, bufEnd, str, stringSize): the option
 * at *opt as "name=value" in str, *opt moved on to the one after (NULL at the
 * end of the buffer).  kOTNoDataErr when *opt is NULL or past bufEnd. */
static void o_CreateOptionString(ppc_cpu *c)
{
    char endpt[64] = "";
    for (uint32_t i = 0; R(0) && i < 63 && rd8(R(0) + i); i++)
        endpt[i] = (char)rd8(R(0) + i);
    uint32_t kinds = kinds_for_name(endpt);
    gaddr pp = R(1), end = R(2), str = R(3);
    uint32_t size = R(4);
    gaddr opt = pp ? rd32(pp) : 0;
    if (!opt || opt + OPT_HEADER > end || !str || !size) {
        RETURN((uint32_t)kOTNoDataErr);
        return;
    }
    uint32_t len = rd32(opt);
    if (len < OPT_HEADER || opt + len > end) {
        RETURN((uint32_t)kOTBadOptionErr);
        return;
    }
    const optdef *d = def_for(kinds, rd32(opt + 4), rd32(opt + 8));
    if (!d)
        d = def_for(0xFFFF, rd32(opt + 4), rd32(opt + 8)); /* named though this endpoint lacks it */
    char text[160], value[96] = "";
    if (d) {
        uint8_t v[MAX_OPT_VALUE] = { 0 };
        uint32_t n = len - OPT_HEADER > MAX_OPT_VALUE ? MAX_OPT_VALUE : len - OPT_HEADER;
        if (n)
            gget(v, opt + OPT_HEADER, n);
        format_value(d, v, n, value, sizeof value);
        snprintf(text, sizeof text, "%s=%s", d->label, value);
    } else {
        snprintf(text, sizeof text, "%08X.%08X", rd32(opt + 4), rd32(opt + 8));
    }
    if (strlen(text) + 1 > size) {
        RETURN((uint32_t)kOTBufferOverflowErr);
        return;
    }
    gput(str, text, (uint32_t)strlen(text) + 1);
    gaddr next = opt + ((len + 3) & ~3u);
    wr32(pp, next + OPT_HEADER <= end ? next : 0);
    RETURN(kOTNoError);
}

/* ---- What is known of the options ---- */

/* An option's description, as OTGetOptionInfoByID and ByName fill it:
 * level, name, the value's size, flags (1 read-only, 2 write-only, 4 holds
 * a boolean) and the option's name. */
static void put_info(gaddr info, const optdef *d)
{
    if (!info)
        return;
    gmemset(info, 0, 52);
    wr32(info, d->level);
    wr32(info + 4, d->name);
    wr32(info + 8, d->size);
    wr32(info + 12, (d->readonly ? 1u : 0) | (d->writeonly ? 2u : 0) | (d->type == V_BOOL ? 4u : 0));
    gput(info + 16, d->label, (uint32_t)strlen(d->label));
}

/* OTHasOptionInfo(level, name): whether Open Transport knows the option. */
static void o_HasOptionInfo(ppc_cpu *c) { RETURN(def_for(0xFFFF, R(0), R(1)) != NULL); }

/* OTGetOptionInfoByID(level, name, info) */
static void o_GetOptionInfoByID(ppc_cpu *c)
{
    const optdef *d = def_for(0xFFFF, R(0), R(1));
    if (!d) {
        RETURN((uint32_t)kOTNotFoundErr);
        return;
    }
    put_info(R(2), d);
    RETURN(kOTNoError);
}

/* OTGetOptionInfoByName(name, info) */
static void o_GetOptionInfoByName(ppc_cpu *c)
{
    char name[48] = "";
    for (uint32_t i = 0; R(0) && i < 47 && rd8(R(0) + i); i++)
        name[i] = (char)rd8(R(0) + i);
    const optdef *d = def_by_name(0xFFFF, name);
    if (!d) {
        RETURN((uint32_t)kOTNotFoundErr);
        return;
    }
    put_info(R(1), d);
    RETURN(kOTNoError);
}

/* OTGetProtocolLevel(protocolName): the XTI level the protocol's options
 * are under; XTI_GENERIC for a name that has none of its own. */
static void o_GetProtocolLevel(ppc_cpu *c)
{
    char name[32] = "";
    for (uint32_t i = 0; R(0) && i < 31 && rd8(R(0) + i); i++)
        name[i] = (char)rd8(R(0) + i);
    static const struct { const char *n; uint32_t level; } levels[] = {
        { "ip", INET_IP }, { "tcp", INET_TCP }, { "udp", INET_UDP }, { "rawip", INET_IP }, { "ddp", ATK_DDP },
        { "atp", ATK_ATP }, { "adsp", ATK_ADSP }, { "asp", ATK_ASP }, { "pap", ATK_PAP },
    };
    for (unsigned i = 0; i < sizeof levels / sizeof levels[0]; i++)
        if (!strcasecmp(levels[i].n, name)) {
            RETURN(levels[i].level);
            return;
        }
    RETURN(XTI_GENERIC);
}

const ppc_export oto_exports[] = {
    { "OTOptionManagement", o_OptionManagement },
    { "OTNextOption", o_NextOption }, { "OTFindOption", o_FindOption },
    { "OTCreateOptions", o_CreateOptions }, { "OTCreateOptionString", o_CreateOptionString },
    { "OTHasOptionInfo", o_HasOptionInfo }, { "OTGetOptionInfoByID", o_GetOptionInfoByID },
    { "OTGetOptionInfoByName", o_GetOptionInfoByName }, { "OTGetProtocolLevel", o_GetProtocolLevel },
};
const unsigned oto_export_count = sizeof oto_exports / sizeof oto_exports[0];
