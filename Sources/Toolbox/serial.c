/*
 * The Serial Driver: .AIn/.AOut (the modem port) and .BIn/.BOut (the
 * printer port) as native Device Manager drivers, each port connected to
 * something on the host:
 *
 *   modem[:port]     a Hayes modem: ATDT host[:port] dials a TCP connection
 *                    (Telnet when the port is 23 or the other end starts
 *                    negotiating), +++ and ATH hang up; with a port, calls
 *                    to it ring and ATA (or S0) answers them
 *   tcp:host:port    a TCP connection, made when the port is opened
 *   listen:port      the same, waiting for one
 *   pty              a pseudo-terminal; its name goes to stderr
 *   /dev/...         a host serial device, with the Mac's baud rate, data
 *                    bits, parity and stop bits
 *   none             present but unplugged
 *
 * The modem port is a modem unless set otherwise; the printer port is
 * unplugged.  The Communications Resource Manager lists both ports.
 */
#include <arpa/inet.h>
#include <ctype.h>
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
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "machine.h"
#include "tb_internal.h"
#include "trap.h"

enum { PORT_A, PORT_B, NPORTS };
enum { K_NONE, K_MODEM, K_TCP, K_LISTEN, K_PTY, K_DEVICE };
enum { M_COMMAND, M_DIALING, M_ONLINE };
enum { INBUF = 16384 };
enum { abortErr = -27, notOpenErr = -28 };
/* Telnet */
enum { IAC = 255, DONT = 254, DO = 253, WONT = 252, WILL = 251, SB = 250, SE = 240 };
enum { TN_DATA, TN_IAC, TN_OPT, TN_SB, TN_SB_IAC };

typedef struct {
    char spec[256];
    int kind;
    int fd, lfd;
    bool opened;
    uint8_t in[INBUF]; /* host to Mac */
    int inlen;
    uint32_t baud;
    uint16_t config;
    /* a read waiting for its bytes */
    gaddr rd_pb;
    /* the modem */
    int mode;
    char cmd[256];
    int cmdlen;
    char last[256];
    bool echo, verbose, quiet;
    uint8_t sreg[32];
    int plus;          /* '+' written online, after a guard time */
    uint64_t plus_at;  /* when the third one came */
    uint64_t last_out; /* the last byte written online */
    uint64_t dial_at;
    bool telnet, telnet_maybe;
    int tn, tn_cmd;
    bool ringing;
    int rings;
    uint64_t ring_at;
} sport;

static sport ports[NPORTS];
static gaddr crm_recs[NPORTS];
static bool polling;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void close_fd(int *fd)
{
    if (*fd >= 0)
        close(*fd);
    *fd = -1;
}

static void nonblocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

/* Bytes for the Mac to read. */
static void to_mac(sport *p, const void *b, int n)
{
    if (n > INBUF - p->inlen)
        n = INBUF - p->inlen;
    memcpy(p->in + p->inlen, b, (size_t)n);
    p->inlen += n;
}

static void to_host(sport *p, const uint8_t *b, size_t n)
{
    while (n > 0 && p->fd >= 0) {
        ssize_t w = write(p->fd, b, n);
        if (w < 0 && errno == EAGAIN) {
            struct pollfd pf = { p->fd, POLLOUT, 0 };
            poll(&pf, 1, 100);
            continue;
        }
        if (w <= 0)
            return;
        b += w;
        n -= (size_t)w;
    }
}

/* ---------------------------------------------------------------------- */
/* Connections */

static int tcp_connect(const char *host, int port, bool wait)
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
        if (!wait)
            nonblocking(fd);
        if (connect(fd, res->ai_addr, res->ai_addrlen) < 0 && errno != EINPROGRESS)
            close_fd(&fd);
        if (fd >= 0 && wait)
            nonblocking(fd);
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
    nonblocking(fd);
    return fd;
}

static speed_t host_speed(uint32_t baud)
{
    static const struct { uint32_t baud; speed_t s; } speeds[] = {
        { 300, B300 }, { 600, B600 }, { 1200, B1200 }, { 1800, B1800 }, { 2400, B2400 }, { 4800, B4800 },
        { 7200, B7200 }, { 9600, B9600 }, { 14400, B14400 }, { 19200, B19200 }, { 28800, B28800 },
        { 38400, B38400 }, { 57600, B57600 }, { 115200, B115200 }, { 230400, B230400 },
    };
    speed_t best = B9600;
    for (size_t i = 0; i < sizeof speeds / sizeof speeds[0]; i++)
        if (speeds[i].baud <= baud + baud / 20)
            best = speeds[i].s;
    return best;
}

/* A host device or pty takes the Mac's settings. */
static void apply_settings(sport *p)
{
    if (p->fd < 0 || (p->kind != K_DEVICE && p->kind != K_PTY))
        return;
    struct termios t;
    if (tcgetattr(p->fd, &t) < 0)
        return;
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(tcflag_t)(CSIZE | PARENB | PARODD | CSTOPB);
    static const tcflag_t sizes[4] = { CS5, CS7, CS6, CS8 };
    t.c_cflag |= sizes[(p->config >> 10) & 3];
    switch ((p->config >> 12) & 3) {
    case 1: t.c_cflag |= PARENB | PARODD; break;
    case 3: t.c_cflag |= PARENB; break;
    }
    if ((p->config >> 14) == 3)
        t.c_cflag |= CSTOPB;
    cfsetspeed(&t, host_speed(p->baud));
    tcsetattr(p->fd, TCSANOW, &t);
}

/* The connection a port has while open (all but the modem, which dials). */
static void connect_port(sport *p)
{
    if (p->fd >= 0 || p->lfd >= 0)
        return;
    switch (p->kind) {
    case K_TCP: {
        char host[256];
        snprintf(host, sizeof host, "%s", p->spec + 4);
        char *colon = strrchr(host, ':');
        int port = 23;
        if (colon) {
            *colon = 0;
            port = atoi(colon + 1);
        }
        p->fd = tcp_connect(host, port, true);
        break;
    }
    case K_LISTEN: p->lfd = tcp_listen(atoi(p->spec + 7)); break;
    case K_PTY: {
        p->fd = posix_openpt(O_RDWR | O_NOCTTY);
        if (p->fd >= 0 && (grantpt(p->fd) < 0 || unlockpt(p->fd) < 0))
            close_fd(&p->fd);
        if (p->fd >= 0) {
            nonblocking(p->fd);
            fprintf(stderr, "bluebox: the %s port is %s\n", p == &ports[PORT_A] ? "modem" : "printer", ptsname(p->fd));
        }
        break;
    }
    case K_DEVICE:
        p->fd = open(p->spec, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (p->fd < 0)
            fprintf(stderr, "bluebox: can't open %s: %s\n", p->spec, strerror(errno));
        apply_settings(p);
        break;
    case K_MODEM:
        if (p->spec[5] == ':')
            p->lfd = tcp_listen(atoi(p->spec + 6));
        break;
    }
}

/* ---------------------------------------------------------------------- */
/* The modem */

static void result(sport *p, int code, const char *text)
{
    if (p->quiet)
        return;
    char s[64];
    if (p->verbose)
        snprintf(s, sizeof s, "\r\n%s\r\n", text);
    else
        snprintf(s, sizeof s, "%d\r", code);
    to_mac(p, s, (int)strlen(s));
}

static void connected(sport *p)
{
    char s[32];
    snprintf(s, sizeof s, "CONNECT %u", p->baud ? p->baud : 9600);
    p->mode = M_ONLINE;
    p->tn = TN_DATA;
    p->last_out = now_ms();
    p->plus = 0;
    result(p, 1, s);
}

static void hang_up(sport *p, bool tell)
{
    close_fd(&p->fd);
    bool was = p->mode != M_COMMAND;
    p->mode = M_COMMAND;
    p->telnet = p->telnet_maybe = false;
    if (tell && was)
        result(p, 3, "NO CARRIER");
}

static void dial(sport *p, const char *number)
{
    char host[256];
    int n = 0;
    for (const char *s = number; *s && n < 255; s++)
        if (!isspace((unsigned char)*s) && *s != ';' && *s != ',')
            host[n++] = *s;
    host[n] = 0;
    if (!strncasecmp(host, "telnet://", 9))
        memmove(host, host + 9, strlen(host + 9) + 1);
    if (!n) {
        result(p, 4, "ERROR");
        return;
    }
    int port = 23;
    char *colon = strrchr(host, ':');
    if (colon) {
        *colon = 0;
        port = atoi(colon + 1);
    }
    p->fd = tcp_connect(host, port, false);
    if (p->fd < 0) {
        result(p, 3, "NO CARRIER");
        return;
    }
    p->telnet = port == 23;
    p->telnet_maybe = !p->telnet;
    p->mode = M_DIALING;
    p->dial_at = now_ms();
}

static void answer(sport *p)
{
    int fd = p->lfd >= 0 ? accept(p->lfd, NULL, NULL) : -1;
    p->ringing = false;
    p->rings = 0;
    if (fd < 0) {
        result(p, 3, "NO CARRIER");
        return;
    }
    nonblocking(fd);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    p->fd = fd;
    p->telnet = p->telnet_maybe = false;
    connected(p);
}

static void reset_modem(sport *p)
{
    p->echo = p->verbose = true;
    p->quiet = false;
    memset(p->sreg, 0, sizeof p->sreg);
    p->sreg[2] = '+';
    p->sreg[3] = '\r';
    p->sreg[4] = '\n';
    p->sreg[5] = 8;
    p->sreg[12] = 50; /* guard time, 1/50 s */
}

static int number(const char **s)
{
    int v = 0;
    while (isdigit((unsigned char)**s))
        v = v * 10 + *(*s)++ - '0';
    return v;
}

/* One command line (after "AT"). */
static void command(sport *p, const char *s)
{
    while (*s) {
        char c = (char)toupper((unsigned char)*s++);
        switch (c) {
        case ' ': break;
        case 'D':
            while (*s == 'T' || *s == 't' || *s == 'P' || *s == 'p')
                s++;
            dial(p, s);
            return;
        case 'A': answer(p); return;
        case 'O':
            number(&s);
            if (p->fd >= 0) {
                connected(p);
                return;
            }
            result(p, 3, "NO CARRIER");
            return;
        case 'H':
            number(&s);
            close_fd(&p->fd);
            p->mode = M_COMMAND;
            break;
        case 'Z':
            number(&s);
            close_fd(&p->fd);
            p->mode = M_COMMAND;
            reset_modem(p);
            break;
        case 'E': p->echo = number(&s) != 0; break;
        case 'V': p->verbose = number(&s) != 0; break;
        case 'Q': p->quiet = number(&s) != 0; break;
        case 'I': {
            number(&s);
            const char *id = "\r\nBlue Box Hayes-compatible modem (TCP)";
            to_mac(p, id, (int)strlen(id));
            break;
        }
        case 'S': {
            int r = number(&s);
            if (*s == '=') {
                s++;
                int v = number(&s);
                if (r >= 0 && r < 32)
                    p->sreg[r] = (uint8_t)v;
            } else if (*s == '?') {
                s++;
                char v[16];
                snprintf(v, sizeof v, "\r\n%03u", r >= 0 && r < 32 ? p->sreg[r] : 0);
                to_mac(p, v, (int)strlen(v));
            }
            break;
        }
        case '&': case '\\': case '%': case '#': case '+':
            if (*s)
                s++;
            number(&s);
            break;
        default:
            if (isalpha((unsigned char)c)) {
                number(&s);
                break;
            }
            result(p, 4, "ERROR");
            return;
        }
    }
    result(p, 0, "OK");
}

/* A byte from the Mac while the modem takes commands. */
static void command_byte(sport *p, uint8_t b)
{
    if (p->echo)
        to_mac(p, &b, 1);
    if (b == p->sreg[3] || b == '\n') {
        p->cmd[p->cmdlen] = 0;
        char *line = p->cmd;
        while (*line == ' ')
            line++;
        if (p->cmdlen > 0 && b == p->sreg[3]) {
            if (!strncasecmp(line, "AT", 2)) {
                snprintf(p->last, sizeof p->last, "%s", line + 2);
                command(p, line + 2);
            } else if (*line) {
                result(p, 4, "ERROR");
            }
        }
        p->cmdlen = 0;
        return;
    }
    if (b == p->sreg[5] || b == 127) {
        if (p->cmdlen)
            p->cmdlen--;
        return;
    }
    if (p->cmdlen < (int)sizeof p->cmd - 1)
        p->cmd[p->cmdlen++] = (char)b;
    /* A/ repeats the last command at once. */
    if (p->cmdlen == 2 && toupper((unsigned char)p->cmd[0]) == 'A' && p->cmd[1] == '/') {
        p->cmdlen = 0;
        command(p, p->last);
    }
}

/* Bytes from the Mac to the connection, Telnet's IAC doubled. */
static void online_bytes(sport *p, const uint8_t *b, size_t n)
{
    if (!p->telnet) {
        to_host(p, b, n);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        to_host(p, &b[i], 1);
        if (b[i] == IAC)
            to_host(p, &b[i], 1);
    }
}

/* The escape: a guard time, three escape characters, a guard time.  The
 * bytes for the connection go out together. */
static void modem_write(sport *p, const uint8_t *b, size_t n)
{
    uint8_t *out = malloc(n + 3);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (p->mode != M_ONLINE) {
            if (p->mode == M_DIALING) { /* any key aborts dialling */
                hang_up(p, true);
                continue;
            }
            command_byte(p, b[i]);
            continue;
        }
        uint64_t t = now_ms(), guard = (uint64_t)p->sreg[12] * 20;
        if (b[i] == p->sreg[2] && p->plus < 3 && (p->plus > 0 || t - p->last_out >= guard)) {
            if (++p->plus == 3)
                p->plus_at = t;
            continue;
        }
        for (; p->plus > 0; p->plus--) /* not an escape after all */
            out[k++] = p->sreg[2];
        out[k++] = b[i];
        p->last_out = t;
    }
    online_bytes(p, out, k);
    free(out);
}

/* Telnet from the other end: options refused but echo and go-ahead
 * suppression, which BBSes want. */
static void telnet_in(sport *p, const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) {
        uint8_t c = b[i];
        switch (p->tn) {
        case TN_DATA:
            if (c == IAC)
                p->tn = TN_IAC;
            else
                to_mac(p, &c, 1);
            break;
        case TN_IAC:
            if (c == IAC) {
                to_mac(p, &c, 1);
                p->tn = TN_DATA;
            } else if (c >= WILL && c <= DONT) {
                p->tn_cmd = c;
                p->tn = TN_OPT;
            } else if (c == SB) {
                p->tn = TN_SB;
            } else {
                p->tn = TN_DATA;
            }
            break;
        case TN_OPT: {
            uint8_t reply[3] = { IAC, 0, c };
            bool ok = c == 1 || c == 3; /* ECHO, SUPPRESS-GO-AHEAD */
            if (p->tn_cmd == WILL)
                reply[1] = ok ? DO : DONT;
            else if (p->tn_cmd == DO)
                reply[1] = c == 3 ? WILL : WONT;
            if (reply[1])
                to_host(p, reply, 3);
            p->tn = TN_DATA;
            break;
        }
        case TN_SB: p->tn = c == IAC ? TN_SB_IAC : TN_SB; break;
        case TN_SB_IAC: p->tn = c == SE ? TN_DATA : TN_SB; break;
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Moving bytes */

static void pump(sport *p)
{
    uint64_t t = now_ms();
    if (p->kind == K_MODEM) {
        if (p->mode == M_DIALING) {
            struct pollfd pf = { p->fd, POLLOUT, 0 };
            if (poll(&pf, 1, 0) > 0) {
                int e = 0;
                socklen_t len = sizeof e;
                getsockopt(p->fd, SOL_SOCKET, SO_ERROR, &e, &len);
                if (e)
                    hang_up(p, true);
                else
                    connected(p);
            } else if (t - p->dial_at > (uint64_t)(p->sreg[7] ? p->sreg[7] : 30) * 1000) {
                hang_up(p, true);
            }
        }
        if (p->mode == M_ONLINE && p->plus == 3 && t - p->plus_at >= (uint64_t)p->sreg[12] * 20) {
            p->plus = 0;
            p->mode = M_COMMAND;
            result(p, 0, "OK");
        }
        if (p->lfd >= 0 && p->mode == M_COMMAND && p->fd < 0) {
            struct pollfd pf = { p->lfd, POLLIN, 0 };
            if (poll(&pf, 1, 0) > 0 && (!p->ringing || t - p->ring_at >= 6000)) {
                p->ringing = true;
                p->ring_at = t;
                p->rings++;
                result(p, 2, "RING");
                if (p->sreg[0] && p->rings >= p->sreg[0])
                    answer(p);
            }
        }
    } else if (p->lfd >= 0 && p->fd < 0) {
        int fd = accept(p->lfd, NULL, NULL);
        if (fd >= 0) {
            nonblocking(fd);
            p->fd = fd;
        }
    }
    if (p->fd < 0 || (p->kind == K_MODEM && p->mode == M_DIALING))
        return;
    uint8_t buf[4096];
    int room = INBUF - p->inlen;
    while (room > 0) {
        ssize_t r = read(p->fd, buf, (size_t)(room < (int)sizeof buf ? room : (int)sizeof buf));
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
            if (p->kind == K_MODEM) {
                hang_up(p, true);
            } else if (p->kind != K_DEVICE && p->kind != K_PTY) {
                close_fd(&p->fd); /* a listener waits for the next */
            }
            return;
        }
        if (r < 0)
            return;
        if (p->telnet_maybe) {
            p->telnet = buf[0] == IAC;
            p->telnet_maybe = false;
        }
        if (p->kind == K_MODEM && p->telnet)
            telnet_in(p, buf, (int)r);
        else
            to_mac(p, buf, (int)r);
        room = INBUF - p->inlen;
    }
}

static int take(sport *p, gaddr buf, int want)
{
    int n = want < p->inlen ? want : p->inlen;
    gput(buf, p->in, (uint32_t)n);
    memmove(p->in, p->in + n, (size_t)(p->inlen - n));
    p->inlen -= n;
    return n;
}

static void complete(gaddr pb, int16_t r)
{
    wr16(pb + IOParam_ioResult_off, (uint16_t)r);
    gaddr fn = rd32(pb + IOParam_ioCompletion_off);
    if (!fn)
        return;
    cpu68k *c = g_cpu;
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d);
    memcpy(a, c->a, sizeof a);
    uint16_t sr = c->sr;
    c->a[0] = pb;
    c->d[0] = (uint32_t)(int32_t)r;
    guest_call_regs(fn);
    memcpy(c->d, d, sizeof d);
    memcpy(c->a, a, sizeof a);
    c->sr = sr;
}

/* A read is done when it has all it asked for, as the Serial Driver's were. */
static bool read_done(sport *p, gaddr pb)
{
    uint32_t want = rd32(pb + IOParam_ioReqCount_off), have = rd32(pb + IOParam_ioActCount_off);
    if (have < want)
        have += (uint32_t)take(p, rd32(pb + IOParam_ioBuffer_off) + have, (int)(want - have));
    wr32(pb + IOParam_ioActCount_off, have);
    return have >= want;
}

void serial_poll(void)
{
    if (polling || !g_cpu)
        return;
    polling = true;
    for (int i = 0; i < NPORTS; i++) {
        sport *p = &ports[i];
        if (!p->opened)
            continue;
        pump(p);
        if (p->rd_pb && read_done(p, p->rd_pb)) {
            gaddr pb = p->rd_pb;
            p->rd_pb = 0;
            complete(pb, noErr);
        }
    }
    polling = false;
}

/* ---------------------------------------------------------------------- */
/* The drivers */

static sport *port_of(gaddr pb)
{
    int16_t unit = (int16_t)(-1 - (int16_t)rd16(pb + IOParam_ioRefNum_off));
    return &ports[unit >= 7 ? PORT_B : PORT_A];
}

static int16_t ser_open(gaddr pb)
{
    sport *p = port_of(pb);
    if (!p->opened) {
        p->opened = true;
        p->inlen = 0;
        p->mode = M_COMMAND;
        reset_modem(p);
        connect_port(p);
    }
    return noErr;
}

static int16_t ser_close(gaddr pb)
{
    sport *p = port_of(pb);
    if (!p->opened)
        return noErr;
    if (p->rd_pb) {
        complete(p->rd_pb, abortErr);
        p->rd_pb = 0;
    }
    p->opened = false;
    if (p->kind == K_MODEM) /* DTR drops: the modem hangs up */
        hang_up(p, false);
    else if (p->kind == K_TCP)
        close_fd(&p->fd);
    return noErr;
}

static int16_t ser_control(gaddr pb, bool async)
{
    sport *p = port_of(pb);
    gaddr cs = pb + CntrlParam_csParam_off;
    switch ((int16_t)rd16(pb + CntrlParam_csCode_off)) {
    case 8: { /* SerReset */
        p->config = rd16(cs);
        uint16_t code = p->config & 0x3FF;
        p->baud = 115200u / (code + 2u);
        apply_settings(p);
        return noErr;
    }
    case 13: /* the baud rate, and the rate we take back */
        p->baud = rd16(cs);
        apply_settings(p);
        return noErr;
    case 9: case 10: case 11: case 12: case 14: case 16: case 17: case 18:
    case 19: case 20: case 21: case 22: case 23: case 24: case 25:
        return noErr;
    case 1: /* KillIO by control code */
        if (p->rd_pb) {
            complete(p->rd_pb, abortErr);
            p->rd_pb = 0;
        }
        return noErr;
    default: return -17; /* controlErr */
    }
}

static int16_t ser_status(gaddr pb, bool async)
{
    sport *p = port_of(pb);
    gaddr cs = pb + CntrlParam_csParam_off;
    switch ((int16_t)rd16(pb + CntrlParam_csCode_off)) {
    case 2: /* SerGetBuf */
        pump(p);
        wr32(cs, (uint32_t)p->inlen);
        return noErr;
    case 8: /* SerStatus: cumErrs, xOffSent, rdPend, wrPend, ctsHold, xOffHold */
        gmemset(cs, 0, 6);
        wr8(cs + 2, p->rd_pb ? 0xFF : 0);
        return noErr;
    case 9: /* SerStatus with DSR and the modem's lines */
        gmemset(cs, 0, 8);
        wr8(cs + 2, p->rd_pb ? 0xFF : 0);
        wr8(cs + 7, (p->fd >= 0 ? 0x08 : 0) | 0x10); /* DCD, CTS */
        return noErr;
    default: return -18; /* statusErr */
    }
}

static int16_t ser_read(gaddr pb, bool async)
{
    sport *p = port_of(pb);
    if (!p->opened)
        return notOpenErr;
    if (p->rd_pb)
        return -19; /* readErr: one at a time */
    wr32(pb + IOParam_ioActCount_off, 0);
    pump(p);
    if (read_done(p, pb))
        return noErr;
    if (async) {
        p->rd_pb = pb;
        return ioInProgress;
    }
    /* Waits, as the driver did, but not for ever: a program that reads
     * more than will come gets what came. */
    uint64_t until = now_ms() + 30000;
    while (!read_done(p, pb) && now_ms() < until) {
        struct pollfd pf = { p->fd, POLLIN, 0 };
        poll(&pf, p->fd >= 0 ? 1 : 0, 20);
        pump(p);
    }
    return noErr;
}

static int16_t ser_write(gaddr pb, bool async)
{
    sport *p = port_of(pb);
    if (!p->opened)
        return notOpenErr;
    uint32_t n = rd32(pb + IOParam_ioReqCount_off);
    uint8_t *b = malloc(n ? n : 1);
    gget(b, rd32(pb + IOParam_ioBuffer_off), n);
    if (p->kind == K_MODEM)
        modem_write(p, b, n);
    else
        to_host(p, b, n);
    free(b);
    wr32(pb + IOParam_ioActCount_off, n);
    return noErr;
}

static int16_t ser_kill(gaddr pb)
{
    sport *p = port_of(pb);
    if (p->rd_pb) {
        complete(p->rd_pb, abortErr);
        p->rd_pb = 0;
    }
    return noErr;
}

const native_driver serial_drivers[4] = {
    { ".AIn", 5, ser_open, ser_close, ser_control, ser_status, ser_read, ser_write, ser_kill },
    { ".AOut", 6, ser_open, ser_close, ser_control, ser_status, ser_read, ser_write, ser_kill },
    { ".BIn", 7, ser_open, ser_close, ser_control, ser_status, ser_read, ser_write, ser_kill },
    { ".BOut", 8, ser_open, ser_close, ser_control, ser_status, ser_read, ser_write, ser_kill },
};

/* ---------------------------------------------------------------------- */
/* Setup */

static char specs[NPORTS][256] = { "modem", "none" };

bool machine_set_serial(int port, const char *spec)
{
    if (port < 0 || port >= NPORTS || !spec)
        return false;
    bool ok = !strcmp(spec, "none") || !strcmp(spec, "pty") || !strcmp(spec, "modem") ||
              (!strncmp(spec, "modem:", 6) && atoi(spec + 6) > 0) || (!strncmp(spec, "tcp:", 4) && strchr(spec + 4, ':')) ||
              (!strncmp(spec, "listen:", 7) && atoi(spec + 7) > 0) || spec[0] == '/';
    if (ok)
        snprintf(specs[port], sizeof specs[port], "%s", spec);
    return ok;
}

static int kind_of(const char *spec)
{
    if (!strncmp(spec, "modem", 5))
        return K_MODEM;
    if (!strncmp(spec, "tcp:", 4))
        return K_TCP;
    if (!strncmp(spec, "listen:", 7))
        return K_LISTEN;
    if (!strcmp(spec, "pty"))
        return K_PTY;
    if (spec[0] == '/')
        return K_DEVICE;
    return K_NONE;
}

/* A serial device's record; its crmAttributes points to a CRMSerialRecord:
 * version, inputDriverName, outputDriverName, name (string handles),
 * deviceIcon, ratedSpeed, maxSpeed, reserved. */
static gaddr crm_record(int id, const char *name, const char *in, const char *out)
{
    gaddr rec = tb_NewPtr(CRMRec_SIZEOF, true, true);
    gaddr ser = tb_NewPtr(30, true, true);
    gaddr names[3];
    const char *s[3] = { in, out, name };
    for (int i = 0; i < 3; i++) {
        size_t n = strlen(s[i]);
        names[i] = tb_NewHandle((int32_t)n + 1, true, false);
        wr8(rd32(names[i]), (uint8_t)n);
        gput(rd32(names[i]) + 1, s[i], (uint32_t)n);
    }
    wr32(ser + 2, names[0]);
    wr32(ser + 6, names[1]);
    wr32(ser + 10, names[2]);
    wr32(ser + 18, 57600);
    wr32(ser + 22, 230400);
    CRMRec_set_crmVersion(rec, 1);
    CRMRec_set_crmDeviceType(rec, 1); /* crmSerialDevice */
    CRMRec_set_crmDeviceID(rec, id);
    CRMRec_set_crmAttributes(rec, (int32_t)ser);
    return rec;
}

/* CRMSearch: the next record of the type asked for, by device ID. */
gaddr serial_crm_search(gaddr req)
{
    if (!req || CRMRec_crmDeviceType(req) != 1)
        return 0;
    int32_t after = CRMRec_crmDeviceID(req);
    for (int i = 0; i < NPORTS; i++)
        if (crm_recs[i] && CRMRec_crmDeviceID(crm_recs[i]) > after)
            return crm_recs[i];
    return 0;
}

/* _CommToolboxDispatch (A08B): A0 points to the selector word, the
 * arguments after it.  Of the Communications Toolbox only the Resource
 * Manager is here, enough for programs to find the serial ports; a
 * program that tests for the trap then asks the CRM for them. */
static void ctb_dispatch(cpu68k *c, uint16_t tw)
{
    uint16_t sel = rd16(c->a[0]);
    switch (sel) {
    case 0x0505: c->d[0] = serial_crm_search(rd32(c->a[0] + 2)); break; /* CRMSearch */
    case 0x0401: case 0x0501: c->d[0] = 0; break;                     /* InitCTBUtilities, InitCRM */
    case 0x0101: case 0x0201: case 0x0301: c->d[0] = (uint32_t)-2; break; /* InitCM, InitTM, InitFT: no tools */
    default:
        LOGF(LOG_UNIMPL, "CommToolboxDispatch selector %04X\n", sel);
        c->d[0] = 0;
    }
}

void serial_reset(void)
{
    for (int i = 0; i < NPORTS; i++) {
        sport *p = &ports[i];
        if (p->spec[0]) {
            close_fd(&p->fd);
            close_fd(&p->lfd);
        }
        memset(p, 0, sizeof *p);
        p->fd = p->lfd = -1;
        snprintf(p->spec, sizeof p->spec, "%s", specs[i]);
        p->kind = kind_of(p->spec);
        p->baud = 9600;
        p->config = 0xCC0A; /* 9600 baud, 8 data bits, no parity, 1 stop bit */
        reset_modem(p);
    }
    polling = false;
    crm_recs[PORT_A] = crm_record(1, "Modem Port", ".AIn", ".AOut");
    crm_recs[PORT_B] = crm_record(2, "Printer Port", ".BIn", ".BOut");
    wr32(OS_TRAP_TABLE + 0x8Bu * 4, native_register(ctb_dispatch, "CommToolboxDispatch", 0xA08B));
}
