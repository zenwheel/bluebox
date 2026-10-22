/*
 * MacTCP on the host's sockets: the .IPP driver's TCP and UDP streams
 * (PBControl calls, by csCode, as the MacTCP Programmer's Guide gives
 * them), ipctlGetAddr, and the Domain Name Resolver that applications load
 * from a 'dnrp' resource in the MacTCP control panel (dnr.c's OpenResolver),
 * here a jump to native code over the host's resolver.
 *
 * A call that can finish now does; one that has to wait (an open, a
 * receive, a send the socket cannot take yet) blocks if it was made
 * synchronously and, if asynchronously, stays in progress (ioResult 1)
 * until mactcp_poll, run at interrupt time, finishes it and calls its
 * completion routine.  The same polling calls each stream's notification
 * routine (ASR) when data arrives or the other end closes.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "forks.h"
#include "machine.h"
#include "tb_internal.h"
#include "trap.h"

void machine_pass_tick(void); /* trap.c: time passes while waiting */

/* csCodes */
enum {
    ipctlGetAddr = 15,
    UDPCreate = 20, UDPRead, UDPBfrReturn, UDPWrite, UDPRelease, UDPMaxMTUSize, UDPStatus,
    TCPCreate = 30, TCPPassiveOpen, TCPActiveOpen, TCPSend = 34, TCPNoCopyRcv, TCPRcvBfrReturn, TCPRcv, TCPClose,
    TCPAbort, TCPStatus, TCPExtendedStat, TCPRelease, TCPGlobalInfo,
};

/* Results */
enum {
    connectionClosing = -23005,
    invalidLength = -23006,
    connectionExists = -23007,
    connectionDoesntExist = -23008,
    insufficientResources = -23009,
    invalidStreamPtr = -23010,
    streamAlreadyOpen = -23011,
    connectionTerminated = -23012,
    invalidBufPtr = -23013,
    invalidRDS = -23014,
    openFailed = -23015,
    commandTimeout = -23016,
    duplicateSocket = -23017,
    nameSyntaxErr = -23041,
    cacheFault = -23042,
    noNameServer = -23044,
    noAnsErr = -23046,
    dnrErr = -23047,
};

/* Notification (ASR) event codes */
enum { TCPClosing = 1, TCPTerminate = 3, TCPDataArrival = 4 };
enum { UDPDataArrival = 1 };
enum { TCPRemoteAbort = 2 }; /* terminReason */

/* Connection states, as TCPStatus reports them */
enum { stClosed = 0, stListen = 2, stSynSent = 6, stEstablished = 8, stFinWait1 = 10, stCloseWait = 14 };

/* Parameter block offsets: the header up to csCode, the stream at 28, and
 * the call's parameters from 32.  UDP's, which the generated definitions
 * do not have, are MacTCP.h's (68k alignment). */
enum { PB_STREAM = 28, CS = 32 };
enum { UDPC_RCVBUFF = 0, UDPC_RCVLEN = 4, UDPC_NOTIFY = 8, UDPC_LOCALPORT = 12, UDPC_USERDATA = 14 };
enum { UDPS_REMOTEHOST = 2, UDPS_REMOTEPORT = 6, UDPS_WDS = 8, UDPS_LOCALPORT = 20 };
enum { UDPR_TIMEOUT = 0, UDPR_REMOTEHOST = 2, UDPR_REMOTEPORT = 6, UDPR_RCVBUFF = 8, UDPR_RCVLEN = 12,
       UDPR_DESTHOST = 20, UDPR_DESTPORT = 24 };
enum { TCPG_MAXCONN = 16 };

#define MAX_STREAMS 64
#define MAX_OPS 256

typedef struct {
    bool used, udp;
    gaddr stream;        /* the StreamPtr: the receive buffer the program gave */
    gaddr rcvbuf;
    uint32_t rcvlen;
    gaddr notify, userdata;
    int fd, lfd;         /* the connection; a passive open's listening socket */
    int state;
    uint32_t lhost, rhost;
    uint16_t lport, rport;
    bool told_data, told_closing, told_term, eof, reset;
} mstream;

typedef struct {
    bool used;
    gaddr pb;
    int code;
    int s;               /* stream index */
    uint64_t deadline;   /* machine time; 0 for none */
    uint32_t sent;       /* a send's progress */
    int16_t result;      /* ioInProgress, or the result to complete with */
} op;

static mstream streams[MAX_STREAMS];
static op ops[MAX_OPS];
/* Resolver answers whose result procedures are still to be called. */
static struct {
    gaddr proc, info, userdata;
    int16_t err;
} answers[16];
static int nanswers;
/* Whether anything is open or under way (a stream, an early connection,
 * an operation, a resolver's answer): mactcp_poll runs with every event
 * call, and a program polling the queue in a loop otherwise paid for
 * walking every empty slot each time (Spacestation Pheta). */
static bool active;
static bool polling;

static uint64_t now_ns(void) { return machine_time_ns(); }

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

/* The address a connection is made from: the host's, as the other end
 * sees it (127.0.0.1 for a connection to this host). */
static uint32_t connection_address(int fd)
{
    struct sockaddr_in la;
    socklen_t len = sizeof la;
    if (fd >= 0 && getsockname(fd, (struct sockaddr *)&la, &len) == 0 && la.sin_family == AF_INET &&
        la.sin_addr.s_addr != 0)
        return ntohl(la.sin_addr.s_addr);
    return local_address();
}

static int stream_index(gaddr stream)
{
    for (int i = 0; i < MAX_STREAMS; i++)
        if (streams[i].used && streams[i].stream == stream && stream)
            return i;
    return -1;
}

static void set_nonblocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

static struct sockaddr_in inet(uint32_t host, uint16_t port)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(host);
    sa.sin_port = htons(port);
    return sa;
}

static void close_fd(int *fd)
{
    if (*fd >= 0)
        close(*fd);
    *fd = -1;
}

/* An FTP PORT command names the port the program will listen on, but Fetch
 * opens that listener only after the server has answered the command, and
 * servers that connect as soon as they read it (pyftpdlib) found nothing
 * there.  MacTCP seems to have ignored a connection to a port no one
 * listened on, so the server's retry a moment later found the listener; a
 * host socket refuses it at once.  So a PORT command a program sends
 * starts the listener here, the passive open for that port takes it over,
 * and one not taken within a minute is closed. */
#define MAX_EARLY 4
static struct {
    bool used;
    int fd;
    uint16_t port;
    uint64_t made;
} early[MAX_EARLY];

static void listen_early(const uint8_t *buf, uint32_t len)
{
    if (len < 16 || memcmp(buf, "PORT ", 5) != 0)
        return;
    char line[64];
    uint32_t n = len < sizeof line - 1 ? len : sizeof line - 1;
    memcpy(line, buf, n);
    line[n] = 0;
    unsigned h[4], p[2];
    if (sscanf(line + 5, "%u,%u,%u,%u,%u,%u", &h[0], &h[1], &h[2], &h[3], &p[0], &p[1]) != 6 || p[0] > 255 ||
        p[1] > 255)
        return;
    uint16_t port = (uint16_t)(p[0] << 8 | p[1]);
    int slot = -1;
    for (int i = 0; i < MAX_EARLY; i++) {
        if (early[i].used && early[i].port == port)
            return;
        if (!early[i].used && slot < 0)
            slot = i;
    }
    if (slot < 0 || port == 0)
        return;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in la = inet(0, port);
    if (bind(fd, (struct sockaddr *)&la, sizeof la) < 0 || listen(fd, 1) < 0) {
        close(fd);
        return;
    }
    set_nonblocking(fd);
    early[slot].used = true;
    active = true;
    early[slot].fd = fd;
    early[slot].port = port;
    early[slot].made = now_ns();
}

/* The listener a PORT command started for this port, or -1. */
static int take_early(uint16_t port)
{
    for (int i = 0; i < MAX_EARLY; i++)
        if (early[i].used && early[i].port == port && port) {
            early[i].used = false;
            return early[i].fd;
        }
    return -1;
}

static void expire_early(bool all)
{
    for (int i = 0; i < MAX_EARLY; i++)
        if (early[i].used && (all || now_ns() - early[i].made > 60000000000ull)) {
            close(early[i].fd);
            early[i].used = false;
        }
}

/* ---------------------------------------------------------------------- */
/* Notification routines: pascal (stream, event, userData[, terminReason],
 * ICMPReport *) for TCP and UDP. */

static void notify(mstream *m, uint16_t event, uint16_t reason)
{
    if (!m->notify)
        return;
    cpu68k *c = g_cpu;
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d);
    memcpy(a, c->a, sizeof a);
    uint16_t sr = c->sr;
    cpu_push32(c, m->stream);
    cpu_push16(c, event);
    cpu_push32(c, m->userdata);
    if (!m->udp)
        cpu_push16(c, reason);
    cpu_push32(c, 0); /* no ICMP report */
    guest_call_regs(m->notify);
    memcpy(c->d, d, sizeof d);
    memcpy(c->a, a, sizeof a); /* the routine popped its arguments (pascal) */
    c->sr = sr;
}

/* An asynchronous call finishes: its result, then its completion routine,
 * with the parameter block in A0 and on the stack (MacTCP's are C). */
static void complete(gaddr pb, int16_t result)
{
    wr16(pb + IOParam_ioResult_off, (uint16_t)result);
    gaddr fn = rd32(pb + IOParam_ioCompletion_off);
    if (!fn)
        return;
    cpu68k *c = g_cpu;
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d);
    memcpy(a, c->a, sizeof a);
    uint16_t sr = c->sr;
    c->a[0] = pb;
    c->d[0] = (uint32_t)(int32_t)result;
    cpu_push32(c, pb);
    guest_call_regs(fn);
    memcpy(c->d, d, sizeof d);
    memcpy(c->a, a, sizeof a);
    c->sr = sr;
}

/* ---------------------------------------------------------------------- */
/* Calls that may wait.  step: the call's result, or ioInProgress while it
 * has to wait for its socket. */

static uint32_t wds_length(gaddr wds)
{
    uint32_t n = 0;
    for (int i = 0; wds && i < 64; i++) {
        uint16_t len = rd16(wds + (uint32_t)i * 6);
        if (!len)
            break;
        n += len;
    }
    return n;
}

/* The bytes of a write data structure, from byte `from` on. */
static uint8_t *wds_gather(gaddr wds, uint32_t *total)
{
    uint32_t n = wds_length(wds);
    uint8_t *buf = malloc(n ? n : 1);
    uint32_t at = 0;
    for (int i = 0; wds && i < 64; i++) {
        uint16_t len = rd16(wds + (uint32_t)i * 6);
        if (!len)
            break;
        gget(buf + at, rd32(wds + (uint32_t)i * 6 + 2), len);
        at += len;
    }
    *total = n;
    return buf;
}

static int16_t errno_result(int e)
{
    switch (e) {
    case ECONNREFUSED:
    case EHOSTUNREACH:
    case ENETUNREACH: return openFailed;
    case ECONNRESET:
    case EPIPE: return connectionTerminated;
    case ETIMEDOUT: return commandTimeout;
    case EADDRINUSE: return duplicateSocket;
    default: return connectionDoesntExist;
    }
}

static bool timed_out(op *o) { return o->deadline && now_ns() >= o->deadline; }

static int16_t step_open(op *o, mstream *m)
{
    gaddr p = o->pb + CS;
    if (o->code == TCPActiveOpen) {
        if (m->fd < 0) {
            uint32_t host = rd32(p + TCPOpenPB_remoteHost_off);
            uint16_t port = rd16(p + TCPOpenPB_remotePort_off);
            m->fd = socket(AF_INET, SOCK_STREAM, 0);
            if (m->fd < 0)
                return insufficientResources;
            set_nonblocking(m->fd);
            int one = 1;
            setsockopt(m->fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
            uint16_t lport = rd16(p + TCPOpenPB_localPort_off);
            if (lport) {
                struct sockaddr_in la = inet(0, lport);
                bind(m->fd, (struct sockaddr *)&la, sizeof la);
            }
            struct sockaddr_in sa = inet(host, port);
            m->rhost = host;
            m->rport = port;
            m->state = stSynSent;
            int cr = connect(m->fd, (struct sockaddr *)&sa, sizeof sa);
            if (cr < 0 && errno != EINPROGRESS) {
                int16_t r = errno_result(errno);
                close_fd(&m->fd);
                m->state = stClosed;
                return r == connectionDoesntExist ? openFailed : r;
            }
            /* Our end's address is known from the start, as MacTCP had it
             * while the connection was still being made (Fetch takes it
             * then for its FTP PORT commands). */
            m->lhost = connection_address(m->fd);
            struct sockaddr_in la2;
            socklen_t l2 = sizeof la2;
            if (getsockname(m->fd, (struct sockaddr *)&la2, &l2) == 0)
                m->lport = ntohs(la2.sin_port);
        }
        struct pollfd pf = { m->fd, POLLOUT, 0 };
        if (poll(&pf, 1, 0) <= 0)
            return timed_out(o) ? commandTimeout : ioInProgress;
        int err = 0;
        socklen_t len = sizeof err;
        getsockopt(m->fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err) {
            close_fd(&m->fd);
            m->state = stClosed;
            int16_t r = errno_result(err);
            return r == connectionDoesntExist ? openFailed : r;
        }
    } else { /* TCPPassiveOpen */
        if (m->lfd < 0 && m->fd < 0) {
            struct sockaddr_in la = inet(0, rd16(p + TCPOpenPB_localPort_off));
            m->lfd = take_early(rd16(p + TCPOpenPB_localPort_off));
            if (m->lfd < 0) {
                m->lfd = socket(AF_INET, SOCK_STREAM, 0);
                if (m->lfd < 0)
                    return insufficientResources;
                int one = 1;
                setsockopt(m->lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
                if (bind(m->lfd, (struct sockaddr *)&la, sizeof la) < 0 || listen(m->lfd, 1) < 0) {
                    int16_t r = errno_result(errno);
                    close_fd(&m->lfd);
                    return r;
                }
                set_nonblocking(m->lfd);
            }
            socklen_t len = sizeof la;
            getsockname(m->lfd, (struct sockaddr *)&la, &len);
            wr16(p + TCPOpenPB_localPort_off, ntohs(la.sin_port)); /* the port chosen for 0 */
            /* and our address, as MacTCP filled it in at once: programs put
             * it in an FTP PORT command (Fetch).  The one the program's
             * other connections are made from, as the other end knows us. */
            uint32_t ours = local_address();
            for (int k = 0; k < MAX_STREAMS; k++)
                if (streams[k].used && !streams[k].udp && streams[k].fd >= 0 && &streams[k] != m &&
                    streams[k].state == stEstablished) {
                    ours = connection_address(streams[k].fd);
                    break;
                }
            m->lhost = ours;
            m->lport = ntohs(la.sin_port);
            wr32(p + TCPOpenPB_localHost_off, m->lhost);
            m->state = stListen;
        }
        struct sockaddr_in ra;
        socklen_t len = sizeof ra;
        int fd = accept(m->lfd, (struct sockaddr *)&ra, &len);
        if (fd < 0)
            return timed_out(o) ? commandTimeout : ioInProgress;
        close_fd(&m->lfd);
        m->fd = fd;
        set_nonblocking(m->fd);
        int one = 1;
        setsockopt(m->fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        m->rhost = ntohl(ra.sin_addr.s_addr);
        m->rport = ntohs(ra.sin_port);
    }
    struct sockaddr_in la;
    socklen_t len = sizeof la;
    getsockname(m->fd, (struct sockaddr *)&la, &len);
    m->lhost = connection_address(m->fd);
    m->lport = ntohs(la.sin_port);
    m->state = stEstablished;
    m->told_data = m->told_closing = m->told_term = m->eof = m->reset = false;
    wr32(p + TCPOpenPB_remoteHost_off, m->rhost);
    wr16(p + TCPOpenPB_remotePort_off, m->rport);
    wr32(p + TCPOpenPB_localHost_off, m->lhost);
    wr16(p + TCPOpenPB_localPort_off, m->lport);
    return noErr;
}

/* Bytes waiting on a connection; sets eof when the other end has closed. */
static int pending_bytes(mstream *m)
{
    if (m->fd < 0)
        return 0;
    int n = 0;
    ioctl(m->fd, FIONREAD, &n);
    if (n == 0 && !m->eof) {
        char b;
        ssize_t r = recv(m->fd, &b, 1, MSG_PEEK);
        if (r == 0)
            m->eof = true;
        else if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            m->eof = m->reset = true;
    }
    return n;
}

static int16_t step_receive(op *o, mstream *m)
{
    gaddr p = o->pb + CS;
    if (m->fd < 0)
        return connectionDoesntExist;
    if (pending_bytes(m) <= 0) {
        if (m->eof)
            return m->reset ? connectionTerminated : connectionClosing;
        return timed_out(o) ? commandTimeout : ioInProgress;
    }
    m->told_data = false; /* the next data says so again */
    if (o->code == TCPRcv) {
        gaddr buf = rd32(p + TCPReceivePB_rcvBuff_off);
        uint16_t want = rd16(p + TCPReceivePB_rcvBuffLen_off);
        uint8_t tmp[65536];
        ssize_t r = recv(m->fd, tmp, want, 0);
        if (r <= 0)
            return r == 0 ? connectionClosing : connectionTerminated;
        gput(buf, tmp, (uint32_t)r);
        wr16(p + TCPReceivePB_rcvBuffLen_off, (uint16_t)r);
        wr8(p + TCPReceivePB_markFlag_off, 0);
        wr8(p + TCPReceivePB_urgentFlag_off, 0);
        return noErr;
    }
    /* TCPNoCopyRcv: into the stream's own buffer, described by the RDS */
    gaddr rds = rd32(p + TCPReceivePB_rdsPtr_off);
    uint16_t entries = rd16(p + TCPReceivePB_rdsLength_off);
    if (!rds || entries < 1)
        return invalidRDS;
    uint32_t room = m->rcvlen > 65535 ? 65535 : m->rcvlen;
    uint8_t *tmp = malloc(room ? room : 1);
    ssize_t r = recv(m->fd, tmp, room, 0);
    if (r <= 0) {
        free(tmp);
        return r == 0 ? connectionClosing : connectionTerminated;
    }
    gput(m->rcvbuf, tmp, (uint32_t)r);
    free(tmp);
    wr16(rds, (uint16_t)r);
    wr32(rds + 2, m->rcvbuf);
    if (entries > 1)
        wr16(rds + 6, 0); /* the list's end */
    wr8(p + TCPReceivePB_markFlag_off, 0);
    wr8(p + TCPReceivePB_urgentFlag_off, 0);
    return noErr;
}

static int16_t step_send(op *o, mstream *m)
{
    gaddr p = o->pb + CS;
    if (m->fd < 0 || m->reset)
        return connectionDoesntExist;
    uint32_t total;
    uint8_t *buf = wds_gather(rd32(p + TCPSendPB_wdsPtr_off), &total);
    if (o->sent == 0)
        listen_early(buf, total);
    while (o->sent < total) {
        ssize_t r = send(m->fd, buf + o->sent, total - o->sent, 0);
        if (r > 0) {
            o->sent += (uint32_t)r;
            continue;
        }
        free(buf);
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return timed_out(o) ? commandTimeout : ioInProgress;
        return connectionTerminated;
    }
    free(buf);
    return noErr;
}

static int16_t step_udp_read(op *o, mstream *m)
{
    gaddr p = o->pb + CS;
    struct sockaddr_in from;
    socklen_t len = sizeof from;
    uint32_t room = m->rcvlen > 65535 ? 65535 : m->rcvlen;
    uint8_t *tmp = malloc(room ? room : 1);
    ssize_t r = recvfrom(m->fd, tmp, room, 0, (struct sockaddr *)&from, &len);
    if (r < 0) {
        free(tmp);
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return timed_out(o) ? commandTimeout : ioInProgress;
        return connectionTerminated;
    }
    m->told_data = false;
    gput(m->rcvbuf, tmp, (uint32_t)r);
    free(tmp);
    wr32(p + UDPR_REMOTEHOST, ntohl(from.sin_addr.s_addr));
    wr16(p + UDPR_REMOTEPORT, ntohs(from.sin_port));
    wr32(p + UDPR_RCVBUFF, m->rcvbuf);
    wr16(p + UDPR_RCVLEN, (uint16_t)r);
    wr32(p + UDPR_DESTHOST, m->lhost);
    wr16(p + UDPR_DESTPORT, m->lport);
    return noErr;
}

static int16_t step(op *o)
{
    mstream *m = &streams[o->s];
    switch (o->code) {
    case TCPActiveOpen:
    case TCPPassiveOpen: return step_open(o, m);
    case TCPRcv:
    case TCPNoCopyRcv: return step_receive(o, m);
    case TCPSend: return step_send(o, m);
    case UDPRead: return step_udp_read(o, m);
    default: return noErr;
    }
}

/* A call that may wait: run it now; if it has to wait, block (synchronous)
 * or leave it in progress for the polling (asynchronous). */
static int16_t run(gaddr pb, int code, int s, uint32_t timeout_s, bool async)
{
    op o = { true, pb, code, s, timeout_s ? now_ns() + (uint64_t)timeout_s * 1000000000u : 0, 0, ioInProgress };
    if (!async) {
        for (;;) {
            int16_t r = step(&o);
            if (r != ioInProgress)
                return r;
            mstream *m = &streams[s];
            int fd = m->fd >= 0 ? m->fd : m->lfd;
            struct pollfd pf = { fd, (short)(code == TCPSend || code == TCPActiveOpen ? POLLOUT : POLLIN), 0 };
            if (fd >= 0)
                poll(&pf, 1, 10);
            machine_pass_tick();
            if (g_cpu->halted || !streams[s].used)
                return connectionTerminated;
        }
    }
    /* Asynchronous: started now (the program's next calls see the socket),
     * completed at interrupt time even if it is already done. */
    o.result = step(&o);
    for (int i = 0; i < MAX_OPS; i++) {
        if (!ops[i].used) {
            ops[i] = o;
            active = true;
            wr16(pb + IOParam_ioResult_off, ioInProgress);
            return ioInProgress;
        }
    }
    return insufficientResources;
}

/* The calls of a stream that is going away end with this result. */
static void fail_ops(int s, int16_t result)
{
    for (int i = 0; i < MAX_OPS; i++)
        if (ops[i].used && ops[i].s == s) {
            ops[i].used = false;
            complete(ops[i].pb, result);
        }
}

/* ---------------------------------------------------------------------- */
/* The driver */

static int16_t new_stream(gaddr pb, bool udp)
{
    gaddr p = pb + CS;
    gaddr buf = rd32(p + (udp ? UDPC_RCVBUFF : TCPCreatePB_rcvBuff_off));
    uint32_t len = rd32(p + (udp ? UDPC_RCVLEN : TCPCreatePB_rcvBuffLen_off));
    if (!buf)
        return invalidBufPtr;
    if (len < 1024)
        return invalidLength;
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (streams[i].used)
            continue;
        mstream *m = &streams[i];
        memset(m, 0, sizeof *m);
        m->used = true;
        active = true;
        m->udp = udp;
        m->stream = buf;
        m->rcvbuf = buf;
        m->rcvlen = len;
        m->lhost = local_address(); /* until a connection says which */
        m->notify = rd32(p + (udp ? UDPC_NOTIFY : TCPCreatePB_notifyProc_off));
        m->userdata = rd32(p + (udp ? UDPC_USERDATA : TCPCreatePB_userDataPtr_off));
        m->fd = m->lfd = -1;
        m->state = stClosed;
        if (udp) {
            m->fd = socket(AF_INET, SOCK_DGRAM, 0);
            struct sockaddr_in la = inet(0, rd16(p + UDPC_LOCALPORT));
            if (m->fd < 0 || bind(m->fd, (struct sockaddr *)&la, sizeof la) < 0) {
                int16_t r = m->fd < 0 ? insufficientResources : errno_result(errno);
                close_fd(&m->fd);
                m->used = false;
                return r;
            }
            set_nonblocking(m->fd);
            socklen_t sl = sizeof la;
            getsockname(m->fd, (struct sockaddr *)&la, &sl);
            m->lhost = local_address();
            m->lport = ntohs(la.sin_port);
            wr16(p + UDPC_LOCALPORT, m->lport);
        }
        wr32(pb + PB_STREAM, buf);
        return noErr;
    }
    return insufficientResources;
}

static void release_stream(int s, gaddr pb)
{
    mstream *m = &streams[s];
    fail_ops(s, connectionTerminated);
    close_fd(&m->fd);
    close_fd(&m->lfd);
    gaddr p = pb + CS;
    wr32(p + (m->udp ? UDPC_RCVBUFF : TCPCreatePB_rcvBuff_off), m->rcvbuf);
    wr32(p + (m->udp ? UDPC_RCVLEN : TCPCreatePB_rcvBuffLen_off), m->rcvlen);
    m->used = false;
}

static int16_t tcp_status(gaddr pb, mstream *m)
{
    /* Only the fields every MacTCP.h had, up to amtUnreadData: programs
     * built with older ones pass shorter parameter blocks (NCSA Mosaic). */
    gaddr p = pb + CS;
    int unread = pending_bytes(m);
    int state = m->state;
    if (state == stEstablished && m->eof)
        state = stCloseWait;
    wr32(p + TCPStatusPB_remoteHost_off, m->rhost);
    wr16(p + TCPStatusPB_remotePort_off, m->rport);
    wr32(p + TCPStatusPB_localHost_off, m->lhost);
    wr16(p + TCPStatusPB_localPort_off, m->lport);
    wr8(p + TCPStatusPB_connectionState_off, (uint8_t)state);
    wr16(p + TCPStatusPB_sendWindow_off, 8192);
    wr16(p + TCPStatusPB_rcvWindow_off, (uint16_t)(m->rcvlen > 65535 ? 65535 : m->rcvlen));
    wr16(p + TCPStatusPB_amtUnackedData_off, 0);
    wr16(p + TCPStatusPB_amtUnreadData_off, (uint16_t)(unread > 65535 ? 65535 : unread));
    return noErr;
}

static int16_t ipp_control(gaddr pb, bool async)
{
    int code = (int16_t)rd16(pb + CntrlParam_csCode_off);
    gaddr p = pb + CS;
    if (code == ipctlGetAddr) {
        wr32(pb + GetAddrParamBlock_ourAddress_off, local_address());
        wr32(pb + GetAddrParamBlock_ourNetMask_off, 0xFFFFFF00);
        return noErr;
    }
    if (code == TCPCreate || code == UDPCreate)
        return new_stream(pb, code == UDPCreate);
    if (code == UDPMaxMTUSize) {
        wr16(p, 1472);
        return noErr;
    }
    if (code == TCPGlobalInfo) {
        wr16(p + TCPG_MAXCONN, MAX_STREAMS);
        return noErr;
    }
    int s = stream_index(rd32(pb + PB_STREAM));
    if (s < 0)
        return invalidStreamPtr;
    mstream *m = &streams[s];
    switch (code) {
    case TCPPassiveOpen:
    case TCPActiveOpen:
        if (m->fd >= 0 || m->lfd >= 0)
            return connectionExists;
        return run(pb, code, s, rd8(p + TCPOpenPB_commandTimeoutValue_off), async);
    case TCPSend: return run(pb, code, s, 0, async);
    case TCPRcv:
    case TCPNoCopyRcv: return run(pb, code, s, rd8(p + TCPReceivePB_commandTimeoutValue_off), async);
    case TCPRcvBfrReturn:
    case UDPBfrReturn: return noErr;
    case TCPClose:
        if (m->fd < 0)
            return connectionDoesntExist;
        shutdown(m->fd, SHUT_WR);
        m->state = m->eof ? stClosed : stFinWait1;
        return noErr;
    case TCPAbort:
        fail_ops(s, connectionTerminated);
        close_fd(&m->fd);
        close_fd(&m->lfd);
        m->state = stClosed;
        return noErr;
    case TCPStatus: return tcp_status(pb, m);
    case TCPRelease:
    case UDPRelease: release_stream(s, pb); return noErr;
    case UDPRead: return run(pb, code, s, rd16(p + UDPR_TIMEOUT), async);
    case UDPWrite: {
        uint32_t total;
        uint8_t *buf = wds_gather(rd32(p + UDPS_WDS), &total);
        struct sockaddr_in to = inet(rd32(p + UDPS_REMOTEHOST), rd16(p + UDPS_REMOTEPORT));
        ssize_t r = sendto(m->fd, buf, total, 0, (struct sockaddr *)&to, sizeof to);
        free(buf);
        return r < 0 ? errno_result(errno) : noErr;
    }
    default: return -17; /* controlErr */
    }
}

static int16_t ipp_open(gaddr pb) { return noErr; }
static int16_t ipp_close(gaddr pb) { return noErr; }
static int16_t ipp_kill(gaddr pb) { return noErr; }

const native_driver mactcp_driver = { ".IPP", 48, ipp_open, ipp_close, ipp_control, NULL, NULL, NULL, ipp_kill };

/* ---------------------------------------------------------------------- */
/* Interrupt time: waiting calls go on; notification routines hear of data
 * and of the other end closing. */

/* pascal void ResultProc(struct hostInfo *, char *userDataPtr) */
static void call_result_procs(void)
{
    int n = nanswers;
    nanswers = 0;
    for (int i = 0; i < n; i++) {
        cpu68k *c = g_cpu;
        uint32_t d[8], a[8];
        memcpy(d, c->d, sizeof d);
        memcpy(a, c->a, sizeof a);
        uint16_t sr = c->sr;
        wr32(answers[i].info, (uint32_t)(int32_t)answers[i].err); /* rtnCode: no longer cacheFault */
        cpu_push32(c, answers[i].info);
        cpu_push32(c, answers[i].userdata);
        guest_call_regs(answers[i].proc);
        memcpy(c->d, d, sizeof d);
        memcpy(c->a, a, sizeof a);
        c->sr = sr;
    }
}

void mactcp_poll(void)
{
    if (polling || !g_cpu || (!active && !nanswers))
        return;
    polling = true;
    expire_early(false);
    call_result_procs();
    for (int i = 0; i < MAX_OPS; i++) {
        if (!ops[i].used)
            continue;
        int16_t r = ops[i].result != ioInProgress ? ops[i].result : step(&ops[i]);
        if (r == ioInProgress)
            continue;
        ops[i].used = false;
        LOGF(LOG_TRAPS, "  .IPP csCode %d completes -> %d\n", ops[i].code, r);
        complete(ops[i].pb, r);
    }
    for (int s = 0; s < MAX_STREAMS; s++) {
        mstream *m = &streams[s];
        if (!m->used || m->fd < 0 || !m->notify)
            continue;
        bool reading = false;
        for (int i = 0; i < MAX_OPS && !reading; i++)
            reading = ops[i].used && ops[i].s == s && (ops[i].code == TCPRcv || ops[i].code == TCPNoCopyRcv ||
                                                       ops[i].code == UDPRead);
        if (m->udp) {
            struct pollfd pf = { m->fd, POLLIN, 0 };
            if (!reading && !m->told_data && poll(&pf, 1, 0) > 0) {
                m->told_data = true;
                notify(m, UDPDataArrival, 0);
            }
            continue;
        }
        if (m->state != stEstablished && m->state != stFinWait1)
            continue;
        int n = pending_bytes(m);
        if (n > 0 && !reading && !m->told_data) {
            m->told_data = true;
            notify(m, TCPDataArrival, 0);
        } else if (m->eof && n == 0 && !m->told_closing) {
            m->told_closing = true;
            notify(m, m->reset ? TCPTerminate : TCPClosing, m->reset ? TCPRemoteAbort : 0);
        }
    }
    bool any = false;
    for (int i = 0; i < MAX_OPS && !any; i++)
        any = ops[i].used;
    for (int i = 0; i < MAX_STREAMS && !any; i++)
        any = streams[i].used;
    for (int i = 0; i < MAX_EARLY && !any; i++)
        any = early[i].used;
    active = any;
    polling = false;
}

static void dnr_native(cpu68k *c, uint16_t tw);
static gaddr stub; /* the resolver's entry */

void mactcp_reset(void)
{
    static bool booted; /* before the first boot the table's fds are 0, not ours */
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (booted) {
            close_fd(&streams[i].fd);
            close_fd(&streams[i].lfd);
        }
        streams[i].fd = streams[i].lfd = -1;
        streams[i].used = false;
    }
    expire_early(true);
    booted = true;
    memset(ops, 0, sizeof ops);
    nanswers = 0;
    polling = active = false;
    stub = native_register(dnr_native, "DNR", 0); /* natives start over at each boot */
    gestalt_set(fourcc("mtcp"), 0x02068000); /* MacTCP 2.0.6, as NumVersion */
}

/* ---------------------------------------------------------------------- */
/* The Domain Name Resolver.  dnr.c finds a file of type 'cdev' and creator
 * 'ztcp' in the System Folder or the Control Panels folder, loads its
 * 'dnrp' resource and calls it as C: (long selector, ...), the result in
 * D0.  Ours is a jump to native code; the file is written at each launch,
 * since the native routine's address can change from one build to the
 * next. */

enum { OPENRESOLVER = 1, CLOSERESOLVER, STRTOADDR, ADDRTOSTR, ENUMCACHE, ADDRTONAME, HXINFO, MXINFO };
enum { HOSTINFO_CNAME = 4, HOSTINFO_ADDR = 260 };

static void c_string(gaddr p, char *out, size_t cap)
{
    size_t n = 0;
    while (p && n + 1 < cap) {
        char ch = (char)rd8(p + (uint32_t)n);
        if (!ch)
            break;
        out[n++] = ch;
    }
    out[n] = 0;
}

static int16_t resolve_name(const char *name, gaddr info)
{
    gmemset(info, 0, 276);
    /* A dotted-decimal address is its own answer, with no name: programs'
     * inet_addr (NCSA Mosaic's) takes an empty name to mean the string was
     * an address. */
    struct in_addr literal;
    if (inet_pton(AF_INET, name, &literal) == 1) {
        wr32(info + HOSTINFO_ADDR, ntohl(literal.s_addr));
        wr32(info, noErr);
        return noErr;
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_flags = AI_CANONNAME;
    int16_t err = noErr;
    if (getaddrinfo(name, NULL, &hints, &res) != 0 || !res) {
        err = noAnsErr;
    } else {
        int k = 0;
        for (struct addrinfo *a = res; a && k < 4; a = a->ai_next) {
            uint32_t ip = ntohl(((struct sockaddr_in *)a->ai_addr)->sin_addr.s_addr);
            bool dup = false;
            for (int j = 0; j < k; j++)
                dup |= rd32(info + HOSTINFO_ADDR + (uint32_t)j * 4) == ip;
            if (!dup)
                wr32(info + HOSTINFO_ADDR + (uint32_t)k++ * 4, ip);
        }
        const char *cname = res->ai_canonname ? res->ai_canonname : name;
        gput(info + HOSTINFO_CNAME, cname, (uint32_t)strnlen(cname, 254));
        freeaddrinfo(res);
    }
    wr32(info, (uint32_t)(int32_t)err); /* rtnCode */
    return err;
}

/* The answer is in the hostInfo now; as MacTCP's resolver did for names
 * not in its cache, the call returns cacheFault and the result procedure
 * hears of it at interrupt time (programs wait for that, NCSA Mosaic among
 * them).  Without a result procedure, the answer is the call's result. */
static int16_t later(gaddr proc, gaddr info, gaddr userdata, int16_t err)
{
    if (!proc || nanswers >= (int)(sizeof answers / sizeof answers[0]))
        return err;
    answers[nanswers].proc = proc;
    answers[nanswers].info = info;
    answers[nanswers].userdata = userdata;
    answers[nanswers].err = err;
    nanswers++;
    wr32(info, (uint32_t)(int32_t)cacheFault); /* until the result procedure hears */
    return cacheFault;
}

static void dnr_native(cpu68k *c, uint16_t tw)
{
    gaddr sp = c->a[7] + 4; /* past the return address */
    int32_t sel = (int32_t)rd32(sp);
    int16_t err = noErr;
    char name[256];
    switch (sel) {
    case OPENRESOLVER:
    case CLOSERESOLVER: break;
    case STRTOADDR: /* (hostName, hostInfo *, resultProc, userData) */
        c_string(rd32(sp + 4), name, sizeof name);
        err = resolve_name(name, rd32(sp + 8));
        err = later(rd32(sp + 12), rd32(sp + 8), rd32(sp + 16), err);
        break;
    case ADDRTOSTR: { /* (unsigned long addr, char *addrStr) */
        uint32_t a = rd32(sp + 4);
        char s[20];
        snprintf(s, sizeof s, "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
        gput(rd32(sp + 8), s, (uint32_t)strlen(s) + 1);
        break;
    }
    case ADDRTONAME: { /* (addr, hostInfo *, resultProc, userData) */
        gaddr info = rd32(sp + 8);
        gmemset(info, 0, 276);
        struct sockaddr_in sa = inet(rd32(sp + 4), 0);
        char host[NI_MAXHOST];
        if (getnameinfo((struct sockaddr *)&sa, sizeof sa, host, sizeof host, NULL, 0, NI_NAMEREQD) == 0) {
            gput(info + HOSTINFO_CNAME, host, (uint32_t)strnlen(host, 254));
            wr32(info + HOSTINFO_ADDR, rd32(sp + 4));
        } else {
            err = noAnsErr;
        }
        wr32(info, (uint32_t)(int32_t)err);
        err = later(rd32(sp + 12), info, rd32(sp + 16), err);
        break;
    }
    default: err = dnrErr; /* the cache, HINFO and MX records: none */
    }
    c->d[0] = (uint32_t)(int32_t)err;
}

void mactcp_install_resolver(void)
{
    uint8_t code[6] = { 0x4E, 0xF9, (uint8_t)(stub >> 24), (uint8_t)(stub >> 16), (uint8_t)(stub >> 8), (uint8_t)stub };
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/Control Panels", fm_system_folder_host());
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/Control Panels/MacTCP", fm_system_folder_host());
    /* Already written for this build? */
    mac_file old;
    char err[256];
    if (appledouble_read(path, &old, err, sizeof err)) {
        rsrc_index idx;
        bool same = false;
        if (old.rsrc_len && rsrc_parse(old.rsrc, old.rsrc_len, &idx, err, sizeof err)) {
            for (size_t i = 0; i < idx.count; i++)
                if (idx.entries[i].type == 0x646E7270 /* dnrp */ && idx.entries[i].data_len == sizeof code &&
                    !memcmp(rsrc_data(old.rsrc, &idx, &idx.entries[i]), code, sizeof code))
                    same = true;
            rsrc_index_free(&idx);
        }
        mac_file_free(&old);
        if (same)
            return;
    }
    rsrc_spec spec = { 0x646E7270, 128, "", 0, code, sizeof code };
    uint8_t *fork;
    size_t len;
    if (!rsrc_build(&spec, 1, &fork, &len))
        return;
    mac_file mf;
    memset(&mf, 0, sizeof mf);
    mf.data = malloc(1);
    mf.rsrc = fork;
    mf.rsrc_len = len;
    memcpy(mf.finder_info, "cdevztcp", 8);
    mf.has_finder_info = true;
    strcpy(mf.name, "MacTCP");
    appledouble_write(path, &mf, err, sizeof err);
    mac_file_free(&mf);
}
