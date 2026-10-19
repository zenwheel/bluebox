/*
 * AppleTalk: the .MPP driver (DDP sockets and NBP) on LocalTalk over UDP.
 *
 * The link is LToUDP, as other emulators and AppleTalk routers speak it:
 * each LocalTalk (LLAP) frame goes to the UDP multicast group
 * 239.192.76.84, port 1954, after a 4-byte sender ID that lets a node
 * ignore its own frames.  Without --appletalk lan the group is joined on
 * the loopback interface only, so copies on this host play each other and
 * nothing leaves it.  The network is a single LocalTalk segment with no
 * router: network 0, nodes 1-254, short DDP headers.
 *
 * DDP datagrams for a socket go to its listener, 68k code called as the
 * .MPP driver called it: A2 the driver's variables with the read header
 * area (RHA) at toRHA, A3 just past the DDP header in it, A4 ReadPacket
 * (ReadRest 2 bytes on), D0 the socket, D1 the bytes left.
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "machine.h"
#include "tb_internal.h"
#include "trap.h"

enum { LTOUDP_PORT = 1954 };
#define LTOUDP_GROUP "239.192.76.84"
enum { LLAP_DDP_SHORT = 1, LLAP_DDP_LONG = 2, LLAP_ENQ = 0x81, LLAP_ACK = 0x82 };
enum { DDP_NBP = 2, NIS_SOCKET = 2, MAX_DATA = 586 };
enum { NBP_BRRQ = 1, NBP_LKUP = 2, NBP_LKUP_REPLY = 3 };
/* .MPP control codes */
enum {
    writeDDP = 246, closeSkt = 247, openSkt = 248, loadNBP = 249, confirmName = 250, lookupName = 251,
    removeName = 252, registerName = 253, killNBP = 254, unloadNBP = 255, setSelfSend = 256,
    SetMyZone = 257, GetATalkInfo = 258, ATalkClosePrep = 259,
};
enum {
    ddpSktErr = -91, ddpLenErr = -92, nbpBuffOvr = -1024, nbpNoConfirm = -1025, nbpConfDiff = -1026,
    nbpDuplicate = -1027, nbpNotFound = -1028, tooManyReqs = -1097, reqAborted = -1105, cbNotFound = -1102,
};
/* MPP parameter block, after csCode */
enum {
    PB_SOCKET = 28, PB_CHECKSUM = 29, PB_WDS = 30, PB_LISTENER = 30, /* a union */
    PB_INTERVAL = 28, PB_COUNT = 29, PB_ENTITY = 30, PB_VERIFY = 34,
    PB_RETBUFF = 34, PB_RETSIZE = 38, PB_MAXGET = 40, PB_GOTTEN = 42,
    PB_CONFADDR = 34, PB_NEWSOCKET = 38, PB_KILLQEL = 28, PB_NEWSELF = 28, PB_OLDSELF = 29,
};
/* The driver's variables as programs see them (ABusVars): this node at 0
 * (sysLAPAddr), the RHA at toRHA, the network number at $1A (Bolo reads
 * both directly). */
enum { SYS_LAP_ADDR = 0, TO_RHA = 1, SYS_NET_NUM = 0x1A, VARS_SIZE = 64 };

static int mode = 1; /* 0 off, 1 this host, 2 the local network, 3 EtherTalk (bridged) */
static uint16_t net;  /* this network's number: 0 on LocalTalk, from AARP and RTMP on EtherTalk */
static char ifname[16]; /* the network interface for mode 2, or "" for the system's choice */
static int fd = -1;
static uint32_t sender;
static uint8_t node;
static bool opened, selfsend, booted;
static gaddr listeners[256];
static bool sockets[256];
static at_handler handlers[256]; /* sockets of ours (ATP's): datagrams go to C */
static gaddr vars, readers;     /* RHA, and ReadPacket/ReadRest */
static gaddr names[64];         /* registered names table entries */
static int nnames;
static uint8_t nbp_id;

/* NBP calls waiting for answers */
enum { OP_LOOKUP, OP_CONFIRM, OP_REGISTER };
typedef struct {
    bool used;
    int kind;
    gaddr pb;
    uint8_t id;
    int tries;
    uint64_t next, interval;
    bool found;
} nbp_op;
static nbp_op ops[16];

/* A datagram for one of our own sockets, delivered at the next poll. */
typedef struct {
    uint8_t frame[3 + 13 + MAX_DATA];
    int len;
} frame_buf;
static frame_buf selfq[8];
static int nself;

/* The packet a listener is reading */
static const uint8_t *cur;
static int cur_left;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ---------------------------------------------------------------------- */
/* The link */

static void send_frame(const uint8_t *frame, int len)
{
    if (mode == 3) {
        et_send(frame, len);
        return;
    }
    if (fd < 0)
        return;
    uint8_t buf[4 + 3 + 13 + MAX_DATA];
    buf[0] = (uint8_t)(sender >> 24);
    buf[1] = (uint8_t)(sender >> 16);
    buf[2] = (uint8_t)(sender >> 8);
    buf[3] = (uint8_t)sender;
    memcpy(buf + 4, frame, (size_t)len);
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(LTOUDP_PORT);
    inet_pton(AF_INET, LTOUDP_GROUP, &to.sin_addr);
    sendto(fd, buf, (size_t)len + 4, 0, (struct sockaddr *)&to, sizeof to);
}

/* An interface's IPv4 address, for multicast on it. */
static bool interface_address(const char *name, struct in_addr *out)
{
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) < 0)
        return false;
    bool found = false;
    for (struct ifaddrs *i = list; i && !found; i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !strcmp(i->ifa_name, name)) {
            *out = ((struct sockaddr_in *)i->ifa_addr)->sin_addr;
            found = true;
        }
    freeifaddrs(list);
    return found;
}

static bool open_link(void)
{
    if (mode == 3) {
        if (!et_open(ifname))
            return false;
        fd = et_fd();
        return true;
    }
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return false;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(LTOUDP_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    struct ip_mreq mr;
    memset(&mr, 0, sizeof mr);
    inet_pton(AF_INET, LTOUDP_GROUP, &mr.imr_multiaddr);
    struct in_addr ifa = { htonl(mode == 2 ? INADDR_ANY : INADDR_LOOPBACK) };
    if (mode == 2 && ifname[0] && !interface_address(ifname, &ifa)) {
        fprintf(stderr, "bluebox: AppleTalk: no IPv4 address on %s\n", ifname);
        close(fd);
        fd = -1;
        return false;
    }
    mr.imr_interface = ifa;
    unsigned char ttl = 1, loop = 1;
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 ||
        setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr) < 0) {
        close(fd);
        fd = -1;
        return false;
    }
    if (mode != 2 || ifname[0])
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof ifa);
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return true;
}

/* A frame from another node, or 0. */
static int receive(uint8_t *frame, int cap)
{
    if (mode == 3)
        return et_receive(frame, cap);
    uint8_t buf[2048];
    for (;;) {
        ssize_t r = recv(fd, buf, sizeof buf, 0);
        if (r < 4 + 3)
            return 0;
        uint32_t id = (uint32_t)buf[0] << 24 | (uint32_t)buf[1] << 16 | (uint32_t)buf[2] << 8 | buf[3];
        if (id == sender)
            continue;
        int n = (int)r - 4 < cap ? (int)r - 4 : cap;
        memcpy(frame, buf + 4, (size_t)n);
        return n;
    }
}

/* LLAP node address acquisition: claim a node unless someone answers an
 * enquiry for it. */
static void acquire_node(void)
{
    if (mode == 3) {
        et_acquire(sender, &net, &node);
        return;
    }
    net = 0;
    uint8_t want = (uint8_t)(1 + (sender % 253));
    for (int attempt = 0; attempt < 20; attempt++, want = (uint8_t)(1 + (want % 253))) {
        bool taken = false;
        for (int i = 0; i < 3 && !taken; i++) {
            uint8_t enq[3] = { want, want, LLAP_ENQ };
            send_frame(enq, 3);
            uint64_t until = now_ms() + 30;
            while (!taken && now_ms() < until) {
                struct pollfd pf = { fd, POLLIN, 0 };
                if (poll(&pf, 1, 10) <= 0)
                    continue;
                uint8_t f[600];
                int n;
                while ((n = receive(f, sizeof f)) > 0)
                    if (n >= 3 && f[2] == LLAP_ACK && f[1] == want)
                        taken = true;
            }
        }
        if (!taken) {
            node = want;
            return;
        }
    }
    node = want;
}

/* ---------------------------------------------------------------------- */
/* Names */

static int upper(int ch) { return ch >= 'a' && ch <= 'z' ? ch - 32 : ch; }

/* NBP's matching: case-insensitive, "=" anything, "≈" (0xC5) any run of
 * characters in the middle. */
static bool name_match(const uint8_t *pat, const uint8_t *s)
{
    int pl = pat[0], sl = s[0];
    if (pl == 1 && pat[1] == '=')
        return true;
    for (int i = 1; i <= pl; i++)
        if (pat[i] == 0xC5) {
            int before = i - 1, after = pl - i;
            if (sl < before + after)
                return false;
            for (int k = 0; k < before; k++)
                if (upper(pat[1 + k]) != upper(s[1 + k]))
                    return false;
            for (int k = 0; k < after; k++)
                if (upper(pat[i + 1 + k]) != upper(s[sl - after + 1 + k]))
                    return false;
            return true;
        }
    if (pl != sl)
        return false;
    for (int i = 1; i <= pl; i++)
        if (upper(pat[i]) != upper(s[i]))
            return false;
    return true;
}

static bool zone_match(const uint8_t *z) { return z[0] == 0 || (z[0] == 1 && z[1] == '*'); }

/* An entity's three strings in host memory, from guest memory. */
typedef struct {
    uint8_t obj[33], type[33], zone[33];
    int len; /* bytes it took */
} entity;

static int get_str(uint8_t *out, const uint8_t *in, int avail)
{
    int n = avail > 0 ? in[0] : 0;
    if (n > 32)
        n = 32;
    if (n + 1 > avail)
        n = avail - 1 < 0 ? 0 : avail - 1;
    out[0] = (uint8_t)n;
    memcpy(out + 1, in + 1, (size_t)n);
    return n + 1;
}

static int parse_entity(entity *e, const uint8_t *in, int avail)
{
    int k = get_str(e->obj, in, avail);
    k += get_str(e->type, in + k, avail - k);
    k += get_str(e->zone, in + k, avail - k);
    e->len = k;
    return k;
}

static void guest_entity(entity *e, gaddr p)
{
    uint8_t buf[99];
    gget(buf, p, sizeof buf);
    parse_entity(e, buf, sizeof buf);
}

static int put_entity(uint8_t *out, const entity *e)
{
    int k = 0;
    memcpy(out + k, e->obj, (size_t)e->obj[0] + 1);
    k += e->obj[0] + 1;
    memcpy(out + k, e->type, (size_t)e->type[0] + 1);
    k += e->type[0] + 1;
    memcpy(out + k, e->zone, (size_t)e->zone[0] + 1);
    k += e->zone[0] + 1;
    return k;
}

/* A names table entry: next, address (net, node, socket), enumerator, entity. */
static bool table_match(gaddr nte, const entity *want, entity *got)
{
    guest_entity(got, nte + 9);
    return name_match(want->obj, got->obj) && name_match(want->type, got->type);
}

/* ---------------------------------------------------------------------- */
/* DDP */

static void send_ddp(uint16_t net, uint8_t dnode, uint8_t dskt, uint8_t sskt, uint8_t type, const uint8_t *data, int n);

static void nbp_send(uint8_t fn, uint8_t id, uint8_t dnode, uint8_t dskt, const uint8_t *tuples, int ntuples, int len)
{
    uint8_t pkt[MAX_DATA];
    pkt[0] = (uint8_t)(fn << 4 | (ntuples & 15));
    pkt[1] = id;
    memcpy(pkt + 2, tuples, (size_t)len);
    send_ddp(0, dnode, dskt, NIS_SOCKET, DDP_NBP, pkt, len + 2);
}

/* A lookup tuple for one of our names: address, enumerator, entity. */
static int tuple_of(uint8_t *out, gaddr nte, const entity *e)
{
    out[0] = (uint8_t)(net >> 8);
    out[1] = (uint8_t)net;
    out[2] = node;
    out[3] = rd8(nte + 7);
    out[4] = rd8(nte + 8);
    return 5 + put_entity(out + 5, e);
}

static void send_lookup(uint8_t id, const entity *e, uint8_t dnode)
{
    uint8_t t[5 + 99];
    t[0] = (uint8_t)(net >> 8);
    t[1] = (uint8_t)net;
    t[2] = node;
    t[3] = NIS_SOCKET;
    t[4] = 0;
    int len = 5 + put_entity(t + 5, e);
    nbp_send(NBP_LKUP, id, dnode, NIS_SOCKET, t, 1, len);
}

static void lookup_request(nbp_op *o, uint8_t dnode)
{
    entity e;
    guest_entity(&e, rd32(o->pb + PB_ENTITY) + (o->kind == OP_REGISTER ? 9 : 0)); /* a names table entry's entity */
    send_lookup(o->id, &e, dnode);
}

/* A lookup by host code (at_nbp_lookup): the first tuple answering it. */
static struct {
    bool used, found;
    uint8_t id;
    uint8_t tuple[5];
} host_lookup;

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

static void add_name(gaddr nte)
{
    wr16(nte + 4, 0);
    wr8(nte + 6, node);
    for (int i = 0; i < nnames; i++)
        if (names[i] == nte)
            return;
    if (nnames < (int)(sizeof names / sizeof names[0]))
        names[nnames++] = nte;
}

/* A lookup's answer, added to its buffer unless already there. */
static void lookup_answer(nbp_op *o, const uint8_t *tuple, int len)
{
    gaddr pb = o->pb, buf = rd32(pb + PB_RETBUFF);
    uint16_t size = rd16(pb + PB_RETSIZE), got = rd16(pb + PB_GOTTEN);
    /* where the buffer's tuples end, and whether this one is there */
    uint32_t used = 0;
    for (int i = 0; i < got; i++) {
        uint8_t t[5 + 99];
        gget(t, buf + used, sizeof t);
        entity e;
        int tl = 5 + parse_entity(&e, t + 5, (int)sizeof t - 5);
        if (tl == len && !memcmp(t, tuple, (size_t)len))
            return;
        used += (uint32_t)tl;
    }
    if (used + (uint32_t)len > size) {
        o->found = true; /* full */
        return;
    }
    gput(buf + used, tuple, (uint32_t)len);
    wr16(pb + PB_GOTTEN, (uint16_t)(got + 1));
}

static void nbp_receive(const uint8_t *pkt, int n, uint8_t snode)
{
    if (n < 2)
        return;
    int fn = pkt[0] >> 4, count = pkt[0] & 15;
    uint8_t id = pkt[1];
    const uint8_t *t = pkt + 2;
    int left = n - 2;
    if (fn == NBP_LKUP || fn == NBP_BRRQ) {
        if (left < 5 || !count)
            return;
        entity want;
        parse_entity(&want, t + 5, left - 5);
        if (!zone_match(want.zone))
            return;
        uint8_t reply[MAX_DATA];
        int len = 0, k = 0;
        for (int i = 0; i < nnames && k < 15; i++) {
            entity got;
            if (!table_match(names[i], &want, &got))
                continue;
            uint8_t tup[5 + 99];
            int tl = tuple_of(tup, names[i], &got);
            if (len + tl > MAX_DATA - 2)
                break;
            memcpy(reply + len, tup, (size_t)tl);
            len += tl;
            k++;
        }
        if (k)
            nbp_send(NBP_LKUP_REPLY, id, t[2], t[3], reply, k, len);
        return;
    }
    if (fn != NBP_LKUP_REPLY)
        return;
    if (host_lookup.used && host_lookup.id == id && count && left >= 5 && !host_lookup.found) {
        memcpy(host_lookup.tuple, t, 5);
        host_lookup.found = true;
    }
    for (int j = 0; j < (int)(sizeof ops / sizeof ops[0]); j++) {
        nbp_op *o = &ops[j];
        if (!o->used || o->id != id)
            continue;
        const uint8_t *p = t;
        int l = left;
        for (int i = 0; i < count && l >= 5; i++) {
            entity e;
            int tl = 5 + parse_entity(&e, p + 5, l - 5);
            if (o->kind == OP_LOOKUP) {
                lookup_answer(o, p, tl);
            } else if (o->kind == OP_CONFIRM) {
                gaddr ca = o->pb + PB_CONFADDR;
                if (p[2] == rd8(ca + 2)) {
                    wr8(o->pb + PB_NEWSOCKET, p[3]);
                    o->found = true;
                }
            } else {
                o->found = true;
            }
            p += tl;
            l -= tl;
        }
    }
    (void)snode;
}

/* The listener's ReadPacket and ReadRest. */
static void set_z(cpu68k *c, bool z) { c->sr = (uint16_t)(z ? c->sr | 4 : c->sr & ~4); }

static void read_packet(cpu68k *c, uint16_t tw)
{
    int want = (int)(c->d[3] & 0xFFFF);
    if (!want || want > cur_left) {
        c->d[3] = (c->d[3] & 0xFFFF0000) | 1;
        set_z(c, false);
        return;
    }
    gput(c->a[3], cur, (uint32_t)want);
    cur += want;
    cur_left -= want;
    c->a[3] += (uint32_t)want;
    c->d[1] = (c->d[1] & 0xFFFF0000) | (uint32_t)cur_left;
    c->d[3] &= 0xFFFF0000;
    set_z(c, true);
}

static void read_rest(cpu68k *c, uint16_t tw)
{
    int size = (int)(c->d[3] & 0xFFFF);
    int n = size < cur_left ? size : cur_left;
    if (n > 0)
        gput(c->a[3], cur, (uint32_t)n);
    c->a[3] += (uint32_t)n;
    c->d[3] = (uint32_t)(int32_t)(size - cur_left);
    cur += cur_left;
    cur_left = 0;
    set_z(c, true);
}

/* A DDP datagram for this node: frame is LLAP header and DDP header. */
static void deliver(const uint8_t *f, int n)
{
    if (n < 3 + 5)
        return;
    int hl;
    uint8_t dskt, type, snode = f[1];
    if (f[2] == LLAP_DDP_SHORT) {
        hl = 5;
        dskt = f[3 + 2];
        type = f[3 + 4];
    } else if (f[2] == LLAP_DDP_LONG && n >= 3 + 13) {
        hl = 13;
        dskt = f[3 + 10];
        type = f[3 + 12];
    } else {
        return;
    }
    int len = ((f[3] & 3) << 8 | f[4]) - hl;
    if (len < 0 || 3 + hl + len > n)
        len = n - 3 - hl;
    const uint8_t *data = f + 3 + hl;
    uint8_t sskt = f[2] == LLAP_DDP_SHORT ? f[3 + 3] : f[3 + 11];
    if (dskt == NIS_SOCKET && type == DDP_NBP) {
        nbp_receive(data, len, snode);
        return;
    }
    /* AEP: the echoer on socket 4 sends a request back as a reply. */
    if (dskt == 4 && type == 4) {
        if (len > 0 && data[0] == 1) {
            uint8_t echo[MAX_DATA];
            memcpy(echo, data, (size_t)len);
            echo[0] = 2;
            send_ddp(0, snode, sskt, 4, 4, echo, len);
        }
        return;
    }
    if (sockets[dskt] && handlers[dskt]) {
        handlers[dskt](dskt, snode, sskt, type, data, len);
        return;
    }
    gaddr fn = listeners[dskt];
    LOGF(LOG_TRAPS, "  DDP in: node %u socket %u -> socket %u, type %u, %d bytes%s\n", snode, f[2] == LLAP_DDP_SHORT ? f[3 + 3] : f[3 + 11],
         dskt, type, len, sockets[dskt] && fn ? "" : " (no listener)");
    if (!sockets[dskt] || !fn || !g_cpu)
        return;
    cpu68k *c = g_cpu;
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d);
    memcpy(a, c->a, sizeof a);
    uint16_t sr = c->sr;
    gput(vars + TO_RHA, f, (uint32_t)(3 + hl));
    cur = data;
    cur_left = len;
    c->a[0] = c->a[1] = 0;
    c->a[2] = vars;
    c->a[3] = vars + TO_RHA + 3 + (uint32_t)hl;
    c->a[4] = readers;
    c->d[0] = dskt;
    c->d[1] = (uint32_t)len;
    guest_call_regs(fn);
    memcpy(c->d, d, sizeof d);
    memcpy(c->a, a, sizeof a);
    c->sr = sr;
    cur = NULL;
    cur_left = 0;
}

void at_send_ddp(uint8_t dnode, uint8_t dskt, uint8_t sskt, uint8_t type, const uint8_t *data, int n)
{
    send_ddp(0, dnode, dskt, sskt, type, data, n);
}

/* A socket of ours: `want` (1-254), or 0 for any dynamic one; 0 if none. */
int at_open_socket(int want, at_handler h)
{
    int s = want;
    if (!s) {
        for (s = 128; s < 255 && sockets[s]; s++)
            ;
        if (s == 255)
            return 0;
    } else if (s < 1 || s > 254 || sockets[s]) {
        return 0;
    }
    sockets[s] = true;
    listeners[s] = 0;
    handlers[s] = h;
    return s;
}

void at_close_socket(int s)
{
    if (s > 0 && s < 255) {
        sockets[s] = false;
        handlers[s] = 0;
    }
}

uint8_t at_node(void) { return node; }
bool at_opened(void) { return opened; }
void at_complete(gaddr pb, int16_t r) { complete(pb, r); }

static void send_ddp(uint16_t net, uint8_t dnode, uint8_t dskt, uint8_t sskt, uint8_t type, const uint8_t *data, int n)
{
    uint8_t f[3 + 13 + MAX_DATA];
    int hl = 5;
    f[0] = dnode;
    f[1] = node;
    f[2] = LLAP_DDP_SHORT;
    f[3] = (uint8_t)((n + hl) >> 8 & 3);
    f[4] = (uint8_t)(n + hl);
    f[5] = dskt;
    f[6] = sskt;
    f[7] = type;
    memcpy(f + 3 + hl, data, (size_t)n);
    (void)net;
    if (type != DDP_NBP)
        LOGF(LOG_TRAPS, "  DDP out: socket %u -> node %u socket %u, type %u, %d bytes\n", sskt, dnode, dskt, type, n);
    if (dnode != node)
        send_frame(f, 3 + hl + n);
    if ((dnode == node || dnode == 255) && (selfsend || dskt == NIS_SOCKET) && nself < (int)(sizeof selfq / sizeof selfq[0])) {
        memcpy(selfq[nself].frame, f, (size_t)(3 + hl + n));
        selfq[nself++].len = 3 + hl + n;
    }
}

/* ---------------------------------------------------------------------- */
/* Interrupt time */

static bool polling;

static int16_t op_done(nbp_op *o)
{
    if (o->kind == OP_LOOKUP) {
        uint16_t want = rd16(o->pb + PB_MAXGET);
        if (o->found)
            return rd16(o->pb + PB_GOTTEN) >= want ? noErr : nbpBuffOvr;
        if (want && rd16(o->pb + PB_GOTTEN) >= want)
            return noErr;
        return o->tries > 0 ? ioInProgress : noErr;
    }
    if (o->kind == OP_CONFIRM) {
        if (o->found)
            return rd8(o->pb + PB_NEWSOCKET) == rd8(o->pb + PB_CONFADDR + 3) ? noErr : nbpConfDiff;
        return o->tries > 0 ? ioInProgress : nbpNoConfirm;
    }
    if (o->found)
        return nbpDuplicate;
    if (o->tries > 0)
        return ioInProgress;
    add_name(rd32(o->pb + PB_ENTITY));
    return noErr;
}

static void retry(nbp_op *o, uint64_t t)
{
    if (o->tries <= 0 || t < o->next)
        return;
    o->tries--;
    o->next = t + o->interval;
    if (o->tries == 0)
        return; /* the last interval passes with no more requests */
    lookup_request(o, o->kind == OP_CONFIRM ? rd8(o->pb + PB_CONFADDR + 2) : 255);
}

static void pump(void)
{
    if (fd < 0)
        return;
    uint8_t f[2048];
    int n;
    while ((n = receive(f, sizeof f)) > 0) {
        if (n < 3)
            continue;
        if (f[2] == LLAP_ENQ) {
            if (f[0] == node && node) {
                uint8_t ack[3] = { node, node, LLAP_ACK };
                send_frame(ack, 3);
            }
            continue;
        }
        if (f[0] == node || f[0] == 255)
            deliver(f, n);
    }
    while (nself > 0) {
        frame_buf b = selfq[0];
        memmove(selfq, selfq + 1, sizeof selfq[0] * (size_t)--nself);
        deliver(b.frame, b.len);
    }
}

void appletalk_poll(void)
{
    if (polling || !opened || !g_cpu)
        return;
    /* at most once a millisecond: programs poll in tight loops */
    static uint64_t last;
    uint64_t t = now_ms();
    if (t == last && !nself)
        return;
    last = t;
    polling = true;
    pump();
    atp_poll();
    adsp_poll();
    for (int i = 0; i < (int)(sizeof ops / sizeof ops[0]); i++) {
        nbp_op *o = &ops[i];
        if (!o->used)
            continue;
        retry(o, t);
        int16_t r = op_done(o);
        if (r == ioInProgress)
            continue;
        o->used = false;
        LOGF(LOG_TRAPS, "  NBP call completes -> %d\n", r);
        complete(o->pb, r);
    }
    polling = false;
}

/* An NBP call: answered at once, or waited for (synchronous), or left to
 * the poll (asynchronous). */
static int16_t nbp_call(gaddr pb, int kind, bool async)
{
    {
        entity e;
        guest_entity(&e, rd32(pb + PB_ENTITY) + (kind == OP_REGISTER ? 9 : 0));
        LOGF(LOG_TRAPS, "  NBP %s %.*s:%.*s@%.*s\n", kind == OP_LOOKUP ? "lookup" : kind == OP_CONFIRM ? "confirm" : "register",
             e.obj[0], e.obj + 1, e.type[0], e.type + 1, e.zone[0], e.zone + 1);
    }
    nbp_op *o = NULL;
    for (int i = 0; i < (int)(sizeof ops / sizeof ops[0]) && !o; i++)
        if (!ops[i].used)
            o = &ops[i];
    if (!o)
        return tooManyReqs;
    memset(o, 0, sizeof *o);
    o->used = true;
    o->kind = kind;
    o->pb = pb;
    o->id = ++nbp_id;
    int interval = rd8(pb + PB_INTERVAL), count = rd8(pb + PB_COUNT);
    o->interval = (uint64_t)(interval ? interval : 1) * 8 * 1000 / 60;
    o->tries = (count ? count : 1) + 1;
    if (kind == OP_LOOKUP) {
        wr16(pb + PB_GOTTEN, 0);
        /* our own names answer too */
        entity want;
        guest_entity(&want, rd32(pb + PB_ENTITY));
        for (int i = 0; i < nnames; i++) {
            entity got;
            if (table_match(names[i], &want, &got) && zone_match(want.zone)) {
                uint8_t tup[5 + 99];
                lookup_answer(o, tup, tuple_of(tup, names[i], &got));
            }
        }
    }
    retry(o, now_ms());
    int16_t r = op_done(o);
    if (r != ioInProgress) {
        o->used = false;
        return r;
    }
    if (async)
        return ioInProgress;
    while ((r = op_done(o)) == ioInProgress) {
        struct pollfd pf = { fd, POLLIN, 0 };
        poll(&pf, fd >= 0 ? 1 : 0, 10);
        polling = true;
        pump();
        polling = false;
        retry(o, now_ms());
    }
    o->used = false;
    return r;
}

/* An NBP lookup from host code (asp.c): the first entity to answer, after
 * up to `tries` broadcasts `interval_ms` apart, or nbpNotFound.  The
 * stack runs meanwhile, as in a synchronous LookupName. */
int16_t at_nbp_lookup(const char *obj, const char *type, const char *zone, int tries, int interval_ms,
                      uint16_t *net_out, uint8_t *node_out, uint8_t *skt_out)
{
    if (!opened || host_lookup.used)
        return -98; /* portNotCf */
    entity e;
    const char *parts[3] = { obj, type, zone && zone[0] ? zone : "*" };
    uint8_t *dst[3] = { e.obj, e.type, e.zone };
    for (int i = 0; i < 3; i++) {
        size_t n = strlen(parts[i]);
        n = n > 32 ? 32 : n;
        dst[i][0] = (uint8_t)n;
        memcpy(dst[i] + 1, parts[i], n);
    }
    host_lookup.used = true;
    host_lookup.found = false;
    host_lookup.id = ++nbp_id;
    uint64_t next = 0;
    for (int sent = 0; !host_lookup.found;) {
        uint64_t t = now_ms();
        if (t >= next) {
            if (sent++ == tries)
                break;
            send_lookup(host_lookup.id, &e, 255);
            next = t + (uint64_t)interval_ms;
        }
        struct pollfd pf = { fd, POLLIN, 0 };
        poll(&pf, fd >= 0 ? 1 : 0, 2);
        appletalk_poll();
    }
    host_lookup.used = false;
    if (!host_lookup.found)
        return nbpNotFound;
    *net_out = (uint16_t)(host_lookup.tuple[0] << 8 | host_lookup.tuple[1]);
    *node_out = host_lookup.tuple[2];
    *skt_out = host_lookup.tuple[3];
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* The driver */

static int16_t write_ddp(gaddr pb)
{
    uint8_t sskt = rd8(pb + PB_SOCKET);
    if (!sockets[sskt])
        return ddpSktErr;
    gaddr wds = rd32(pb + PB_WDS);
    gaddr hdr = rd32(wds + 2);
    uint8_t data[MAX_DATA];
    int n = 0;
    for (gaddr e = wds + 6;; e += 6) {
        uint16_t l = rd16(e);
        if (!l)
            break;
        if (n + l > MAX_DATA)
            return ddpLenErr;
        gget(data + n, rd32(e + 2), l);
        n += l;
    }
    send_ddp(rd16(hdr + 7), rd8(hdr + 11), rd8(hdr + 13), sskt, rd8(hdr + 15), data, n);
    return noErr;
}

static int16_t mpp_control(gaddr pb, bool async)
{
    switch ((int16_t)rd16(pb + CntrlParam_csCode_off)) {
    case openSkt: {
        int s = rd8(pb + PB_SOCKET);
        if (!s) {
            for (s = 128; s < 255 && sockets[s]; s++)
                ;
            if (s == 255)
                return ddpSktErr;
        } else if (sockets[s] || s < 1 || s > 254) {
            return ddpSktErr;
        }
        sockets[s] = true;
        handlers[s] = 0;
        listeners[s] = rd32(pb + PB_LISTENER);
        LOGF(LOG_TRAPS, "  DDP socket %d opened, listener %08X\n", s, listeners[s]);
        wr8(pb + PB_SOCKET, (uint8_t)s);
        return noErr;
    }
    case closeSkt: {
        int s = rd8(pb + PB_SOCKET);
        if (!sockets[s])
            return ddpSktErr;
        sockets[s] = false;
        listeners[s] = 0;
        return noErr;
    }
    case writeDDP: return write_ddp(pb);
    case registerName:
        if (rd8(pb + PB_VERIFY)) {
            entity want;
            guest_entity(&want, rd32(pb + PB_ENTITY) + 9);
            for (int i = 0; i < nnames; i++) {
                entity got;
                if (table_match(names[i], &want, &got))
                    return nbpDuplicate;
            }
            return nbp_call(pb, OP_REGISTER, async);
        }
        add_name(rd32(pb + PB_ENTITY));
        return noErr;
    case lookupName: return nbp_call(pb, OP_LOOKUP, async);
    case confirmName: return nbp_call(pb, OP_CONFIRM, async);
    case removeName: {
        entity want;
        guest_entity(&want, rd32(pb + PB_ENTITY));
        for (int i = 0; i < nnames; i++) {
            entity got;
            if (table_match(names[i], &want, &got)) {
                memmove(names + i, names + i + 1, sizeof names[0] * (size_t)(nnames - i - 1));
                nnames--;
                return noErr;
            }
        }
        return nbpNotFound;
    }
    case killNBP: {
        gaddr q = rd32(pb + PB_KILLQEL);
        for (int i = 0; i < (int)(sizeof ops / sizeof ops[0]); i++)
            if (ops[i].used && ops[i].pb == q) {
                ops[i].used = false;
                complete(q, reqAborted);
                return noErr;
            }
        return cbNotFound;
    }
    case setSelfSend:
        wr8(pb + PB_OLDSELF, selfsend);
        selfsend = rd8(pb + PB_NEWSELF) != 0;
        return noErr;
    case GetATalkInfo:
        gmemset(pb + 28, 0, 50);
        wr32(pb + 30, vars);
        wr16(pb + 44, selfsend);
        wr16(pb + 50, net);
        wr8(pb + 52, node);
        return noErr;
    case loadNBP: case unloadNBP: case SetMyZone: case ATalkClosePrep:
        return noErr;
    default: return -17; /* controlErr */
    }
}

static int16_t mpp_open(gaddr pb);
int16_t at_open(void) { return mpp_open(0); }

static int16_t mpp_open(gaddr pb)
{
    if (opened)
        return noErr;
    if (!mode)
        return -98; /* portNotCf */
    if (fd < 0 && !open_link())
        return -98;
    sender = (uint32_t)getpid() ^ (uint32_t)now_ms() << 16;
    acquire_node();
    wr8(vars + SYS_LAP_ADDR, node);
    LOGF(LOG_TRAPS, "  AppleTalk: node %u\n", node);
    wr16(vars + SYS_NET_NUM, net);
    opened = true;
    sockets[NIS_SOCKET] = true;
    wr32(LM_ABusVars, vars);
    wr8(LM_PortBUse, 1); /* AppleTalk */
    return noErr;
}

const native_driver mpp_driver = { ".MPP", 9, mpp_open, NULL, mpp_control, NULL, NULL, NULL, NULL };

/* ---------------------------------------------------------------------- */
/* Setup */

/* The LAP Manager: LAPMgrPtr ($B18) points to it and programs call
 * LAPMgrCall (2 on) with a selector in D0, chiefly to add and remove
 * AppleTalk Transition Queue entries (LAddAEQ 23, LRmvAEQ 24).  The queue
 * is kept, and since AppleTalk here never closes, no transition is sent
 * through it. */
enum { LAPMgrPtr = 0x0B18, LAddAEQ = 23, LRmvAEQ = 24 };
static gaddr transition_queue[16];
static int ntransition;

static void lap_manager(cpu68k *c, uint16_t tw)
{
    int sel = (int)(c->d[0] & 0xFF);
    gaddr entry = c->a[0];
    if (sel == LAddAEQ && ntransition < (int)(sizeof transition_queue / sizeof transition_queue[0]))
        transition_queue[ntransition++] = entry;
    else if (sel == LRmvAEQ)
        for (int i = 0; i < ntransition; i++)
            if (transition_queue[i] == entry) {
                memmove(transition_queue + i, transition_queue + i + 1, sizeof transition_queue[0] * (size_t)(--ntransition - i));
                break;
            }
    LOGF(LOG_TRAPS, "  LAP Manager selector %d\n", sel);
    c->d[0] = 0;
}

bool machine_set_appletalk(const char *how)
{
    if (!strcmp(how, "off"))
        mode = 0;
    else if (!strcmp(how, "local"))
        mode = 1;
    else if (!strcmp(how, "lan"))
        mode = 2;
    else if (!strncmp(how, "lan:", 4) && how[4] && strlen(how + 4) < sizeof ifname)
        mode = 2;
    else if (!strncmp(how, "bridge:", 7) && how[7] && strlen(how + 7) < sizeof ifname)
        mode = 3;
    else
        return false;
    const char *colon = strchr(how, ':');
    snprintf(ifname, sizeof ifname, "%s", mode >= 2 && colon ? colon + 1 : "");
    return true;
}

bool appletalk_enabled(void) { return mode != 0; }
uint8_t appletalk_node(void) { return node; }

void appletalk_reset(void)
{
    if (booted && fd >= 0 && mode != 3)
        close(fd);
    if (booted && mode == 3)
        et_close();
    booted = true;
    fd = -1;
    opened = selfsend = false;
    node = 0;
    nnames = nself = 0;
    memset(sockets, 0, sizeof sockets);
    memset(handlers, 0, sizeof handlers);
    atp_reset();
    adsp_reset();
    memset(listeners, 0, sizeof listeners);
    memset(ops, 0, sizeof ops);
    memset(&host_lookup, 0, sizeof host_lookup);
    polling = false;
    vars = tb_NewPtr(VARS_SIZE, true, true);
    /* ReadPacket at A4, ReadRest at A4+2: branches to jumps to natives */
    readers = tb_NewPtr(24, true, true);
    gaddr rp = native_register(read_packet, "ReadPacket", 0), rr = native_register(read_rest, "ReadRest", 0);
    wr16(readers, 0x6004);      /* BRA.S readers+6 */
    wr16(readers + 2, 0x6008);  /* BRA.S readers+12 */
    wr16(readers + 4, 0x4E71);
    wr16(readers + 6, 0x4EF9);  /* JMP rp */
    wr32(readers + 8, rp);
    wr16(readers + 12, 0x4EF9); /* JMP rr */
    wr32(readers + 14, rr);
    wr8(LM_PortBUse, 0xFF);
    if (mode)
        gestalt_set(fourcc("atlk"), 58); /* AppleTalk 58, as System 7.5 had */
    ntransition = 0;
    gaddr lap = tb_NewPtr(8, true, true);
    wr16(lap, 0x4E75);      /* RTS: the manager's own entry */
    wr16(lap + 2, 0x4EF9);  /* LAPMgrCall: JMP to us */
    wr32(lap + 4, native_register(lap_manager, "LAPManager", 0));
    wr32(LAPMgrPtr, lap);
}
