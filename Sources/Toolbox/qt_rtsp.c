/*
 * QuickTime streaming's network side, as QuickTime 4 and 5 received it:
 * session descriptions (SDP), the RTSP client that asks a server for a
 * presentation, and RTP from it, interleaved on the RTSP connection (TCP,
 * as QuickTime's "streaming through firewalls" setting did) or on the UDP
 * ports a description names (multicast included).  The payloads we turn
 * into sound and pictures are the audio/video profile's linear, mu-law,
 * A-law and DVI formats, JPEG (RFC 2435), and QuickTime's own payload
 * (X-QT), which carries a track's sample description and its samples for
 * our classic codecs.  The rest (H.263, MPEG, Sorenson, QDesign) have no
 * decoder here: their streams are received but not shown.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "qt_codec.h"
#include "qt_stream.h"

/* ---- SDP ---- */

static void resolve(const char *base, const char *ctl, char *out, size_t n)
{
    if (!ctl[0] || !strcmp(ctl, "*")) {
        snprintf(out, n, "%s", base ? base : "");
        return;
    }
    if (strstr(ctl, "://") || !base || !base[0]) {
        snprintf(out, n, "%s", ctl);
        return;
    }
    bool slash = base[strlen(base) - 1] == '/';
    if (slash && ctl[0] == '/')
        ctl++;
    snprintf(out, n, "%s%s%s", base, slash || ctl[0] == '/' ? "" : "/", ctl);
}

static uint32_t media_type_of(const char *name)
{
    if (!strncmp(name, "video", 5))
        return fourcc("vide");
    if (!strncmp(name, "audio", 5))
        return fourcc("soun");
    if (!strncmp(name, "text", 4))
        return fourcc("text");
    char b[4] = { ' ', ' ', ' ', ' ' };
    for (int i = 0; i < 4 && name[i] && name[i] != ' '; i++)
        b[i] = name[i];
    return fourcc(b);
}

/* The audio/video profile's static payload types (RFC 3551). */
static void static_payload(qts_sdp_media *m)
{
    static const struct { int pt; const char *name; int clock, ch; } t[] = {
        { 0, "PCMU", 8000, 1 },  { 3, "GSM", 8000, 1 },    { 5, "DVI4", 8000, 1 },   { 6, "DVI4", 16000, 1 },
        { 8, "PCMA", 8000, 1 },  { 10, "L16", 44100, 2 },  { 11, "L16", 44100, 1 },  { 14, "MPA", 90000, 1 },
        { 16, "DVI4", 11025, 1 }, { 17, "DVI4", 22050, 1 }, { 26, "JPEG", 90000, 0 }, { 31, "H261", 90000, 0 },
        { 32, "MPV", 90000, 0 }, { 34, "H263", 90000, 0 },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (t[i].pt == m->pt) {
            snprintf(m->encoding, sizeof m->encoding, "%s", t[i].name);
            m->clock = t[i].clock;
            m->channels = t[i].ch;
        }
}

bool qts_sdp_parse(const char *text, size_t len, const char *base, qts_sdp *out)
{
    memset(out, 0, sizeof *out);
    char session_addr[64] = "", session_ctl[512] = "";
    bool any = false;
    qts_sdp_media *m = NULL;
    size_t i = 0;
    while (i < len) {
        size_t e = i;
        while (e < len && text[e] != '\n' && text[e] != '\r')
            e++;
        char line[1024];
        size_t n = e - i < sizeof line - 1 ? e - i : sizeof line - 1;
        memcpy(line, text + i, n);
        line[n] = 0;
        i = e;
        while (i < len && (text[i] == '\n' || text[i] == '\r'))
            i++;
        if (n < 2 || line[1] != '=')
            continue;
        const char *v = line + 2;
        switch (line[0]) {
        case 'v':
            any = true;
            break;
        case 's':
            if (!m)
                snprintf(out->name, sizeof out->name, "%s", v);
            break;
        case 'c': {
            char a[64];
            if (sscanf(v, "IN IP4 %63[^/ ]", a) == 1)
                snprintf(m ? m->addr : session_addr, 64, "%s", a);
            break;
        }
        case 'm': {
            if (out->nmedia >= QTS_MAX_MEDIA) {
                m = &out->m[QTS_MAX_MEDIA - 1]; /* further media ignored */
                break;
            }
            m = &out->m[out->nmedia++];
            char name[32], proto[32];
            if (sscanf(v, "%31s %d %31s %d", name, &m->port, proto, &m->pt) < 4)
                m->pt = -1;
            m->media_type = media_type_of(name);
            if (m->pt >= 0 && m->pt < 96)
                static_payload(m);
            any = true;
            break;
        }
        case 'a': {
            char ctl[512];
            int pt, a, b, c, d;
            if (!strncmp(v, "control:", 8)) {
                snprintf(ctl, sizeof ctl, "%s", v + 8);
                if (m)
                    snprintf(m->control, sizeof m->control, "%s", ctl);
                else
                    snprintf(session_ctl, sizeof session_ctl, "%s", ctl);
            } else if (!strncmp(v, "range:npt=", 10)) {
                const char *dash = strchr(v + 10, '-');
                if (dash && dash[1])
                    out->range_end = atof(dash + 1);
            } else if (m && sscanf(v, "rtpmap:%d", &pt) == 1 && pt == m->pt) {
                const char *s = strchr(v, ' ');
                if (s) {
                    char enc[32] = "";
                    int clock = 0, ch = 0;
                    sscanf(s + 1, "%31[^/]/%d/%d", enc, &clock, &ch);
                    snprintf(m->encoding, sizeof m->encoding, "%s", enc);
                    m->clock = clock;
                    m->channels = ch ? ch : m->media_type == fourcc("soun") ? 1 : 0;
                }
            } else if (m && sscanf(v, "fmtp:%d", &pt) == 1 && pt == m->pt) {
                const char *s = strchr(v, ' ');
                snprintf(m->fmtp, sizeof m->fmtp, "%s", s ? s + 1 : "");
            } else if (m && sscanf(v, "cliprect:%d,%d,%d,%d", &a, &b, &c, &d) == 4) {
                m->width = d - b;
                m->height = c - a;
            } else if (m && sscanf(v, "x-dimensions:%d,%d", &a, &b) == 2) {
                m->width = a;
                m->height = b;
            } else if (m && sscanf(v, "framesize:%d %d-%d", &pt, &a, &b) == 3) {
                m->width = a;
                m->height = b;
            }
            break;
        }
        }
    }
    snprintf(out->control, sizeof out->control, "%s", base ? base : "");
    if (session_ctl[0])
        resolve(base, session_ctl, out->control, sizeof out->control);
    for (int k = 0; k < out->nmedia; k++) {
        qts_sdp_media *x = &out->m[k];
        char abs[512];
        resolve(out->control, x->control, abs, sizeof abs);
        snprintf(x->control, sizeof x->control, "%s", abs);
        if (!x->addr[0])
            snprintf(x->addr, sizeof x->addr, "%s", session_addr);
    }
    return any && out->nmedia > 0;
}

/* ---- The connection ---- */

#define TIMEOUT_MS 8000

typedef struct {
    int media;
    uint8_t *data;
    size_t len;
} packet;

struct qts_net {
    bool rtsp;
    int fd;                      /* the RTSP connection */
    char url[512], base[512], session[128];
    int cseq;
    uint8_t *in;                 /* what has come on it, not yet taken */
    size_t inlen, incap;
    int channel_media[64];       /* interleaved channel -> medium, -1 none */
    packet *queue;
    int nqueue, capqueue;
    int udp[QTS_MAX_MEDIA];      /* RTP sockets (UDP), -1 none */
    int nmedia;
    char controls[QTS_MAX_MEDIA][512];
    char aggregate[512];
    uint32_t packets, bytes;
};

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static bool parse_url(const char *url, char *host, size_t hn, int *port)
{
    if (strncasecmp(url, "rtsp://", 7))
        return false;
    const char *h = url + 7, *e = h;
    while (*e && *e != '/' && *e != ':')
        e++;
    if (e == h || (size_t)(e - h) >= hn)
        return false;
    memcpy(host, h, (size_t)(e - h));
    host[e - h] = 0;
    *port = *e == ':' ? atoi(e + 1) : 554;
    return *port > 0 && *port < 65536;
}

static int connect_to(const char *host, int port)
{
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *res = NULL;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) || !res)
        return -1;
    int fd = -1;
    for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0)
            continue;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        int r = connect(fd, a->ai_addr, a->ai_addrlen);
        if (r && errno == EINPROGRESS) {
            fd_set w;
            FD_ZERO(&w);
            FD_SET(fd, &w);
            struct timeval tv = { TIMEOUT_MS / 1000, 0 };
            int err = 0;
            socklen_t el = sizeof err;
            r = select(fd + 1, NULL, &w, NULL, &tv) == 1 && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) && !err
                    ? 0 : -1;
        }
        if (r) {
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
#ifdef SO_NOSIGPIPE
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    }
#endif
    return fd;
}

static void enqueue(qts_net *n, int media, const uint8_t *d, size_t len)
{
    if (n->nqueue == n->capqueue) {
        int cap = n->capqueue ? n->capqueue * 2 : 64;
        packet *q = realloc(n->queue, (size_t)cap * sizeof *q);
        if (!q)
            return;
        n->queue = q;
        n->capqueue = cap;
    }
    uint8_t *b = malloc(len ? len : 1);
    if (!b)
        return;
    memcpy(b, d, len);
    n->queue[n->nqueue++] = (packet){ media, b, len };
    n->packets++;
    n->bytes += (uint32_t)len;
}

/* What is waiting on the connection, read without blocking (or waiting up
 * to wait_ms for something); false when it closed or failed. */
static bool fill(qts_net *n, int wait_ms)
{
    if (wait_ms > 0) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(n->fd, &r);
        struct timeval tv = { wait_ms / 1000, (wait_ms % 1000) * 1000 };
        if (select(n->fd + 1, &r, NULL, NULL, &tv) <= 0)
            return true;
    }
    for (;;) {
        if (n->incap - n->inlen < 4096) {
            size_t cap = n->incap ? n->incap * 2 : 65536;
            uint8_t *b = realloc(n->in, cap);
            if (!b)
                return false;
            n->in = b;
            n->incap = cap;
        }
        ssize_t r = recv(n->fd, n->in + n->inlen, n->incap - n->inlen, 0);
        if (r > 0) {
            n->inlen += (size_t)r;
            continue;
        }
        if (r == 0)
            return false;
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
    }
}

static void take(qts_net *n, size_t k)
{
    memmove(n->in, n->in + k, n->inlen - k);
    n->inlen -= k;
}

/* Interleaved packets at the front of the input queued; a whole response,
 * if one is there, returned (malloc'd, headers then body, NUL-terminated). */
static char *next_message(qts_net *n)
{
    for (;;) {
        if (n->inlen >= 4 && n->in[0] == '$') {
            size_t len = (size_t)n->in[2] << 8 | n->in[3];
            if (n->inlen < 4 + len)
                return NULL;
            int ch = n->in[1];
            if (ch < 64 && n->channel_media[ch] >= 0)
                enqueue(n, n->channel_media[ch], n->in + 4, len);
            take(n, 4 + len);
            continue;
        }
        if (!n->inlen)
            return NULL;
        /* a response: headers up to a blank line, then Content-Length bytes */
        uint8_t *end = NULL;
        for (size_t i = 0; i + 3 < n->inlen; i++)
            if (!memcmp(n->in + i, "\r\n\r\n", 4)) {
                end = n->in + i + 4;
                break;
            }
        if (!end) {
            if (n->in[0] != 'R') /* not a response: resynchronise on the next '$' */
                for (size_t i = 1; i <= n->inlen; i++)
                    if (i == n->inlen || n->in[i] == '$') {
                        take(n, i);
                        break;
                    }
            return NULL;
        }
        size_t head = (size_t)(end - n->in), body = 0;
        for (size_t i = 0; i < head; i++)
            if ((i == 0 || n->in[i - 1] == '\n') && !strncasecmp((char *)n->in + i, "Content-Length:", 15))
                body = (size_t)atol((char *)n->in + i + 15);
        if (n->inlen < head + body)
            return NULL;
        char *msg = malloc(head + body + 1);
        if (!msg)
            return NULL;
        memcpy(msg, n->in, head + body);
        msg[head + body] = 0;
        take(n, head + body);
        return msg;
    }
}

static bool header(const char *msg, const char *name, char *out, size_t n)
{
    size_t nl = strlen(name);
    for (const char *p = msg; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
        if (*p == '\r' || *p == '\n')
            break;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ')
                v++;
            size_t k = 0;
            while (v[k] && v[k] != '\r' && v[k] != '\n' && k < n - 1)
                k++;
            memcpy(out, v, k);
            out[k] = 0;
            return true;
        }
    }
    return false;
}

/* A request and its response (malloc'd), status in *status. */
static char *request(qts_net *n, const char *method, const char *url, const char *extra, int *status)
{
    char req[2048];
    int len = snprintf(req, sizeof req, "%s %s RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: QTS (qtver=4.1.2;os=Mac 8.6)\r\n%s%s%s%s\r\n",
                       method, url, ++n->cseq, n->session[0] ? "Session: " : "", n->session,
                       n->session[0] ? "\r\n" : "", extra ? extra : "");
    *status = 0;
    if (len <= 0 || len >= (int)sizeof req)
        return NULL;
    for (int sent = 0; sent < len;) {
        ssize_t w = send(n->fd, req + sent, (size_t)(len - sent), 0);
        if (w < 0 && (errno == EAGAIN || errno == EINTR)) {
            fd_set wf;
            FD_ZERO(&wf);
            FD_SET(n->fd, &wf);
            struct timeval tv = { 1, 0 };
            select(n->fd + 1, NULL, &wf, NULL, &tv);
            continue;
        }
        if (w <= 0)
            return NULL;
        sent += (int)w;
    }
    int64_t until = now_ms() + TIMEOUT_MS;
    for (;;) {
        char *msg = next_message(n);
        if (msg) {
            char cs[16];
            if (header(msg, "CSeq", cs, sizeof cs) && atoi(cs) != n->cseq) { /* an old one */
                free(msg);
                continue;
            }
            sscanf(msg, "RTSP/%*d.%*d %d", status);
            return msg;
        }
        int64_t left = until - now_ms();
        if (left <= 0 || !fill(n, (int)(left < 200 ? left : 200)))
            return NULL;
    }
}

static qts_net *net_new(void)
{
    qts_net *n = calloc(1, sizeof *n);
    if (!n)
        return NULL;
    n->fd = -1;
    for (int i = 0; i < 64; i++)
        n->channel_media[i] = -1;
    for (int i = 0; i < QTS_MAX_MEDIA; i++)
        n->udp[i] = -1;
    return n;
}

qts_net *qts_net_rtsp(const char *url, char **sdp, int16_t *err)
{
    char host[256];
    int port;
    *sdp = NULL;
    if (!parse_url(url, host, sizeof host, &port)) {
        *err = qtsBadDataErr;
        return NULL;
    }
    qts_net *n = net_new();
    if (!n) {
        *err = -108;
        return NULL;
    }
    n->rtsp = true;
    snprintf(n->url, sizeof n->url, "%s", url);
    n->fd = connect_to(host, port);
    if (n->fd < 0) {
        free(n);
        *err = qtsConnectionFailedErr;
        return NULL;
    }
    int status;
    char *msg = request(n, "DESCRIBE", url, "Accept: application/sdp\r\n", &status);
    if (!msg || status != 200) {
        free(msg);
        *err = !msg ? qtsTimeoutErr : status == 404 ? -43 /* fnfErr */ : qtsConnectionFailedErr;
        qts_net_close(n);
        return NULL;
    }
    if (!header(msg, "Content-Base", n->base, sizeof n->base) &&
        !header(msg, "Content-Location", n->base, sizeof n->base))
        snprintf(n->base, sizeof n->base, "%s", url);
    const char *body = strstr(msg, "\r\n\r\n");
    *sdp = strdup(body ? body + 4 : "");
    free(msg);
    *err = 0;
    return n;
}

int16_t qts_net_setup(qts_net *n, const qts_sdp *sdp)
{
    n->nmedia = sdp->nmedia;
    snprintf(n->aggregate, sizeof n->aggregate, "%s", sdp->control[0] ? sdp->control : n->url);
    if (!n->rtsp)
        return 0;
    for (int i = 0; i < sdp->nmedia; i++) {
        char extra[256];
        snprintf(extra, sizeof extra, "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n", 2 * i, 2 * i + 1);
        snprintf(n->controls[i], sizeof n->controls[i], "%s", sdp->m[i].control);
        int status;
        char *msg = request(n, "SETUP", sdp->m[i].control[0] ? sdp->m[i].control : n->url, extra, &status);
        if (!msg || status != 200) {
            free(msg);
            return msg ? (status == 461 ? qtsUnsupportedFeatureErr : qtsConnectionFailedErr) : qtsTimeoutErr;
        }
        char v[256];
        if (!n->session[0] && header(msg, "Session", v, sizeof v)) {
            char *semi = strchr(v, ';');
            if (semi)
                *semi = 0;
            snprintf(n->session, sizeof n->session, "%s", v);
        }
        int a = 2 * i, b = 2 * i + 1;
        if (header(msg, "Transport", v, sizeof v)) {
            const char *il = strstr(v, "interleaved=");
            if (il)
                sscanf(il + 12, "%d-%d", &a, &b);
        }
        if (a >= 0 && a < 64)
            n->channel_media[a] = i;
        free(msg);
    }
    return 0;
}

int16_t qts_net_play(qts_net *n, double from)
{
    if (!n->rtsp)
        return 0;
    char extra[64] = "";
    if (from >= 0)
        snprintf(extra, sizeof extra, "Range: npt=%.3f-\r\n", from);
    int status;
    char *msg = request(n, "PLAY", n->aggregate, extra, &status);
    free(msg);
    return !msg ? qtsTimeoutErr : status == 200 ? 0 : status == 457 ? qtsBadDataErr : qtsConnectionFailedErr;
}

int16_t qts_net_pause(qts_net *n)
{
    if (!n->rtsp)
        return 0;
    int status;
    char *msg = request(n, "PAUSE", n->aggregate, NULL, &status);
    free(msg);
    return !msg ? qtsTimeoutErr : status == 200 ? 0 : qtsConnectionFailedErr;
}

void qts_net_close(qts_net *n)
{
    if (!n)
        return;
    if (n->rtsp && n->fd >= 0 && n->session[0]) {
        /* TEARDOWN without waiting long for its answer */
        char req[1024];
        int len = snprintf(req, sizeof req, "TEARDOWN %s RTSP/1.0\r\nCSeq: %d\r\nSession: %s\r\n\r\n",
                           n->aggregate[0] ? n->aggregate : n->url, ++n->cseq, n->session);
        if (len > 0 && len < (int)sizeof req)
            (void)!send(n->fd, req, (size_t)len, 0);
    }
    if (n->fd >= 0)
        close(n->fd);
    for (int i = 0; i < QTS_MAX_MEDIA; i++)
        if (n->udp[i] >= 0)
            close(n->udp[i]);
    for (int i = 0; i < n->nqueue; i++)
        free(n->queue[i].data);
    free(n->queue);
    free(n->in);
    free(n);
}

qts_net *qts_net_udp(const qts_sdp *sdp, int16_t *err)
{
    qts_net *n = net_new();
    if (!n) {
        *err = -108;
        return NULL;
    }
    n->nmedia = sdp->nmedia;
    for (int i = 0; i < sdp->nmedia; i++) {
        const qts_sdp_media *m = &sdp->m[i];
        if (m->port <= 0)
            continue;
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0)
            continue;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
        struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((uint16_t)m->port) };
        struct in_addr group;
        bool multicast = inet_aton(m->addr, &group) && (ntohl(group.s_addr) >> 28) == 14;
        sa.sin_addr.s_addr = multicast ? group.s_addr : htonl(INADDR_ANY);
        if (bind(fd, (struct sockaddr *)&sa, sizeof sa)) {
            close(fd);
            qts_net_close(n);
            *err = qtsAddressBusyErr;
            return NULL;
        }
        if (multicast) {
            struct ip_mreq mr = { .imr_multiaddr = group, .imr_interface = { htonl(INADDR_ANY) } };
            setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr);
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        n->udp[i] = fd;
    }
    *err = 0;
    return n;
}

int qts_net_poll(qts_net *n, qts_packet_fn fn, void *ctx)
{
    if (n->rtsp && n->fd >= 0) {
        fill(n, 0);
        char *msg;
        while ((msg = next_message(n))) /* answers nobody waits for (keep-alives) */
            free(msg);
    }
    uint8_t buf[65536];
    for (int i = 0; i < QTS_MAX_MEDIA; i++)
        if (n->udp[i] >= 0)
            for (int k = 0; k < 256; k++) {
                ssize_t r = recv(n->udp[i], buf, sizeof buf, 0);
                if (r <= 0)
                    break;
                enqueue(n, i, buf, (size_t)r);
            }
    int count = n->nqueue;
    for (int i = 0; i < n->nqueue; i++) {
        if (fn)
            fn(ctx, n->queue[i].media, n->queue[i].data, n->queue[i].len);
        free(n->queue[i].data);
    }
    n->nqueue = 0;
    return count;
}

void qts_net_counts(const qts_net *n, uint32_t *packets, uint32_t *bytes)
{
    *packets = n->packets;
    *bytes = n->bytes;
}

bool qts_net_rtsp_session(const qts_net *n) { return n->rtsp; }

/* ---- Payloads ---- */

enum { P_NONE, P_L8, P_L16, P_PCMU, P_PCMA, P_DVI4, P_JPEG, P_XQT };

struct qts_depay {
    int kind;
    int clock, channels;
    uint32_t media;
    /* a unit being gathered from packets (JPEG, X-QT) */
    uint8_t *buf;
    size_t len, cap;
    uint32_t time;
    bool gathering;
    int jtype, jq, jw, jh, jdri;
    uint8_t jq_tables[128];
    int jq_len;
    /* X-QT: the payload's description and decoders */
    uint8_t *desc;
    uint32_t desc_len;
    uint32_t desc_media;
    int32_t desc_scale;
    qt_vdec *vdec;
    qt_video_format vf;
    uint8_t *frame;
    qt_sound_format sf;
};

static int kind_of(const qts_sdp_media *m)
{
    const char *e = m->encoding;
    if (!strcasecmp(e, "L8"))
        return P_L8;
    if (!strcasecmp(e, "L16"))
        return P_L16;
    if (!strcasecmp(e, "PCMU"))
        return P_PCMU;
    if (!strcasecmp(e, "PCMA"))
        return P_PCMA;
    if (!strcasecmp(e, "DVI4"))
        return P_DVI4;
    if (!strcasecmp(e, "JPEG"))
        return P_JPEG;
    if (!strcasecmp(e, "X-QT") || !strcasecmp(e, "X-QUICKTIME"))
        return P_XQT;
    return P_NONE;
}

bool qts_depay_supported(const qts_sdp_media *m) { return kind_of(m) != P_NONE; }

qts_depay *qts_depay_new(const qts_sdp_media *m)
{
    int k = kind_of(m);
    if (k == P_NONE)
        return NULL;
    qts_depay *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->kind = k;
    d->clock = m->clock > 0 ? m->clock : 8000;
    d->channels = m->channels > 0 ? m->channels : 1;
    d->media = k == P_JPEG ? fourcc("vide") : k == P_XQT ? 0 : fourcc("soun");
    return d;
}

void qts_depay_free(qts_depay *d)
{
    if (!d)
        return;
    if (d->vdec)
        qt_vdec_free(d->vdec);
    free(d->frame);
    free(d->desc);
    free(d->buf);
    free(d);
}

uint32_t qts_depay_media(const qts_depay *d) { return d->media; }

static int16_t ulaw(uint8_t u)
{
    u = (uint8_t)~u;
    int t = ((u & 0x0F) << 3) + 0x84;
    t <<= (u & 0x70) >> 4;
    return (int16_t)(u & 0x80 ? 0x84 - t : t - 0x84);
}

static int16_t alaw(uint8_t a)
{
    a ^= 0x55;
    int t = (a & 0x0F) << 4, seg = (a & 0x70) >> 4;
    if (seg == 0)
        t += 8;
    else if (seg == 1)
        t += 0x108;
    else
        t = (t + 0x108) << (seg - 1);
    return (int16_t)(a & 0x80 ? t : -t);
}

static const int16_t dvi_steps[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
static const int8_t dvi_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

/* DVI4 (RFC 3551 4.5.1): a 4-byte state (predicted value, step index) per
 * channel, then samples, four bits each, high nibble first. */
static uint32_t dvi4(const uint8_t *p, size_t n, int16_t **pcm)
{
    if (n < 4)
        return 0;
    int pred = (int16_t)(p[0] << 8 | p[1]), index = p[2] > 88 ? 88 : p[2];
    size_t frames = (n - 4) * 2;
    int16_t *out = malloc(frames * sizeof *out);
    if (!out)
        return 0;
    for (size_t i = 0; i < frames; i++) {
        int nib = i & 1 ? p[4 + i / 2] & 15 : p[4 + i / 2] >> 4;
        int step = dvi_steps[index], diff = step >> 3;
        if (nib & 4)
            diff += step;
        if (nib & 2)
            diff += step >> 1;
        if (nib & 1)
            diff += step >> 2;
        pred += nib & 8 ? -diff : diff;
        pred = pred < -32768 ? -32768 : pred > 32767 ? 32767 : pred;
        index += dvi_index[nib];
        index = index < 0 ? 0 : index > 88 ? 88 : index;
        out[i] = (int16_t)pred;
    }
    *pcm = out;
    return (uint32_t)frames;
}

static bool append(qts_depay *d, const uint8_t *p, size_t n, size_t at)
{
    if (at + n > d->cap) {
        size_t cap = d->cap ? d->cap : 65536;
        while (cap < at + n)
            cap *= 2;
        if (cap > 64 * 1024 * 1024)
            return false;
        uint8_t *b = realloc(d->buf, cap);
        if (!b)
            return false;
        d->buf = b;
        d->cap = cap;
    }
    memcpy(d->buf + at, p, n);
    if (at + n > d->len)
        d->len = at + n;
    return true;
}

/* ---- JPEG (RFC 2435): the frame's headers made from the payload's ---- */

static const uint8_t luma_q[64] = {
    16, 11, 12, 14, 12, 10, 16, 14, 13, 14, 18, 17, 16, 19, 24, 40, 26, 24, 22, 22, 24, 49,
    35, 37, 29, 40, 58, 51, 61, 60, 57, 51, 56, 55, 64, 72, 92, 78, 64, 68, 87, 69, 55, 56,
    80, 109, 81, 87, 95, 98, 103, 104, 103, 62, 77, 113, 121, 112, 100, 120, 92, 101, 103, 99,
};
static const uint8_t chroma_q[64] = {
    17, 18, 18, 24, 21, 24, 47, 26, 26, 47, 99, 66, 56, 66, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
};
static const uint8_t lum_dc_lens[16] = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t lum_ac_lens[16] = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const uint8_t chm_dc_lens[16] = { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const uint8_t chm_ac_lens[16] = { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const uint8_t dc_symbols[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t lum_ac_symbols[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
    0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};
static const uint8_t chm_ac_symbols[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22,
    0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
    0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36,
    0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
    0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
    0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
    0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

static uint8_t *put_huffman(uint8_t *p, const uint8_t *lens, const uint8_t *syms, int nsyms, int class_id)
{
    *p++ = 0xFF;
    *p++ = 0xC4;
    *p++ = 0;
    *p++ = (uint8_t)(3 + 16 + nsyms);
    *p++ = (uint8_t)class_id;
    memcpy(p, lens, 16);
    p += 16;
    memcpy(p, syms, (size_t)nsyms);
    return p + nsyms;
}

uint8_t *qts_jpeg_frame(int type, int q, int w, int h, const uint8_t *qtables, int qlen, int dri,
                        const uint8_t *scan, size_t scan_len, size_t *out_len)
{
    uint8_t lq[64], cq[64];
    if (q >= 128) {
        if (!qtables || qlen < 64)
            return NULL;
        memcpy(lq, qtables, 64);
        memcpy(cq, qlen >= 128 ? qtables + 64 : qtables, 64);
    } else {
        int f = q < 1 ? 5000 : q < 50 ? 5000 / q : 200 - q * 2;
        for (int i = 0; i < 64; i++) {
            int a = (luma_q[i] * f + 50) / 100, b = (chroma_q[i] * f + 50) / 100;
            lq[i] = (uint8_t)(a < 1 ? 1 : a > 255 ? 255 : a);
            cq[i] = (uint8_t)(b < 1 ? 1 : b > 255 ? 255 : b);
        }
    }
    uint8_t *out = malloc(1024 + scan_len);
    if (!out)
        return NULL;
    uint8_t *p = out;
    *p++ = 0xFF, *p++ = 0xD8;
    for (int t = 0; t < 2; t++) {
        *p++ = 0xFF, *p++ = 0xDB, *p++ = 0, *p++ = 67, *p++ = (uint8_t)t;
        memcpy(p, t ? cq : lq, 64);
        p += 64;
    }
    if (dri) {
        *p++ = 0xFF, *p++ = 0xDD, *p++ = 0, *p++ = 4, *p++ = (uint8_t)(dri >> 8), *p++ = (uint8_t)dri;
    }
    *p++ = 0xFF, *p++ = 0xC0, *p++ = 0, *p++ = 17, *p++ = 8;
    *p++ = (uint8_t)(h >> 8), *p++ = (uint8_t)h, *p++ = (uint8_t)(w >> 8), *p++ = (uint8_t)w, *p++ = 3;
    *p++ = 0, *p++ = (type & 63) == 0 ? 0x21 : 0x22, *p++ = 0;
    *p++ = 1, *p++ = 0x11, *p++ = 1;
    *p++ = 2, *p++ = 0x11, *p++ = 1;
    p = put_huffman(p, lum_dc_lens, dc_symbols, 12, 0x00);
    p = put_huffman(p, lum_ac_lens, lum_ac_symbols, 162, 0x10);
    p = put_huffman(p, chm_dc_lens, dc_symbols, 12, 0x01);
    p = put_huffman(p, chm_ac_lens, chm_ac_symbols, 162, 0x11);
    *p++ = 0xFF, *p++ = 0xDA, *p++ = 0, *p++ = 12, *p++ = 3;
    *p++ = 0, *p++ = 0x00, *p++ = 1, *p++ = 0x11, *p++ = 2, *p++ = 0x11;
    *p++ = 0, *p++ = 63, *p++ = 0;
    memcpy(p, scan, scan_len);
    p += scan_len;
    if (scan_len < 2 || scan[scan_len - 2] != 0xFF || scan[scan_len - 1] != 0xD9)
        *p++ = 0xFF, *p++ = 0xD9;
    *out_len = (size_t)(p - out);
    return out;
}

static int jpeg_packet(qts_depay *d, const uint8_t *p, size_t n, bool marker, uint32_t ts, qts_unit *out)
{
    if (n < 8)
        return 0;
    uint32_t off = (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    int type = p[4], q = p[5], w = p[6] * 8, h = p[7] * 8;
    size_t at = 8;
    int dri = 0;
    if (type >= 64 && type < 128) {
        if (n < at + 4)
            return 0;
        dri = p[at] << 8 | p[at + 1];
        at += 4;
    }
    if (q >= 128 && off == 0) {
        if (n < at + 4)
            return 0;
        int len = p[at + 2] << 8 | p[at + 3];
        if (n < at + 4 + (size_t)len)
            return 0;
        d->jq_len = len < 128 ? len : 128;
        memcpy(d->jq_tables, p + at + 4, (size_t)d->jq_len);
        at += 4 + (size_t)len;
    }
    if (off == 0) {
        d->len = 0;
        d->gathering = true;
        d->time = ts;
        d->jtype = type, d->jq = q, d->jw = w, d->jh = h, d->jdri = dri;
    }
    if (!d->gathering || ts != d->time || !append(d, p + at, n - at, off))
        return 0;
    if (!marker)
        return 0;
    d->gathering = false;
    size_t jl;
    uint8_t *j = qts_jpeg_frame(d->jtype, d->jq, d->jw, d->jh, d->jq_tables, d->jq_len, d->jdri, d->buf, d->len, &jl);
    if (!j)
        return 0;
    bool ok = qt_decode_image(j, jl, &out->rgba, &out->w, &out->h);
    free(j);
    out->time = ts;
    return ok ? 1 : 0;
}

/* ---- QuickTime's own payload ---- */

static void xqt_description(qts_depay *d, const uint8_t *desc, uint32_t len)
{
    if (d->desc && d->desc_len == len && !memcmp(d->desc, desc, len))
        return;
    free(d->desc);
    d->desc = malloc(len);
    if (!d->desc)
        return;
    memcpy(d->desc, desc, len);
    d->desc_len = len;
    if (d->vdec) {
        qt_vdec_free(d->vdec);
        d->vdec = NULL;
    }
    free(d->frame);
    d->frame = NULL;
    if (len < 16)
        return;
    uint32_t format = (uint32_t)desc[4] << 24 | (uint32_t)desc[5] << 16 | (uint32_t)desc[6] << 8 | desc[7];
    if (d->desc_media == fourcc("vide") && len >= 86) {
        d->vf = (qt_video_format){ format, desc[32] << 8 | desc[33], desc[34] << 8 | desc[35], desc[82] << 8 | desc[83],
                                   NULL };
        if (format != fourcc("jpeg") && qt_vdec_supported(format, d->vf.depth) && d->vf.width > 0 &&
            d->vf.height > 0 && d->vf.width <= 4096 && d->vf.height <= 4096) {
            d->vdec = qt_vdec_new(&d->vf);
            d->frame = calloc((size_t)d->vf.width * (size_t)d->vf.height, 4);
        }
    } else if (d->desc_media == fourcc("soun") && len >= 36) {
        d->sf = (qt_sound_format){ format, desc[24] << 8 | desc[25], desc[26] << 8 | desc[27],
                                   (double)((uint32_t)desc[32] << 24 | (uint32_t)desc[33] << 16 |
                                            (uint32_t)desc[34] << 8 | desc[35]) / 65536.0 };
        if ((format == fourcc("raw ") || format == fourcc("twos") || format == fourcc("sowt")) && d->sf.bits != 16)
            d->sf.bits = 8;
    }
}

/* One sample of the description's media, decoded. */
static int xqt_sample(qts_depay *d, const uint8_t *s, size_t n, uint32_t ts, qts_unit *out)
{
    out->time = ts;
    if (d->desc_media == fourcc("vide")) {
        if (d->vf.format == fourcc("jpeg"))
            return qt_decode_image(s, n, &out->rgba, &out->w, &out->h) ? 1 : 0;
        if (!d->vdec || !qt_vdec_decode(d->vdec, s, n, d->frame))
            return 0;
        size_t bytes = (size_t)d->vf.width * (size_t)d->vf.height * 4;
        out->rgba = malloc(bytes);
        if (!out->rgba)
            return 0;
        memcpy(out->rgba, d->frame, bytes);
        out->w = d->vf.width;
        out->h = d->vf.height;
        return 1;
    }
    if (d->desc_media == fourcc("soun") && qt_adec_supported(&d->sf)) {
        out->frames = qt_adec_decode(&d->sf, s, n, &out->pcm);
        out->channels = d->sf.channels;
        out->rate = d->sf.rate;
        return out->frames ? 1 : 0;
    }
    return 0;
}

static int xqt_packet(qts_depay *d, const uint8_t *p, size_t n, bool marker, uint32_t ts, qts_unit *out)
{
    if (n < 4)
        return 0;
    bool q = p[0] & 0x08, packed = p[0] & 0x04;
    size_t at = 4;
    if (q) {
        if (n < at + 12)
            return 0;
        size_t dlen = (size_t)p[at + 2] << 8 | p[at + 3];
        if (dlen < 12 || at + dlen > n)
            return 0;
        d->desc_media = (uint32_t)p[at + 4] << 24 | (uint32_t)p[at + 5] << 16 | (uint32_t)p[at + 6] << 8 | p[at + 7];
        d->desc_scale = (int32_t)((uint32_t)p[at + 8] << 24 | (uint32_t)p[at + 9] << 16 | (uint32_t)p[at + 10] << 8 |
                                  p[at + 11]);
        d->media = d->desc_media;
        for (size_t t = at + 12; t + 4 <= at + dlen;) { /* TLVs: length (all of it), type, value */
            size_t tl = (size_t)p[t] << 8 | p[t + 1];
            if (tl < 4 || t + tl > at + dlen)
                break;
            if (p[t + 2] == 's' && p[t + 3] == 'd')
                xqt_description(d, p + t + 4, (uint32_t)(tl - 4));
            t += tl;
        }
        at += (dlen + 3) & ~(size_t)3;
    }
    if (!d->desc)
        return 0;
    if (packed) { /* samples each with their length and time; the first decoded, the rest kept for later packets */
        int got = 0;
        while (at + 8 <= n && !got) {
            size_t sl = (size_t)p[at] << 24 | (size_t)p[at + 1] << 16 | (size_t)p[at + 2] << 8 | p[at + 3];
            uint32_t st = (uint32_t)p[at + 4] << 24 | (uint32_t)p[at + 5] << 16 | (uint32_t)p[at + 6] << 8 | p[at + 7];
            if (at + 8 + sl > n)
                break;
            got = xqt_sample(d, p + at + 8, sl, st, out);
            at += 8 + ((sl + 3) & ~(size_t)3);
        }
        return got;
    }
    if (!d->gathering || ts != d->time) {
        d->len = 0;
        d->gathering = true;
        d->time = ts;
    }
    if (!append(d, p + at, n - at, d->len) || !marker)
        return 0;
    d->gathering = false;
    return xqt_sample(d, d->buf, d->len, ts, out);
}

int qts_depay_packet(qts_depay *d, const uint8_t *pkt, size_t len, qts_unit *out)
{
    memset(out, 0, sizeof *out);
    if (len < 12 || pkt[0] >> 6 != 2)
        return 0;
    size_t at = 12 + 4 * (size_t)(pkt[0] & 15);
    if (pkt[0] & 0x10) { /* an extension */
        if (len < at + 4)
            return 0;
        at += 4 + 4 * (size_t)(pkt[at + 2] << 8 | pkt[at + 3]);
    }
    if (pkt[0] & 0x20) { /* padding */
        size_t pad = pkt[len - 1];
        if (pad > len)
            return 0;
        len -= pad;
    }
    if (at > len)
        return 0;
    bool marker = pkt[1] & 0x80;
    uint32_t ts = (uint32_t)pkt[4] << 24 | (uint32_t)pkt[5] << 16 | (uint32_t)pkt[6] << 8 | pkt[7];
    const uint8_t *p = pkt + at;
    size_t n = len - at;
    out->time = ts;
    switch (d->kind) {
    case P_L8:
    case P_L16:
    case P_PCMU:
    case P_PCMA: {
        size_t bytes = d->kind == P_L16 ? 2 : 1;
        size_t frames = n / bytes / (size_t)d->channels;
        if (!frames)
            return 0;
        int16_t *pcm = malloc(frames * (size_t)d->channels * sizeof *pcm);
        if (!pcm)
            return 0;
        for (size_t i = 0; i < frames * (size_t)d->channels; i++)
            pcm[i] = d->kind == P_L16 ? (int16_t)(p[2 * i] << 8 | p[2 * i + 1])
                     : d->kind == P_L8 ? (int16_t)((p[i] - 128) * 256)
                     : d->kind == P_PCMU ? ulaw(p[i]) : alaw(p[i]);
        out->pcm = pcm;
        out->frames = (uint32_t)frames;
        out->channels = d->channels;
        out->rate = d->clock;
        return 1;
    }
    case P_DVI4:
        out->frames = dvi4(p, n, &out->pcm);
        out->channels = 1;
        out->rate = d->clock;
        return out->frames ? 1 : 0;
    case P_JPEG:
        return jpeg_packet(d, p, n, marker, ts, out);
    case P_XQT:
        return xqt_packet(d, p, n, marker, ts, out);
    }
    return 0;
}
