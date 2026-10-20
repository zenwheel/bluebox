/*
 * The Connection Manager, with three tools of our own:
 *
 *   Serial Tool       a serial port (serial.c) as it is: whatever the port
 *                     is connected to, a Hayes modem the program dials
 *                     itself included
 *   Apple Modem Tool  the modem port's modem, dialled (ATDT PhoneNumber)
 *                     when the connection opens, answered when it listens
 *   TCP Tool          a TCP connection to Host and Port, Telnet's options
 *                     refused (but echo and go-ahead suppression) when
 *                     Telnet is on; listening on LocalPort
 *
 * Each connection keeps what has come in until the program reads it, and
 * the strings CMAddSearch asked for are looked for in what it reads.
 * Asynchronous calls finish at interrupt time (ctb_poll), their completion
 * routines called then with the record's errCode set.
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
#include <sys/socket.h>
#include <unistd.h>

#include "ctb.h"

enum { cmNoErr = 0, cmRejected = 1, cmFailed = 2, cmTimeOut = 3, cmNotOpen = 4, cmNotClosed = 5,
       cmNoRequestPending = 6, cmNotSupported = 7, cmUserCancel = 9 };
enum { cmData = 1 << 0 };
enum { cmStatusOpening = 1 << 0, cmStatusOpen = 1 << 1, cmStatusClosing = 1 << 2, cmStatusDataAvail = 1 << 3,
       cmStatusDRPend = 1 << 6, cmStatusListenPend = 1 << 13, cmStatusIncomingCallPresent = 1 << 14 };
enum { cmDataIn = 0, cmDataOut = 1 };
enum { cmSearchSevenBit = 1 };
enum { T_SERIAL = 1, T_MODEM, T_TCP };
enum { ST_CLOSED, ST_OPENING, ST_OPEN, ST_LISTENING, ST_INCOMING };
enum { MAX_CONNS = 8, INBUF = 16384, MAX_READS = 8, MAX_DONE = 16, MAX_SEARCH = 16, SYNC_CAP_MS = 60000 };
/* Telnet */
enum { IAC = 255, DONT = 254, DO = 253, WONT = 252, WILL = 251, SB = 250, BRK = 243, SE = 240 };
enum { TN_DATA, TN_IAC, TN_OPT, TN_SB, TN_SB_IAC };
/* Completion routines: (hConn) */
#define PI_COMPLETION 0x000000C0u
/* Search callbacks: (hConn, matchPtr, refNum) */
#define PI_SEARCH 0x00000FC0u

/* ---------------------------------------------------------------------- */
/* The tools' configurations */

static const char *const parities[] = { "None", "Odd", "Even", NULL };
static const char *const stops[] = { "1", "1.5", "2", NULL };
static const char *const shakes[] = { "None", "XOnXOff", "CTS", "DTR", NULL };
static const char *const dials[] = { "Tone", "Pulse", NULL };

/* Serial and modem: the port's settings */
enum { C_BAUD = 4, C_BITS = 8, C_PARITY = 12, C_STOP = 14, C_SHAKE = 16, C_HOLD = 18, C_REMIND = 19, C_PORT = 20,
       C_PHONE = 84, C_DIAL = 148, SERIAL_SIZE = 84, MODEM_SIZE = 150 };
/* TCP */
enum { C_HOST = 4, C_TCPPORT = 260, C_TELNET = 264, C_LOCALPORT = 268, TCP_SIZE = 272 };

#define PORT_FIELDS \
    { "Baud", CF_INT, C_BAUD, 50, 230400, NULL, "9600" }, \
    { "DataBits", CF_INT, C_BITS, 5, 8, NULL, "8" }, \
    { "Parity", CF_ENUM, C_PARITY, 0, 0, parities, "None" }, \
    { "StopBits", CF_ENUM, C_STOP, 0, 0, stops, "1" }, \
    { "Port", CF_STR, C_PORT, 0, 63, NULL, "Modem Port" }, \
    { "Handshake", CF_ENUM, C_SHAKE, 0, 0, shakes, "None" }

static const ctb_field serial_fields[] = {
    PORT_FIELDS,
    { "HoldConnection", CF_BOOL, C_HOLD, 0, 0, NULL, "False" },
    { "RemindDisconnect", CF_BOOL, C_REMIND, 0, 0, NULL, "False" },
};
static const ctb_field modem_fields[] = {
    { "PhoneNumber", CF_STR, C_PHONE, 0, 63, NULL, "" },
    { "DialType", CF_ENUM, C_DIAL, 0, 0, dials, "Tone" },
    PORT_FIELDS,
};
static const ctb_field tcp_fields[] = {
    { "Host", CF_STR, C_HOST, 0, 255, NULL, "" },
    { "Port", CF_INT, C_TCPPORT, 1, 65535, NULL, "23" },
    { "Telnet", CF_BOOL, C_TELNET, 0, 0, NULL, "True" },
    { "LocalPort", CF_INT, C_LOCALPORT, 0, 65535, NULL, "0" },
};

#define COUNT(a) (int)(sizeof a / sizeof a[0])
static const ctb_tool serial_tool = { "Serial Tool", 0x73657274 /* 'sert' */, SERIAL_SIZE, serial_fields,
                                      COUNT(serial_fields) };
static const ctb_tool modem_tool = { "Apple Modem Tool", 0x6D6F646D /* 'modm' */, MODEM_SIZE, modem_fields,
                                     COUNT(modem_fields) };
static const ctb_tool tcp_tool = { "TCP Tool", 0x74637074 /* 'tcpt' */, TCP_SIZE, tcp_fields, COUNT(tcp_fields) };
static const ctb_tool *const cm_tools[] = { &serial_tool, &modem_tool, &tcp_tool };

/* ---------------------------------------------------------------------- */
/* Connections */

typedef struct {
    gaddr buf, count_ptr, flags_ptr, completor, iopb;
    uint32_t want, have;
    uint64_t until;
    bool async;
} cm_read_req;

typedef struct {
    gaddr proc, iopb;
    int16_t err;
    int kind; /* 0 open/listen/close/break, 1 read, 2 write */
    uint32_t count;
} cm_done;

typedef struct {
    int32_t id;
    uint8_t s[256];
    int len;
    bool seven;
    gaddr callback;
} cm_search;

typedef struct {
    bool used;
    gaddr h;
    int tool;
    int state;
    int port;
    int fd, lfd;
    bool telnet;
    int tn, tn_cmd;
    /* the modem's replies while dialling or answering */
    char line[128];
    int linelen;
    bool answering;
    /* an open or listen waiting */
    gaddr open_completor;
    bool open_async;
    uint64_t open_until;
    uint8_t in[INBUF];
    int inlen;
    cm_read_req reads[MAX_READS];
    int nreads;
    cm_done done[MAX_DONE];
    int ndone;
    cm_search search[MAX_SEARCH];
    int nsearch;
    int32_t next_search;
    uint8_t recent[256]; /* the last bytes read, for searches */
    int nrecent;
} conn;

static conn conns[MAX_CONNS];

static gaddr rec(conn *c) { return rd32(c->h); }
static gaddr cfg(conn *c) { return rd32(rec(c) + ConnRecord_config_off); }

static conn *conn_of(gaddr h)
{
    if (!h || !rd32(h))
        return NULL;
    uint32_t i = rd32(rd32(h) + ConnRecord_cmPrivate_off);
    if (i < 1 || i > MAX_CONNS || !conns[i - 1].used || conns[i - 1].h != h)
        return NULL;
    return &conns[i - 1];
}

static void close_fd(int *fd)
{
    if (*fd >= 0)
        close(*fd);
    *fd = -1;
}

static int port_number(conn *c)
{
    char name[64];
    ctb_config_str(cfg(c), C_PORT, name, sizeof name);
    return !strcasecmp(name, "Printer Port") || !strcasecmp(name, "B") ? 1 : 0;
}

static void set_err(conn *c, int16_t e) { wr16(rec(c) + ConnRecord_errCode_off, (uint16_t)e); }

/* A completion routine called at the next poll. */
static void complete_later(conn *c, gaddr proc, gaddr iopb, int16_t err, int kind, uint32_t count)
{
    if (c->ndone < MAX_DONE)
        c->done[c->ndone++] = (cm_done){ proc, iopb, err, kind, count };
}

/* ---------------------------------------------------------------------- */
/* Moving bytes */

static void to_app(conn *c, const uint8_t *b, int n)
{
    if (n > INBUF - c->inlen)
        n = INBUF - c->inlen;
    memcpy(c->in + c->inlen, b, (size_t)n);
    c->inlen += n;
}

static void tcp_send(conn *c, const uint8_t *b, size_t n)
{
    while (n > 0 && c->fd >= 0) {
        ssize_t w = send(c->fd, b, n, 0);
        if (w < 0 && (errno == EAGAIN || errno == EINTR)) {
            struct pollfd pf = { c->fd, POLLOUT, 0 };
            poll(&pf, 1, 100);
            continue;
        }
        if (w <= 0)
            return;
        b += w;
        n -= (size_t)w;
    }
}

/* Telnet: options refused but echo and go-ahead suppression. */
static void telnet_in(conn *c, const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) {
        uint8_t ch = b[i];
        switch (c->tn) {
        case TN_DATA:
            if (ch == IAC)
                c->tn = TN_IAC;
            else
                to_app(c, &ch, 1);
            break;
        case TN_IAC:
            if (ch == IAC) {
                to_app(c, &ch, 1);
                c->tn = TN_DATA;
            } else if (ch >= WILL && ch <= DONT) {
                c->tn_cmd = ch;
                c->tn = TN_OPT;
            } else {
                c->tn = ch == SB ? TN_SB : TN_DATA;
            }
            break;
        case TN_OPT: {
            uint8_t reply[3] = { IAC, 0, ch };
            bool ok = ch == 1 || ch == 3; /* ECHO, SUPPRESS-GO-AHEAD */
            if (c->tn_cmd == WILL)
                reply[1] = ok ? DO : DONT;
            else if (c->tn_cmd == DO)
                reply[1] = ch == 3 ? WILL : WONT;
            if (reply[1])
                tcp_send(c, reply, 3);
            c->tn = TN_DATA;
            break;
        }
        case TN_SB: c->tn = ch == IAC ? TN_SB_IAC : TN_SB; break;
        case TN_SB_IAC: c->tn = ch == SE ? TN_DATA : TN_SB; break;
        }
    }
}

static void lost(conn *c)
{
    close_fd(&c->fd);
    if (c->state == ST_OPEN)
        c->state = ST_CLOSED;
}

/* What has come in, into the connection's buffer. */
static void pump_in(conn *c)
{
    uint8_t buf[4096];
    int room = INBUF - c->inlen;
    if (c->tool == T_TCP) {
        if (c->fd < 0 || c->state == ST_OPENING)
            return;
        while (room > 0) {
            ssize_t r = recv(c->fd, buf, (size_t)(room < (int)sizeof buf ? room : (int)sizeof buf), 0);
            if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
                lost(c);
                return;
            }
            if (r < 0)
                return;
            if (c->telnet)
                telnet_in(c, buf, (int)r);
            else
                to_app(c, buf, (int)r);
            room = INBUF - c->inlen;
        }
        return;
    }
    if (!serial_port_in_use(c->port))
        return;
    while (room > 0) {
        int r = serial_port_read(c->port, buf, room < (int)sizeof buf ? room : (int)sizeof buf);
        if (r <= 0)
            break;
        to_app(c, buf, r);
        room = INBUF - c->inlen;
    }
    if (c->tool == T_MODEM && c->state == ST_OPEN && !serial_port_online(c->port))
        c->state = ST_CLOSED; /* the carrier went */
}

static void send_bytes(conn *c, const uint8_t *b, uint32_t n)
{
    if (c->tool != T_TCP) {
        serial_port_write(c->port, b, (int)n);
        return;
    }
    if (!c->telnet) {
        tcp_send(c, b, n);
        return;
    }
    for (uint32_t i = 0; i < n; i++) { /* IAC doubled */
        tcp_send(c, &b[i], 1);
        if (b[i] == IAC)
            tcp_send(c, &b[i], 1);
    }
}

/* The bytes a program reads, watched for the strings it searches for. */
static void searched(conn *c, gaddr at, uint32_t n)
{
    for (uint32_t i = 0; i < n && c->nsearch; i++) {
        uint8_t b = rd8(at + i);
        if (c->nrecent == (int)sizeof c->recent) {
            memmove(c->recent, c->recent + 1, sizeof c->recent - 1);
            c->nrecent--;
        }
        c->recent[c->nrecent++] = b;
        for (int k = 0; k < c->nsearch; k++) {
            cm_search *s = &c->search[k];
            if (!s->len || s->len > c->nrecent)
                continue;
            const uint8_t *tail = c->recent + c->nrecent - s->len;
            bool hit = true;
            for (int j = 0; j < s->len && hit; j++)
                hit = s->seven ? ((tail[j] ^ s->s[j]) & 0x7F) == 0 : tail[j] == s->s[j];
            if (hit) {
                uint32_t args[3] = { c->h, at + i, (uint32_t)s->id };
                ctb_callback(s->callback, PI_SEARCH, args, 3);
            }
        }
    }
}

/* Up to want bytes of what has come in into a program's buffer. */
static uint32_t take(conn *c, gaddr buf, uint32_t want)
{
    uint32_t n = want < (uint32_t)c->inlen ? want : (uint32_t)c->inlen;
    if (!n)
        return 0;
    gput(buf, c->in, n);
    memmove(c->in, c->in + n, (size_t)c->inlen - n);
    c->inlen -= (int)n;
    searched(c, buf, n);
    return n;
}

/* ---------------------------------------------------------------------- */
/* Opening and closing */

static int tcp_connect(const char *host, int port)
{
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res)
        return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        if (connect(fd, res->ai_addr, res->ai_addrlen) < 0 && errno != EINPROGRESS)
            close_fd(&fd);
    }
    freeaddrinfo(res);
    return fd;
}

static int tcp_listen(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

/* The port's settings from the configuration. */
static void setup_port(conn *c)
{
    gaddr k = cfg(c);
    static const int stop_bits[3] = { 1, 15, 2 };
    serial_port_setup(c->port, rd32(k + C_BAUD), (int)rd32(k + C_BITS), rd16(k + C_PARITY),
                      stop_bits[rd16(k + C_STOP) % 3], rd16(k + C_SHAKE));
}

/* The open (or listen) is over: the program told, if it asked to be. */
static void opened(conn *c, int16_t err)
{
    set_err(c, err);
    if (c->open_async)
        complete_later(c, c->open_completor, 0, err, 0, 0);
    c->open_async = false;
    c->open_completor = 0;
}

static void modem_command(conn *c, const char *s)
{
    serial_port_write(c->port, (const uint8_t *)s, (int)strlen(s));
}

/* The modem's replies while dialling or answering: lines of text. */
static void modem_replies(conn *c)
{
    while (c->inlen > 0 && (c->state == ST_OPENING || c->state == ST_LISTENING)) {
        uint8_t b = c->in[0];
        memmove(c->in, c->in + 1, (size_t)--c->inlen);
        if (b != '\r' && b != '\n') {
            if (c->linelen < (int)sizeof c->line - 1)
                c->line[c->linelen++] = (char)b;
            continue;
        }
        c->line[c->linelen] = 0;
        c->linelen = 0;
        const char *l = c->line;
        if (!strncmp(l, "CONNECT", 7)) {
            if (b == '\r' && c->inlen && c->in[0] == '\n') /* the rest of the reply's line */
                memmove(c->in, c->in + 1, (size_t)--c->inlen);
            c->state = ST_OPEN;
            opened(c, cmNoErr);
        } else if (!strcmp(l, "RING") && c->state == ST_LISTENING && !c->answering) {
            c->state = ST_INCOMING; /* CMAccept answers it */
            opened(c, cmNoErr);
        } else if (!strcmp(l, "NO CARRIER") || !strcmp(l, "BUSY") || !strcmp(l, "NO DIALTONE") ||
                   !strcmp(l, "NO ANSWER") || !strcmp(l, "ERROR")) {
            c->state = ST_CLOSED;
            opened(c, cmFailed);
        }
    }
}

/* Moves an opening or listening connection on. */
static void progress(conn *c)
{
    uint64_t now = ctb_now_ms();
    if (c->state == ST_OPENING && c->tool == T_TCP) {
        struct pollfd pf = { c->fd, POLLOUT, 0 };
        if (poll(&pf, 1, 0) > 0) {
            int e = 0;
            socklen_t len = sizeof e;
            getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &e, &len);
            if (e) {
                close_fd(&c->fd);
                c->state = ST_CLOSED;
                opened(c, cmFailed);
            } else {
                c->state = ST_OPEN;
                c->tn = TN_DATA;
                opened(c, cmNoErr);
            }
        }
    } else if (c->state == ST_LISTENING && c->tool == T_TCP && c->lfd >= 0) {
        int fd = accept(c->lfd, NULL, NULL);
        if (fd >= 0) {
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            c->fd = fd;
            close_fd(&c->lfd);
            c->state = ST_INCOMING;
            opened(c, cmNoErr);
        }
    } else if ((c->state == ST_OPENING || c->state == ST_LISTENING) && c->tool == T_MODEM) {
        pump_in(c);
        modem_replies(c);
    }
    if ((c->state == ST_OPENING || c->state == ST_LISTENING) && now >= c->open_until) {
        if (c->tool == T_MODEM)
            serial_port_dtr(c->port, false);
        close_fd(&c->fd);
        close_fd(&c->lfd);
        c->state = ST_CLOSED;
        opened(c, cmTimeOut);
    }
}

/* Waits (a synchronous call) until done says so or the time is up. */
static void wait_for(conn *c, bool (*done)(conn *), uint64_t until)
{
    uint64_t cap = ctb_now_ms() + SYNC_CAP_MS;
    if (until > cap)
        until = cap;
    for (;;) {
        progress(c);
        if (c->state != ST_OPENING && c->state != ST_LISTENING)
            pump_in(c);
        if (done(c) || ctb_now_ms() >= until)
            return;
        int fd = c->tool == T_TCP ? (c->fd >= 0 ? c->fd : c->lfd) : serial_port_fd(c->port);
        struct pollfd pf = { fd, POLLIN, 0 };
        poll(&pf, fd >= 0 ? 1 : 0, 10);
    }
}

static bool settled(conn *c) { return c->state != ST_OPENING && c->state != ST_LISTENING; }

static void shut(conn *c)
{
    if (c->tool == T_TCP) {
        close_fd(&c->fd);
        close_fd(&c->lfd);
    } else if (c->state != ST_CLOSED || serial_port_in_use(c->port)) {
        bool hold = c->tool == T_SERIAL && rd8(cfg(c) + C_HOLD);
        if (c->tool == T_MODEM)
            serial_port_dtr(c->port, false);
        if (!hold)
            serial_port_close(c->port);
    }
    c->state = ST_CLOSED;
    /* Reads waiting end now. */
    for (int i = 0; i < c->nreads; i++) {
        cm_read_req *r = &c->reads[i];
        complete_later(c, r->completor, r->iopb, cmNotOpen, 1, r->have);
    }
    c->nreads = 0;
}

/* ---------------------------------------------------------------------- */
/* Reads waiting */

static void finish_read(conn *c, int16_t err)
{
    cm_read_req r = c->reads[0];
    memmove(&c->reads[0], &c->reads[1], sizeof c->reads[0] * (size_t)--c->nreads);
    complete_later(c, r.completor, r.iopb, err, 1, r.have);
}

static void serve_reads(conn *c)
{
    while (c->nreads > 0) {
        cm_read_req *r = &c->reads[0];
        r->have += take(c, r->buf + r->have, r->want - r->have);
        if (r->have >= r->want)
            finish_read(c, cmNoErr);
        else if (ctb_now_ms() >= r->until)
            finish_read(c, cmTimeOut);
        else if (c->state != ST_OPEN && !c->inlen && r->have == 0)
            finish_read(c, cmNotOpen);
        else
            break;
    }
}

static void call_done(conn *c)
{
    while (c->ndone > 0 && c->used) {
        cm_done d = c->done[0];
        memmove(&c->done[0], &c->done[1], sizeof c->done[0] * (size_t)--c->ndone);
        gaddr r = rec(c);
        wr16(r + ConnRecord_errCode_off, (uint16_t)d.err);
        if (d.kind == 1)
            wr32(r + ConnRecord_asyncCount_off + 4 * cmDataIn, d.count);
        else if (d.kind == 2)
            wr32(r + ConnRecord_asyncCount_off + 4 * cmDataOut, d.count);
        if (d.iopb) {
            wr16(d.iopb + CMIOPB_errCode_off, (uint16_t)d.err);
            wr32(d.iopb + CMIOPB_count_off, d.count);
        }
        uint32_t args[1] = { c->h };
        ctb_callback(d.proc, PI_COMPLETION, args, 1);
    }
}

static void poll_conn(conn *c)
{
    progress(c);
    if (c->state != ST_OPENING && c->state != ST_LISTENING)
        pump_in(c);
    serve_reads(c);
    call_done(c);
}

void cm_poll(void)
{
    for (int i = 0; i < MAX_CONNS; i++)
        if (conns[i].used)
            poll_conn(&conns[i]);
}

/* ---------------------------------------------------------------------- */
/* The manager's calls */

static void cm_config_changed(gaddr h)
{
    conn *c = conn_of(h);
    if (!c || c->tool == T_TCP)
        return;
    int port = port_number(c);
    if (port != c->port && c->state == ST_CLOSED)
        c->port = port;
    if (c->state == ST_OPEN)
        setup_port(c);
}

uint32_t cm_init(uint16_t sel, const uint32_t *a) { return cmNoErr; }

/* (procID, flags, desiredSizes, refCon, userData) */
uint32_t cm_new(uint16_t sel, const uint32_t *a)
{
    int16_t id = (int16_t)a[0];
    if (!ctb_tool_of(MGR_CM, id))
        return 0;
    int slot = -1;
    for (int i = 0; i < MAX_CONNS && slot < 0; i++)
        if (!conns[i].used)
            slot = i;
    if (slot < 0)
        return 0;
    gaddr h = tb_NewHandle(ConnRecord_SIZEOF, false, true);
    if (!h)
        return 0;
    ctb_record_init(h, MGR_CM, id, a[1], (int32_t)a[3], (int32_t)a[4]);
    conn *c = &conns[slot];
    memset(c, 0, sizeof *c);
    c->used = true;
    c->h = h;
    c->tool = id;
    c->fd = c->lfd = -1;
    c->port = port_number(c);
    c->next_search = 1;
    gaddr r = rd32(h);
    wr32(r + ConnRecord_cmPrivate_off, (uint32_t)slot + 1);
    for (int i = 0; i < 8; i++) {
        int32_t want = a[2] ? (int32_t)rd32(a[2] + 4 * (uint32_t)i) : 0;
        if (i == cmDataIn || i == cmDataOut)
            want = want > 0 ? want : 1024;
        else
            want = 0;
        gaddr b = want ? tb_NewPtr(want, false, true) : 0;
        r = rd32(h);
        wr32(r + ConnRecord_bufSizes_off + 4 * (uint32_t)i, b ? (uint32_t)want : 0);
        wr32(r + ConnRecord_bufferArray_off + 4 * (uint32_t)i, b);
    }
    return h;
}

static void free_conn(conn *c)
{
    gaddr r = rec(c);
    for (int i = 0; i < 8; i++)
        if (rd32(r + ConnRecord_bufferArray_off + 4 * (uint32_t)i))
            tb_DisposePtr(rd32(r + ConnRecord_bufferArray_off + 4 * (uint32_t)i));
    c->used = false;
    ctb_record_free(c->h);
}

uint32_t cm_dispose(uint16_t sel, const uint32_t *a)
{
    conn *c = conn_of(a[0]);
    if (!c)
        return 0;
    shut(c);
    c->ndone = 0;
    free_conn(c);
    return 0;
}

/* Choose picked another tool: a new record with the old one's reference
 * constants, the old one closed and gone. */
static gaddr cm_renew(gaddr h, int16_t proc_id)
{
    gaddr r = rd32(h);
    uint32_t args[5] = { (uint32_t)proc_id, rd32(r + ConnRecord_flags_off), 0, rd32(r + ConnRecord_refCon_off),
                         rd32(r + ConnRecord_userData_off) };
    gaddr sizes = tb_NewPtr(32, false, false);
    gmemcpy(sizes, r + ConnRecord_bufSizes_off, 32);
    args[2] = sizes;
    gaddr nh = cm_new(0x0108, args);
    tb_DisposePtr(sizes);
    if (nh) {
        uint32_t d[1] = { h };
        cm_dispose(0x0109, d);
    }
    return nh;
}

static void cm_dispose_record(gaddr h)
{
    uint32_t d[1] = { h };
    cm_dispose(0x0109, d);
}

const ctb_manager cm_manager = { cm_tools, COUNT(cm_tools), cm_config_changed, cm_renew, cm_dispose_record };

uint32_t cm_idle(uint16_t sel, const uint32_t *a)
{
    conn *c = conn_of(a[0]);
    if (c)
        poll_conn(c);
    return 0;
}

/* Starts an open or a listen; true when it has finished already. */
static bool start(conn *c, bool listen, int32_t timeout)
{
    gaddr k = cfg(c);
    c->open_until = ctb_deadline(timeout);
    if (c->tool == T_TCP) {
        if (listen) {
            int port = (int)rd32(k + C_LOCALPORT);
            c->lfd = tcp_listen(port ? port : (int)rd32(k + C_TCPPORT));
            if (c->lfd < 0) {
                opened(c, cmFailed);
                return true;
            }
            c->state = ST_LISTENING;
        } else {
            char host[256];
            ctb_config_str(k, C_HOST, host, sizeof host);
            c->fd = host[0] ? tcp_connect(host, (int)rd32(k + C_TCPPORT)) : -1;
            if (c->fd < 0) {
                opened(c, cmFailed);
                return true;
            }
            c->state = ST_OPENING;
        }
        c->telnet = rd8(k + C_TELNET) != 0;
        c->tn = TN_DATA;
        return false;
    }
    c->port = port_number(c);
    serial_port_open(c->port);
    setup_port(c);
    serial_port_dtr(c->port, true);
    if (c->tool == T_SERIAL) {
        c->state = listen ? ST_INCOMING : ST_OPEN;
        opened(c, cmNoErr);
        return true;
    }
    /* The modem: what it had to say before is of no interest. */
    uint8_t junk[256];
    while (serial_port_read(c->port, junk, sizeof junk) > 0)
        ;
    c->inlen = c->linelen = 0;
    c->answering = false;
    if (listen) {
        c->state = ST_LISTENING;
        return false;
    }
    char number[64], cmd[96];
    ctb_config_str(k, C_PHONE, number, sizeof number);
    if (!number[0]) {
        opened(c, cmFailed);
        return true;
    }
    snprintf(cmd, sizeof cmd, "ATD%c%s\r", rd16(k + C_DIAL) ? 'P' : 'T', number);
    modem_command(c, cmd);
    c->state = ST_OPENING;
    if (timeout < 0 || timeout > 60 * 60)
        c->open_until = ctb_now_ms() + 60000; /* S7: a minute for the carrier */
    return false;
}

static uint32_t open_or_listen(const uint32_t *a, bool listen)
{
    conn *c = conn_of(a[0]);
    if (!c)
        return (uint32_t)cmFailed;
    if (c->state != ST_CLOSED)
        return (uint32_t)cmNotClosed;
    set_err(c, cmNoErr);
    c->open_async = a[1] != 0;
    c->open_completor = a[2];
    bool done = start(c, listen, (int32_t)a[3]);
    if (c->open_async)
        return cmNoErr;
    if (!done)
        wait_for(c, settled, c->open_until);
    if (!settled(c)) {
        c->state = ST_CLOSED;
        close_fd(&c->fd);
        close_fd(&c->lfd);
        set_err(c, cmTimeOut);
    }
    return (uint32_t)(int32_t)(int16_t)rd16(rec(c) + ConnRecord_errCode_off);
}

/* (hConn, async, completor, timeout) */
uint32_t cm_open(uint16_t sel, const uint32_t *a) { return open_or_listen(a, false); }
uint32_t cm_listen(uint16_t sel, const uint32_t *a) { return open_or_listen(a, true); }

static bool answered(conn *c) { return c->state != ST_OPENING; }

/* (hConn, accept) */
uint32_t cm_accept(uint16_t sel, const uint32_t *a)
{
    conn *c = conn_of(a[0]);
    if (!c || c->state != ST_INCOMING)
        return (uint32_t)cmNoRequestPending;
    if (!a[1]) {
        shut(c);
        return cmNoErr;
    }
    if (c->tool != T_MODEM) {
        c->state = ST_OPEN;
        return cmNoErr;
    }
    c->answering = true;
    c->state = ST_OPENING;
    c->open_async = false;
    c->open_until = ctb_now_ms() + 60000;
    modem_command(c, "ATA\r");
    wait_for(c, answered, c->open_until);
    return c->state == ST_OPEN ? cmNoErr : (uint32_t)cmFailed;
}

/* (hConn, async, completor, timeout, now) */
uint32_t cm_close(uint16_t sel, const uint32_t *a)
{
    conn *c = conn_of(a[0]);
    if (!c)
        return (uint32_t)cmNotOpen;
    shut(c);
    set_err(c, cmNoErr);
    if (a[1])
        complete_later(c, a[2], 0, cmNoErr, 0, 0);
    return cmNoErr;
}

uint32_t cm_abort(uint16_t sel, const uint32_t *a)
{
    conn *c = conn_of(a[0]);
    if (!c)
        return (uint32_t)cmFailed;
    if (c->state == ST_OPENING || c->state == ST_LISTENING) {
        if (c->tool == T_MODEM)
            modem_command(c, "\r"); /* any key stops the dialling */
        close_fd(&c->fd);
        close_fd(&c->lfd);
        c->state = ST_CLOSED;
        opened(c, cmUserCancel);
    }
    return cmNoErr;
}

/* (hConn, sizes, &flags) */
uint32_t cm_status(uint16_t sel, const uint32_t *a)
{
    conn *c = conn_of(a[0]);
    if (!c)
        return (uint32_t)cmFailed;
    progress(c);
    if (c->state != ST_OPENING && c->state != ST_LISTENING)
        pump_in(c);
    serve_reads(c);
    uint32_t f = 0;
    switch (c->state) {
    case ST_OPENING: f |= cmStatusOpening; break;
    case ST_OPEN: f |= cmStatusOpen; break;
    case ST_LISTENING: f |= cmStatusListenPend; break;
    case ST_INCOMING: f |= cmStatusIncomingCallPresent; break;
    }
    if (c->inlen)
        f |= cmStatusDataAvail;
    if (c->nreads)
        f |= cmStatusDRPend;
    if (a[1]) {
        gmemset(a[1], 0, 32);
        wr32(a[1] + 4 * cmDataIn, (uint32_t)c->inlen);
    }
    if (a[2])
        wr32(a[2], f);
    return cmNoErr;
}

static int16_t queue_read(conn *c, gaddr buf, uint32_t want, bool async, gaddr completor, int32_t timeout,
                          gaddr iopb, gaddr count_ptr, uint32_t *got)
{
    pump_in(c);
    uint32_t have = c->nreads ? 0 : take(c, buf, want);
    if (have >= want) {
        *got = have;
        if (async)
            complete_later(c, completor, iopb, cmNoErr, 1, have);
        return cmNoErr;
    }
    if (c->nreads == MAX_READS)
        return cmFailed;
    cm_read_req r = { buf, count_ptr, 0, completor, iopb, want, have, ctb_deadline(timeout), async };
    if (!async) {
        /* Waits, as the tools did, but not for ever. */
        uint64_t until = r.until, cap = ctb_now_ms() + SYNC_CAP_MS;
        if (until > cap)
            until = cap;
        while (r.have < r.want && ctb_now_ms() < until && (c->state == ST_OPEN || c->inlen)) {
            int fd = c->tool == T_TCP ? c->fd : serial_port_fd(c->port);
            struct pollfd pf = { fd, POLLIN, 0 };
            poll(&pf, fd >= 0 ? 1 : 0, 10);
            pump_in(c);
            r.have += take(c, buf + r.have, r.want - r.have);
        }
        *got = r.have;
        if (r.have >= r.want)
            return cmNoErr;
        return c->state == ST_OPEN || r.have ? cmTimeOut : cmNotOpen;
    }
    if (c->state != ST_OPEN && !c->inlen && !have)
        return cmNotOpen;
    c->reads[c->nreads++] = r;
    *got = have;
    return cmNoErr;
}

/* (hConn, buffer, &toRead, channel, async, completor, timeout, &flags) */
uint32_t cm_read(uint16_t sel, const uint32_t *a)
{
    conn *c = conn_of(a[0]);
    if (!c || !a[2])
        return (uint32_t)cmFailed;
    if (a[7])
        wr16(a[7], 0);
    if (!((int16_t)a[3] & cmData)) {
        wr32(a[2], 0);
        return cmNoErr;
    }
    uint32_t got = 0;
    int16_t e = queue_read(c, a[1], rd32(a[2]), a[4] != 0, a[5], (int32_t)a[6], 0, a[2], &got);
    if (!a[4])
        wr32(a[2], got);
    set_err(c, e);
    return (uint32_t)(int32_t)e;
}

/* (hConn, buffer, &toWrite, channel, async, completor, timeout, flags) */
uint32_t cm_write(uint16_t sel, const uint32_t *a)
{
    conn *c = conn_of(a[0]);
    if (!c || !a[2])
        return (uint32_t)cmFailed;
    uint32_t n = rd32(a[2]);
    if (c->state != ST_OPEN && !(c->tool == T_SERIAL && serial_port_in_use(c->port))) {
        wr32(a[2], 0);
        return (uint32_t)cmNotOpen;
    }
    if ((int16_t)a[3] & cmData) {
        uint8_t *b = malloc(n ? n : 1);
        gget(b, a[1], n);
        send_bytes(c, b, n);
        free(b);
    }
    if (a[4])
        complete_later(c, a[5], 0, cmNoErr, 2, n);
    else
        wr32(a[2], n);
    return cmNoErr;
}

uint32_t cm_reset(uint16_t sel, const uint32_t *a)
{
    conn *c = conn_of(a[0]);
    if (!c)
        return 0;
    pump_in(c);
    c->inlen = 0;
    c->nrecent = 0;
    return 0;
}

/* (hConn, duration in ticks, async, completor) */
uint32_t cm_break(uint16_t sel, const uint32_t *a)
{
    conn *c = conn_of(a[0]);
    if (!c)
        return 0;
    if (c->tool == T_TCP) {
        static const uint8_t brk[2] = { IAC, BRK };
        if (c->telnet)
            tcp_send(c, brk, 2);
    } else {
        int32_t ms = (int32_t)a[1] * 50 / 3;
        serial_port_break(c->port, true);
        usleep((useconds_t)(ms < 0 ? 0 : ms > 1000 ? 1000 : ms) * 1000);
        serial_port_break(c->port, false);
    }
    if (a[2])
        complete_later(c, a[3], 0, cmNoErr, 0, 0);
    return 0;
}

/* (hConn, string, flags, callBack): the search's reference number */
uint32_t cm_add_search(uint16_t sel, const uint32_t *a)
{
    conn *c = conn_of(a[0]);
    if (!c || !a[1] || c->nsearch == MAX_SEARCH)
        return (uint32_t)-1;
    cm_search *s = &c->search[c->nsearch++];
    s->id = c->next_search++;
    s->len = rd8(a[1]);
    gget(s->s, a[1] + 1, (uint32_t)s->len);
    s->seven = ((uint16_t)a[2] & cmSearchSevenBit) != 0;
    s->callback = a[3];
    return (uint32_t)s->id;
}

/* (hConn, refnum) */
uint32_t cm_remove_search(uint16_t sel, const uint32_t *a)
{
    conn *c = conn_of(a[0]);
    for (int i = 0; c && i < c->nsearch; i++)
        if (c->search[i].id == (int32_t)a[1]) {
            memmove(&c->search[i], &c->search[i + 1], sizeof c->search[0] * (size_t)(c->nsearch - i - 1));
            c->nsearch--;
            break;
        }
    return 0;
}

uint32_t cm_clear_search(uint16_t sel, const uint32_t *a)
{
    conn *c = conn_of(a[0]);
    if (c)
        c->nsearch = 0;
    return 0;
}

/* (hConn, which): reads (cmDataIn) or writes waiting, cancelled */
uint32_t cm_iokill(uint16_t sel, const uint32_t *a)
{
    conn *c = conn_of(a[0]);
    if (!c)
        return (uint32_t)cmFailed;
    if ((int16_t)a[1] == cmDataIn)
        while (c->nreads)
            finish_read(c, cmUserCancel);
    return cmNoErr;
}

/* (hConn, &environs) */
uint32_t cm_get_environs(uint16_t sel, const uint32_t *a)
{
    conn *c = conn_of(a[0]);
    if (!c || !a[1])
        return (uint32_t)cmFailed;
    gaddr e = a[1], k = cfg(c);
    bool serial = c->tool != T_TCP;
    int shake = serial ? rd16(k + C_SHAKE) : 0;
    gmemset(e, 0, ConnEnvironRec_SIZEOF);
    wr32(e + ConnEnvironRec_baudRate_off, serial ? rd32(k + C_BAUD) : 115200);
    wr16(e + ConnEnvironRec_dataBits_off, serial ? (uint16_t)rd32(k + C_BITS) : 8);
    wr16(e + ConnEnvironRec_channels_off, cmData);
    wr8(e + ConnEnvironRec_swFlowControl_off, shake == 1);
    wr8(e + ConnEnvironRec_hwFlowControl_off, shake >= 2);
    return cmNoErr;
}

/* ---------------------------------------------------------------------- */
/* Parameter block calls */

/* (hConn, &iopb) */
uint32_t cm_new_iopb(uint16_t sel, const uint32_t *a)
{
    if (!a[1])
        return (uint32_t)cmFailed;
    gaddr p = tb_NewPtr(CMIOPB_SIZEOF, false, true);
    if (!p)
        return (uint32_t)memFullErr;
    wr16(p + CMIOPB_qType_off, 10); /* cmIOPBQType */
    wr32(p + CMIOPB_hConn_off, a[0]);
    wr32(a[1], p);
    return cmNoErr;
}

uint32_t cm_dispose_iopb(uint16_t sel, const uint32_t *a)
{
    if (a[1])
        tb_DisposePtr(a[1]);
    return cmNoErr;
}

/* (hConn, iopb, async) */
uint32_t cm_pb_read(uint16_t sel, const uint32_t *a)
{
    conn *c = conn_of(a[0]);
    gaddr pb = a[1];
    if (!c || !pb)
        return (uint32_t)cmFailed;
    uint32_t got = 0;
    int16_t e = queue_read(c, rd32(pb + CMIOPB_theBuffer_off), rd32(pb + CMIOPB_count_off), a[2] != 0,
                           rd32(pb + CMIOPB_userCompletion_off), (int32_t)rd32(pb + CMIOPB_timeout_off), pb, 0, &got);
    if (!a[2]) {
        wr32(pb + CMIOPB_count_off, got);
        wr16(pb + CMIOPB_errCode_off, (uint16_t)e);
    }
    return (uint32_t)(int32_t)e;
}

uint32_t cm_pb_write(uint16_t sel, const uint32_t *a)
{
    conn *c = conn_of(a[0]);
    gaddr pb = a[1];
    if (!c || !pb)
        return (uint32_t)cmFailed;
    uint32_t n = rd32(pb + CMIOPB_count_off);
    if (c->state != ST_OPEN && !(c->tool == T_SERIAL && serial_port_in_use(c->port)))
        return (uint32_t)cmNotOpen;
    uint8_t *b = malloc(n ? n : 1);
    gget(b, rd32(pb + CMIOPB_theBuffer_off), n);
    send_bytes(c, b, n);
    free(b);
    if (a[2]) {
        complete_later(c, rd32(pb + CMIOPB_userCompletion_off), pb, cmNoErr, 2, n);
    } else {
        wr16(pb + CMIOPB_errCode_off, cmNoErr);
    }
    return cmNoErr;
}

/* (hConn, iopb): a read waiting for it, cancelled */
uint32_t cm_pb_iokill(uint16_t sel, const uint32_t *a)
{
    conn *c = conn_of(a[0]);
    if (!c)
        return (uint32_t)cmFailed;
    for (int i = 0; i < c->nreads; i++)
        if (c->reads[i].iopb == a[1]) {
            cm_read_req r = c->reads[i];
            memmove(&c->reads[i], &c->reads[i + 1], sizeof c->reads[0] * (size_t)(c->nreads - i - 1));
            c->nreads--;
            complete_later(c, r.completor, r.iopb, cmUserCancel, 1, r.have);
            break;
        }
    return cmNoErr;
}

void cm_reset_all(void)
{
    static bool booted; /* before the first boot the table's fds are 0, not ours */
    for (int i = 0; i < MAX_CONNS; i++) {
        if (booted && conns[i].used) {
            close_fd(&conns[i].fd);
            close_fd(&conns[i].lfd);
        }
        conns[i].used = false;
    }
    booted = true;
}
