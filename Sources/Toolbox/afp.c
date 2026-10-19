/*
 * An AppleShare client: AFP 2.x calls, over TCP (DSI) or AppleTalk (ASP,
 * asp.c), for volumes mirrored by the File Manager.
 *
 * AFP over TCP wraps each call in DSI: a 16-byte header (flags 0 request
 * or 1 reply, command, request ID, an error code or write offset, the
 * length after the header, 4 reserved bytes), then the AFP command block.
 * A session starts with DSIOpenSession, whose options give the server's
 * request quantum (the largest request it takes) and ours for attention
 * packets; DSICommand carries a call and its reply; DSIWrite carries
 * FPWrite's block and then its data, the write offset saying where the
 * data starts.  The server sends DSITickle when it has heard nothing in a
 * while, closing the session if it goes unanswered, and DSIAttention for
 * news (ignored here).
 *
 * AFP itself: a call is a command byte and its parameters, big-endian,
 * the reply a 32-bit result (0, or kFPAccessDenied -5000 and on) and
 * data.  Names here are AFP 2 long names, MacRoman Pascal strings with
 * path type 2, and dates are seconds from 2000-01-01 GMT.  Files and
 * directories are described by bitmaps of the parameters wanted, packed
 * in bit order, names as offsets to strings after them.
 *
 * Calls are synchronous.  A session that drops (a server's idle timeout
 * when nothing called afp_idle, a restart) is opened again, logged in
 * and its volumes opened again, and the call done again once.
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
#include <time.h>
#include <unistd.h>

#include "afp.h"
#include "asp.h"

/* AFP result codes */
enum {
    kFPNoErr = 0, kFPAccessDenied = -5000, kFPBadUAM = -5002, kFPBadVersNum = -5003, kFPEOFErr = -5009,
    kFPMiscErr = -5014, kFPNoServer = -5016, kFPObjectNotFound = -5018, kFPParmErr = -5019,
    kFPSessClosed = -5022, kFPUserNotAuth = -5023,
};
/* AFP commands */
enum {
    FPCloseVol = 2, FPCloseFork = 4, FPCreateDir = 6, FPCreateFile = 7, FPDelete = 8, FPEnumerate = 9,
    FPGetSrvrParms = 16, FPLogin = 18, FPLogout = 20, FPMoveAndRename = 23, FPOpenVol = 24, FPOpenFork = 26,
    FPRead = 27, FPRename = 28, FPSetForkParms = 31, FPWrite = 33, FPGetFileDirParms = 34,
    FPSetFileDirParms = 35,
};
/* File and directory bitmaps */
enum {
    B_ATTR = 0x1, B_PARENT = 0x2, B_CREATE = 0x4, B_MODIFY = 0x8, B_BACKUP = 0x10, B_FINDER = 0x20,
    B_LONGNAME = 0x40, B_SHORTNAME = 0x80, B_NODE = 0x100, B_DATALEN = 0x200, B_RSRCLEN = 0x400,
    /* directories' own after B_NODE */
    B_OFFSPRING = 0x200, B_OWNER = 0x400, B_GROUP = 0x800, B_ACCESS = 0x1000,
};
enum { FILE_BITMAP = B_ATTR | B_PARENT | B_CREATE | B_MODIFY | B_FINDER | B_LONGNAME | B_NODE | B_DATALEN | B_RSRCLEN,
       DIR_BITMAP = B_ATTR | B_PARENT | B_CREATE | B_MODIFY | B_FINDER | B_LONGNAME | B_NODE };
enum { VOL_ID_BIT = 0x20, LONG_NAMES = 2 };
/* DSI */
enum { DSI_CLOSE = 1, DSI_COMMAND = 2, DSI_GETSTATUS = 3, DSI_OPEN = 4, DSI_TICKLE = 5, DSI_WRITE = 6,
       DSI_ATTENTION = 8 };
enum { DSI_HDR = 16, DSI_TIMEOUT_MS = 30000, ATTN_QUANTUM = 1024, MAX_CHUNK = 128 * 1024 };
/* seconds from 1970 to 2000, and from 1904 to 1970 */
#define AFP_EPOCH 946684800LL
#define MAC_EPOCH 2082844800LL

enum { MAX_VOLS = 32 };
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

struct afp_session {
    bool tcp;
    /* DSI */
    int fd;
    uint16_t reqid;
    uint32_t server_quantum;
    /* ASP */
    asp_session *asp;
    int quantum; /* the most a read or write moves at once */
    bool alive;
    time_t last_sent;
    /* to connect again */
    char where[256], user[64], password[64];
    struct {
        char name[28];
        uint16_t id;
    } vols[MAX_VOLS];
    int nvols;
    uint8_t *buf; /* replies */
    int bufcap;
};

/* ---------------------------------------------------------------------- */
/* Packing */

typedef struct {
    uint8_t b[1024];
    int n;
} block;

static void p8(block *k, int v) { k->b[k->n++] = (uint8_t)v; }
static void p16(block *k, int v)
{
    p8(k, v >> 8);
    p8(k, v);
}
static void p32(block *k, uint32_t v)
{
    p16(k, (int)(v >> 16));
    p16(k, (int)(v & 0xFFFF));
}
static void pstr(block *k, const char *s)
{
    size_t n = strlen(s);
    n = n > 255 ? 255 : n;
    p8(k, (int)n);
    memcpy(k->b + k->n, s, n);
    k->n += (int)n;
}
static void pad(block *k)
{
    if (k->n & 1)
        p8(k, 0);
}
/* A long-name path: type 2 and the name. */
static void ppath(block *k, const char *name)
{
    p8(k, LONG_NAMES);
    pstr(k, name ? name : "");
}

static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static bool name_ok(const char *name) { return !name || strlen(name) <= 31; }

/* An AFP date as a Mac one: local seconds since 1904; 0 for none. */
static uint32_t mac_time(uint32_t afp)
{
    if (afp == 0x80000000u)
        return 0;
    time_t t = (time_t)((int64_t)(int32_t)afp + AFP_EPOCH);
    struct tm lt;
    localtime_r(&t, &lt);
    return (uint32_t)((int64_t)t + lt.tm_gmtoff + MAC_EPOCH);
}

/* ---------------------------------------------------------------------- */
/* DSI */

static bool dsi_send(afp_session *s, int flags, int cmd, uint16_t id, uint32_t code, const uint8_t *a, int alen,
                     const uint8_t *b, int blen)
{
    uint8_t h[DSI_HDR];
    uint32_t total = (uint32_t)(alen + blen);
    h[0] = (uint8_t)flags;
    h[1] = (uint8_t)cmd;
    h[2] = (uint8_t)(id >> 8);
    h[3] = (uint8_t)id;
    for (int i = 0; i < 4; i++) {
        h[4 + i] = (uint8_t)(code >> (24 - 8 * i));
        h[8 + i] = (uint8_t)(total >> (24 - 8 * i));
        h[12 + i] = 0;
    }
    const uint8_t *parts[3] = { h, a, b };
    int lens[3] = { DSI_HDR, alen, blen };
    for (int p = 0; p < 3; p++)
        for (int done = 0; done < lens[p];) {
            ssize_t w = send(s->fd, parts[p] + done, (size_t)(lens[p] - done), SEND_FLAGS);
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0)
                return false;
            done += (int)w;
        }
    s->last_sent = time(NULL);
    return true;
}

static bool read_full(int fd, uint8_t *p, size_t n)
{
    while (n) {
        struct pollfd pf = { fd, POLLIN, 0 };
        int r = poll(&pf, 1, DSI_TIMEOUT_MS);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return false;
        ssize_t k = recv(fd, p, n, 0);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

/* Bytes not wanted, read and dropped. */
static bool skip(int fd, uint32_t n)
{
    uint8_t junk[4096];
    while (n) {
        uint32_t k = n < sizeof junk ? n : (uint32_t)sizeof junk;
        if (!read_full(fd, junk, k))
            return false;
        n -= k;
    }
    return true;
}

static void dsi_lost(afp_session *s)
{
    if (s->fd >= 0)
        close(s->fd);
    s->fd = -1;
    s->alive = false;
}

/* A DSI request and its reply (data into s->buf): the error code field. */
static int dsi_call(afp_session *s, int cmd, const uint8_t *a, int alen, const uint8_t *b, int blen, uint32_t code,
                    int *rlen)
{
    *rlen = 0;
    if (!s->alive)
        return kFPSessClosed;
    uint16_t id = s->reqid++;
    if (!dsi_send(s, 0, cmd, id, code, a, alen, b, blen)) {
        dsi_lost(s);
        return kFPSessClosed;
    }
    for (;;) {
        uint8_t h[DSI_HDR];
        if (!read_full(s->fd, h, DSI_HDR)) {
            dsi_lost(s);
            return kFPSessClosed;
        }
        uint32_t len = g32(h + 8);
        if (h[0] == 0) { /* the server's request */
            if (!skip(s->fd, len)) {
                dsi_lost(s);
                return kFPSessClosed;
            }
            if (h[1] == DSI_TICKLE)
                dsi_send(s, 0, DSI_TICKLE, s->reqid++, 0, NULL, 0, NULL, 0);
            else if (h[1] == DSI_CLOSE) {
                dsi_lost(s);
                return kFPSessClosed;
            }
            continue;
        }
        if (g16(h + 2) != id || h[1] != cmd) {
            if (!skip(s->fd, len)) {
                dsi_lost(s);
                return kFPSessClosed;
            }
            continue;
        }
        uint32_t keep = len < (uint32_t)s->bufcap ? len : (uint32_t)s->bufcap;
        if (!read_full(s->fd, s->buf, keep) || !skip(s->fd, len - keep)) {
            dsi_lost(s);
            return kFPSessClosed;
        }
        *rlen = (int)keep;
        return (int32_t)g32(h + 4);
    }
}

static int tcp_connect(const char *where, char *err, size_t errlen)
{
    char host[256];
    const char *port = "548";
    snprintf(host, sizeof host, "%s", where);
    char *colon = strrchr(host, ':');
    if (colon && !strchr(colon + 1, ']') && strchr(host, ':') == colon) {
        *colon = 0;
        port = colon + 1;
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int gai = getaddrinfo(host, port, &hints, &res);
    if (gai) {
        snprintf(err, errlen, "%s: %s", host, gai_strerror(gai));
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0)
            continue;
        int fl = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int r = connect(fd, a->ai_addr, a->ai_addrlen);
        if (r < 0 && errno == EINPROGRESS) {
            struct pollfd pf = { fd, POLLOUT, 0 };
            int soerr = 0;
            socklen_t sl = sizeof soerr;
            if (poll(&pf, 1, 10000) == 1 && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) && !soerr)
                r = 0;
            else
                errno = soerr ? soerr : ETIMEDOUT;
        }
        if (r < 0) {
            snprintf(err, errlen, "%s: %s", where, strerror(errno));
            close(fd);
            fd = -1;
            continue;
        }
        fcntl(fd, F_SETFL, fl);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    }
    freeaddrinfo(res);
    return fd;
}

static bool dsi_open(afp_session *s, char *err, size_t errlen)
{
    s->fd = tcp_connect(s->where, err, errlen);
    if (s->fd < 0)
        return false;
    s->alive = true;
    uint8_t opt[6] = { 1, 4, ATTN_QUANTUM >> 24, ATTN_QUANTUM >> 16 & 0xFF, ATTN_QUANTUM >> 8 & 0xFF,
                       ATTN_QUANTUM & 0xFF };
    int n, r = dsi_call(s, DSI_OPEN, opt, sizeof opt, NULL, 0, 0, &n);
    if (r != 0) {
        snprintf(err, errlen, "%s: the server didn't open a session (%d)", s->where, r);
        dsi_lost(s);
        return false;
    }
    s->server_quantum = 0;
    for (int i = 0; i + 2 <= n && i + 2 + s->buf[i + 1] <= n; i += 2 + s->buf[i + 1])
        if (s->buf[i] == 0 && s->buf[i + 1] == 4)
            s->server_quantum = g32(s->buf + i + 2);
    if (s->server_quantum < 1024)
        s->server_quantum = 1024;
    uint32_t q = s->server_quantum - 64;
    s->quantum = (int)(q < MAX_CHUNK ? q : MAX_CHUNK);
    return true;
}

/* ---------------------------------------------------------------------- */
/* Calls */

/* A call and its reply in s->buf; *rlen its length. */
static int call(afp_session *s, const block *k, int *rlen)
{
    int dummy;
    if (!rlen)
        rlen = &dummy;
    if (s->tcp)
        return dsi_call(s, DSI_COMMAND, k->b, k->n, NULL, 0, 0, rlen);
    if (!s->asp || !asp_alive(s->asp)) {
        *rlen = 0;
        return kFPSessClosed;
    }
    s->last_sent = time(NULL);
    return asp_command(s->asp, k->b, k->n, s->buf, s->bufcap, rlen);
}

static int call_write(afp_session *s, const block *k, const uint8_t *data, int dlen, int *rlen)
{
    if (s->tcp)
        return dsi_call(s, DSI_WRITE, k->b, k->n, data, dlen, (uint32_t)k->n, rlen);
    if (!s->asp || !asp_alive(s->asp)) {
        *rlen = 0;
        return kFPSessClosed;
    }
    s->last_sent = time(NULL);
    return asp_write(s->asp, k->b, k->n, data, dlen, s->buf, s->bufcap, rlen);
}

static int login(afp_session *s)
{
    static const char *versions[] = { "AFP2.2", "AFPVersion 2.1", "AFPVersion 2.0" };
    bool guest = !s->user[0];
    int r = kFPBadVersNum;
    for (size_t v = 0; v < sizeof versions / sizeof versions[0] && r == kFPBadVersNum; v++) {
        block k = { .n = 0 };
        p8(&k, FPLogin);
        pstr(&k, versions[v]);
        pstr(&k, guest ? "No User Authent" : "Cleartxt Passwrd");
        if (!guest) {
            pstr(&k, s->user);
            pad(&k);
            char pw[8] = { 0 };
            memcpy(pw, s->password, strnlen(s->password, 8));
            memcpy(k.b + k.n, pw, 8);
            k.n += 8;
        }
        r = call(s, &k, NULL);
    }
    return r;
}

static void hang_up(afp_session *s)
{
    if (s->tcp) {
        if (s->alive)
            dsi_send(s, 0, DSI_CLOSE, s->reqid++, 0, NULL, 0, NULL, 0);
        dsi_lost(s);
    } else if (s->asp) {
        asp_close(s->asp);
        s->asp = NULL;
    }
    s->alive = false;
}

/* A session and a login, from scratch. */
static int session(afp_session *s, char *err, size_t errlen)
{
    if (s->tcp) {
        if (!dsi_open(s, err, errlen))
            return kFPNoServer;
    } else {
        s->asp = asp_open(s->where, NULL, NULL, err, errlen);
        if (!s->asp)
            return kFPNoServer;
        s->quantum = ASP_QUANTUM;
        s->alive = true;
    }
    int r = login(s);
    if (r != 0) {
        snprintf(err, errlen, "%s: the server refused the login (%d)", s->where, r);
        hang_up(s);
    }
    return r;
}

static afp_session *connect_to(bool tcp, const char *where, const char *user, const char *password, char *err,
                               size_t errlen)
{
    afp_session *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->tcp = tcp;
    s->fd = -1;
    s->bufcap = MAX_CHUNK + 4096;
    s->buf = malloc((size_t)s->bufcap);
    snprintf(s->where, sizeof s->where, "%s", where);
    snprintf(s->user, sizeof s->user, "%s", user ? user : "");
    snprintf(s->password, sizeof s->password, "%s", password ? password : "");
    if (!s->buf || session(s, err, errlen) != 0) {
        free(s->buf);
        free(s);
        return NULL;
    }
    return s;
}

afp_session *afp_connect_tcp(const char *host, const char *user, const char *password, char *err, size_t errlen)
{
    return connect_to(true, host, user, password, err, errlen);
}

afp_session *afp_connect_atalk(const char *nbp_name, const char *user, const char *password, char *err,
                               size_t errlen)
{
    return connect_to(false, nbp_name, user, password, err, errlen);
}

void afp_close(afp_session *s)
{
    if (!s)
        return;
    if (s->alive) {
        block k = { .n = 0 };
        p8(&k, FPLogout);
        p8(&k, 0);
        call(s, &k, NULL);
    }
    hang_up(s);
    free(s->buf);
    free(s);
}

void afp_idle(afp_session *s)
{
    if (!s || !s->alive || time(NULL) - s->last_sent < 30)
        return;
    if (s->tcp) {
        /* a tickle, and whatever the server sent meanwhile */
        dsi_send(s, 0, DSI_TICKLE, s->reqid++, 0, NULL, 0, NULL, 0);
        struct pollfd pf = { s->fd, POLLIN, 0 };
        while (s->alive && poll(&pf, 1, 0) == 1) {
            uint8_t h[DSI_HDR];
            if (!read_full(s->fd, h, DSI_HDR) || !skip(s->fd, g32(h + 8)) || (h[0] == 0 && h[1] == DSI_CLOSE))
                dsi_lost(s);
        }
    } else {
        asp_tickle(s->asp);
        s->last_sent = time(NULL);
    }
}

/* After a dropped session: open it again, and the volumes. */
static bool reconnect(afp_session *s)
{
    char err[256];
    hang_up(s);
    if (session(s, err, sizeof err) != 0)
        return false;
    for (int i = 0; i < s->nvols; i++) {
        block k = { .n = 0 };
        p8(&k, FPOpenVol);
        p8(&k, 0);
        p16(&k, VOL_ID_BIT);
        pstr(&k, s->vols[i].name);
        int n, r = call(s, &k, &n);
        if (r != 0 || n < 4 || g16(s->buf + 2) != s->vols[i].id)
            return false; /* gone, or a different ID */
    }
    return true;
}

/* An operation done again on a new session when the session was lost. */
#define RETRYING(s, expr)                                                                                         \
    do {                                                                                                          \
        int r_ = (expr);                                                                                          \
        if (r_ == kFPSessClosed && reconnect(s))                                                                  \
            r_ = (expr);                                                                                          \
        return r_;                                                                                                \
    } while (0)

/* ---------------------------------------------------------------------- */
/* Volumes */

static int volumes(afp_session *s, char (**names)[28], int *count)
{
    *names = NULL;
    *count = 0;
    block k = { .n = 0 };
    p8(&k, FPGetSrvrParms);
    p8(&k, 0);
    int n, r = call(s, &k, &n);
    if (r != 0)
        return r;
    if (n < 5)
        return kFPMiscErr;
    int nv = s->buf[4];
    char (*v)[28] = calloc((size_t)(nv ? nv : 1), sizeof *v);
    if (!v)
        return kFPMiscErr;
    int got = 0;
    for (int i = 0, p = 5; i < nv && p + 2 <= n; i++) {
        int len = s->buf[p + 1];
        if (p + 2 + len > n)
            break;
        int l = len > 27 ? 27 : len;
        memcpy(v[got], s->buf + p + 2, (size_t)l);
        v[got++][l] = 0;
        p += 2 + len;
    }
    *names = v;
    *count = got;
    return 0;
}

int afp_volumes(afp_session *s, char (**names)[28], int *count) { RETRYING(s, volumes(s, names, count)); }

static int open_volume(afp_session *s, const char *name, uint16_t *volid)
{
    block k = { .n = 0 };
    p8(&k, FPOpenVol);
    p8(&k, 0);
    p16(&k, VOL_ID_BIT);
    pstr(&k, name);
    int n, r = call(s, &k, &n);
    if (r != 0)
        return r;
    if (n < 4)
        return kFPMiscErr;
    *volid = g16(s->buf + 2);
    int i;
    for (i = 0; i < s->nvols && strcmp(s->vols[i].name, name); i++)
        ;
    if (i == s->nvols && s->nvols < MAX_VOLS)
        s->nvols++;
    if (i < MAX_VOLS) {
        snprintf(s->vols[i].name, sizeof s->vols[i].name, "%s", name);
        s->vols[i].id = *volid;
    }
    return 0;
}

int afp_open_volume(afp_session *s, const char *name, uint16_t *volid)
{
    if (strlen(name) > 27)
        return kFPParmErr;
    RETRYING(s, open_volume(s, name, volid));
}

int afp_close_volume(afp_session *s, uint16_t vol)
{
    for (int i = 0; i < s->nvols; i++)
        if (s->vols[i].id == vol) {
            memmove(s->vols + i, s->vols + i + 1, sizeof s->vols[0] * (size_t)(s->nvols - i - 1));
            s->nvols--;
            break;
        }
    block k = { .n = 0 };
    p8(&k, FPCloseVol);
    p8(&k, 0);
    p16(&k, vol);
    return call(s, &k, NULL);
}

/* ---------------------------------------------------------------------- */
/* Files and directories */

/* A file's or directory's parameters, as packed for its bitmap. */
static bool parse_params(const uint8_t *p, int n, bool dir, uint16_t bitmap, afp_entry *e)
{
    memset(e, 0, sizeof *e);
    e->dir = dir;
    int at = 0;
#define NEED(k) \
    if (at + (k) > n) \
        return false
    for (int bit = 0; bit < 16; bit++) {
        uint16_t b = (uint16_t)(1 << bit);
        if (!(bitmap & b))
            continue;
        switch (b) {
        case B_ATTR: NEED(2); at += 2; break;
        case B_PARENT: NEED(4); e->parent = g32(p + at); at += 4; break;
        case B_CREATE: NEED(4); e->create_date = mac_time(g32(p + at)); at += 4; break;
        case B_MODIFY: NEED(4); e->modify_date = mac_time(g32(p + at)); at += 4; break;
        case B_BACKUP: NEED(4); at += 4; break;
        case B_FINDER: NEED(32); memcpy(e->finder_info, p + at, 32); at += 32; break;
        case B_LONGNAME:
        case B_SHORTNAME: {
            NEED(2);
            int off = g16(p + at);
            at += 2;
            if (b == B_LONGNAME && off && off < n) {
                int l = p[off];
                if (off + 1 + l > n)
                    return false;
                l = l > 31 ? 31 : l;
                memcpy(e->name, p + off + 1, (size_t)l);
                e->name[l] = 0;
            }
            break;
        }
        case B_NODE: NEED(4); e->id = g32(p + at); at += 4; break;
        case 0x200: /* data fork length; a directory's offspring count */
            if (dir) {
                NEED(2);
                at += 2;
            } else {
                NEED(4);
                e->data_len = g32(p + at);
                at += 4;
            }
            break;
        case 0x400: /* resource fork length; a directory's owner */
            NEED(4);
            if (!dir)
                e->rsrc_len = g32(p + at);
            at += 4;
            break;
        case 0x800: /* group ID */
        case 0x1000: /* access rights */
            NEED(4);
            at += 4;
            break;
        default: return false; /* not asked for */
        }
    }
#undef NEED
    return true;
}

static int list(afp_session *s, uint16_t vol, uint32_t dir, afp_entry **entries, int *count)
{
    *entries = NULL;
    *count = 0;
    int cap = 0, got = 0;
    afp_entry *v = NULL;
    int max_reply = s->tcp ? (s->quantum < 32768 ? s->quantum : 32768) : ASP_QUANTUM;
    for (int start = 1;;) {
        block k = { .n = 0 };
        p8(&k, FPEnumerate);
        p8(&k, 0);
        p16(&k, vol);
        p32(&k, dir);
        p16(&k, FILE_BITMAP);
        p16(&k, DIR_BITMAP);
        p16(&k, 200);
        p16(&k, start);
        p16(&k, max_reply);
        ppath(&k, "");
        int n, r = call(s, &k, &n);
        if (r == kFPObjectNotFound)
            break;
        if (r != 0 || n < 6) {
            free(v);
            return r ? r : kFPMiscErr;
        }
        uint16_t fb = g16(s->buf), db = g16(s->buf + 2);
        int act = g16(s->buf + 4);
        if (!act)
            break;
        const uint8_t *p = s->buf + 6, *end = s->buf + n;
        for (int i = 0; i < act && p + 2 <= end; i++) {
            int len = p[0];
            if (len < 2 || p + len > end)
                break;
            if (got == cap) {
                cap = cap ? cap * 2 : 64;
                afp_entry *nv = realloc(v, sizeof *v * (size_t)cap);
                if (!nv) {
                    free(v);
                    return kFPMiscErr;
                }
                v = nv;
            }
            bool isdir = (p[1] & 0x80) != 0;
            if (parse_params(p + 2, len - 2, isdir, isdir ? db : fb, &v[got]))
                got++;
            p += len;
        }
        start += act;
    }
    *entries = v;
    *count = got;
    return 0;
}

int afp_list(afp_session *s, uint16_t vol, uint32_t dir, afp_entry **entries, int *count)
{
    RETRYING(s, list(s, vol, dir, entries, count));
}

static int stat_entry(afp_session *s, uint16_t vol, uint32_t dir, const char *name, afp_entry *out)
{
    block k = { .n = 0 };
    p8(&k, FPGetFileDirParms);
    p8(&k, 0);
    p16(&k, vol);
    p32(&k, dir);
    p16(&k, FILE_BITMAP);
    p16(&k, DIR_BITMAP);
    ppath(&k, name);
    int n, r = call(s, &k, &n);
    if (r != 0)
        return r;
    if (n < 6)
        return kFPMiscErr;
    bool isdir = (s->buf[4] & 0x80) != 0;
    if (!parse_params(s->buf + 6, n - 6, isdir, isdir ? g16(s->buf + 2) : g16(s->buf), out))
        return kFPMiscErr;
    return 0;
}

int afp_stat(afp_session *s, uint16_t vol, uint32_t dir, const char *name, afp_entry *out)
{
    if (!name_ok(name))
        return kFPParmErr;
    RETRYING(s, stat_entry(s, vol, dir, name, out));
}

/* ---------------------------------------------------------------------- */
/* Forks */

static int open_fork(afp_session *s, uint16_t vol, uint32_t dir, const char *name, bool rsrc, int mode,
                     uint16_t *ref, uint32_t *len)
{
    block k = { .n = 0 };
    p8(&k, FPOpenFork);
    p8(&k, rsrc ? 0x80 : 0);
    p16(&k, vol);
    p32(&k, dir);
    p16(&k, rsrc ? B_RSRCLEN : B_DATALEN);
    p16(&k, mode);
    ppath(&k, name);
    int n, r = call(s, &k, &n);
    if (r != 0)
        return r;
    if (n < 8)
        return kFPMiscErr;
    *ref = g16(s->buf + 2);
    *len = g32(s->buf + 4);
    return 0;
}

static int close_fork(afp_session *s, uint16_t ref)
{
    block k = { .n = 0 };
    p8(&k, FPCloseFork);
    p8(&k, 0);
    p16(&k, ref);
    return call(s, &k, NULL);
}

static int read_fork(afp_session *s, uint16_t vol, uint32_t dir, const char *name, bool rsrc, uint8_t **data,
                     size_t *len)
{
    *data = NULL;
    *len = 0;
    uint16_t ref;
    uint32_t size;
    int r = open_fork(s, vol, dir, name, rsrc, 0x01, &ref, &size);
    if (r != 0)
        return r;
    uint8_t *out = malloc(size ? size : 1);
    if (!out) {
        close_fork(s, ref);
        return kFPMiscErr;
    }
    uint32_t got = 0;
    while (got < size) {
        uint32_t want = size - got;
        if (want > (uint32_t)s->quantum)
            want = (uint32_t)s->quantum;
        block k = { .n = 0 };
        p8(&k, FPRead);
        p8(&k, 0);
        p16(&k, ref);
        p32(&k, got);
        p32(&k, want);
        p8(&k, 0);
        p8(&k, 0);
        int n;
        r = call(s, &k, &n);
        if (r != 0 && r != kFPEOFErr)
            break;
        if ((uint32_t)n > want)
            n = (int)want;
        memcpy(out + got, s->buf, (size_t)n);
        got += (uint32_t)n;
        if (r == kFPEOFErr || !n) {
            r = 0;
            break;
        }
    }
    if (r == kFPSessClosed) {
        free(out);
        return r;
    }
    int c = close_fork(s, ref);
    if (r == 0)
        r = c;
    if (r != 0) {
        free(out);
        return r;
    }
    *data = out;
    *len = got;
    return 0;
}

int afp_read_fork(afp_session *s, uint16_t vol, uint32_t dir, const char *name, bool rsrc, uint8_t **data,
                  size_t *len)
{
    if (!name_ok(name))
        return kFPParmErr;
    RETRYING(s, read_fork(s, vol, dir, name, rsrc, data, len));
}

static int write_fork(afp_session *s, uint16_t vol, uint32_t dir, const char *name, bool rsrc, const uint8_t *data,
                      size_t len)
{
    if (len > 0x7FFFFFFF)
        return kFPParmErr;
    uint16_t ref;
    uint32_t old;
    int r = open_fork(s, vol, dir, name, rsrc, 0x03, &ref, &old);
    if (r != 0)
        return r;
    /* the largest piece of data a write's request has room for */
    int chunk = s->tcp ? (int)(s->server_quantum - 64 < MAX_CHUNK ? s->server_quantum - 64 : MAX_CHUNK)
                       : ASP_QUANTUM;
    for (size_t done = 0; done < len && r == 0;) {
        int n = len - done < (size_t)chunk ? (int)(len - done) : chunk;
        block k = { .n = 0 };
        p8(&k, FPWrite);
        p8(&k, 0);
        p16(&k, ref);
        p32(&k, (uint32_t)done);
        p32(&k, (uint32_t)n);
        int rl;
        r = call_write(s, &k, data + done, n, &rl);
        if (r == 0) {
            uint32_t last = rl >= 4 ? g32(s->buf) : (uint32_t)(done + (size_t)n);
            if (last <= done)
                r = kFPMiscErr;
            else
                done = last;
        }
    }
    if (r == 0) {
        block k = { .n = 0 };
        p8(&k, FPSetForkParms);
        p8(&k, 0);
        p16(&k, ref);
        p16(&k, rsrc ? B_RSRCLEN : B_DATALEN);
        p32(&k, (uint32_t)len);
        r = call(s, &k, NULL);
    }
    if (r == kFPSessClosed)
        return r;
    int c = close_fork(s, ref);
    return r ? r : c;
}

int afp_write_fork(afp_session *s, uint16_t vol, uint32_t dir, const char *name, bool rsrc, const uint8_t *data,
                   size_t len)
{
    if (!name_ok(name))
        return kFPParmErr;
    RETRYING(s, write_fork(s, vol, dir, name, rsrc, data, len));
}

/* ---------------------------------------------------------------------- */
/* Changes */

static int set_finder_info(afp_session *s, uint16_t vol, uint32_t dir, const char *name, const uint8_t info[32])
{
    block k = { .n = 0 };
    p8(&k, FPSetFileDirParms);
    p8(&k, 0);
    p16(&k, vol);
    p32(&k, dir);
    p16(&k, B_FINDER);
    ppath(&k, name);
    pad(&k);
    memcpy(k.b + k.n, info, 32);
    k.n += 32;
    return call(s, &k, NULL);
}

int afp_set_finder_info(afp_session *s, uint16_t vol, uint32_t dir, const char *name, const uint8_t info[32])
{
    if (!name_ok(name))
        return kFPParmErr;
    RETRYING(s, set_finder_info(s, vol, dir, name, info));
}

/* A call on a name in a directory: create, delete. */
static int name_call(afp_session *s, int cmd, int flag, uint16_t vol, uint32_t dir, const char *name, uint32_t *newid)
{
    block k = { .n = 0 };
    p8(&k, cmd);
    p8(&k, flag);
    p16(&k, vol);
    p32(&k, dir);
    ppath(&k, name);
    int n, r = call(s, &k, &n);
    if (r == 0 && newid)
        *newid = n >= 4 ? g32(s->buf) : 0;
    return r;
}

int afp_create_file(afp_session *s, uint16_t vol, uint32_t dir, const char *name)
{
    if (!name_ok(name) || !name[0])
        return kFPParmErr;
    RETRYING(s, name_call(s, FPCreateFile, 0, vol, dir, name, NULL));
}

int afp_create_dir(afp_session *s, uint16_t vol, uint32_t dir, const char *name, uint32_t *newid)
{
    uint32_t id = 0;
    if (!name_ok(name) || !name[0])
        return kFPParmErr;
    if (!newid)
        newid = &id;
    RETRYING(s, name_call(s, FPCreateDir, 0, vol, dir, name, newid));
}

int afp_delete(afp_session *s, uint16_t vol, uint32_t dir, const char *name)
{
    if (!name_ok(name))
        return kFPParmErr;
    RETRYING(s, name_call(s, FPDelete, 0, vol, dir, name, NULL));
}

static int move(afp_session *s, uint16_t vol, uint32_t dir, const char *name, uint32_t to_dir, const char *new_name)
{
    block k = { .n = 0 };
    if (to_dir == dir) {
        p8(&k, FPRename);
        p8(&k, 0);
        p16(&k, vol);
        p32(&k, dir);
        ppath(&k, name);
        ppath(&k, new_name);
    } else {
        p8(&k, FPMoveAndRename);
        p8(&k, 0);
        p16(&k, vol);
        p32(&k, dir);
        p32(&k, to_dir);
        ppath(&k, name);
        ppath(&k, "");
        ppath(&k, strcmp(new_name, name) ? new_name : "");
    }
    return call(s, &k, NULL);
}

int afp_move(afp_session *s, uint16_t vol, uint32_t dir, const char *name, uint32_t to_dir, const char *new_name)
{
    if (!new_name || !new_name[0])
        new_name = name;
    if (!name_ok(name) || !name_ok(new_name) || !name[0])
        return kFPParmErr;
    if (to_dir == dir && !strcmp(name, new_name))
        return 0;
    RETRYING(s, move(s, vol, dir, name, to_dir, new_name));
}
