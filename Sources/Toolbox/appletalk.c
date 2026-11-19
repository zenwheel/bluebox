/*
 * AppleTalk: the .MPP driver (DDP sockets and NBP) on LocalTalk over UDP.
 *
 * The link is LToUDP, as other emulators and AppleTalk routers speak it:
 * each LocalTalk (LLAP) frame goes to the UDP multicast group
 * 239.192.76.84, port 1954, after a 4-byte sender ID that lets a node
 * ignore its own frames.  Without --appletalk lan the group is joined on
 * the loopback interface only, so copies on this host play each other and
 * nothing leaves it.  Without a router the network is a single LocalTalk
 * segment: network 0, nodes 1-254, short DDP headers.
 *
 * With a router on it (TashRouter, jrouter, an Apple Internet Router ...)
 * this node is one of an internet's, as the MPP was (Inside AppleTalk, 2nd
 * edition, chapters 4-8): RTMP's broadcasts (or the answer to an RTMP
 * request) give THIS-NET and A-ROUTER, which ages out after 50 seconds
 * without one; datagrams for other networks go to A-ROUTER with long
 * headers, and long headers addressed to this node are taken.  NBP looks
 * in other zones (and in this one, "*") by a broadcast request (BrRq) to
 * the router, which has the zone's networks' routers look; ZIP's calls go
 * to the router's ZIP socket through ATP.  On EtherTalk (Phase 2, an
 * extended network: ethertalk.c) GetNetInfo gives the cable range, this
 * node's zone (the one asked for, or the router's default) and the zone's
 * multicast address.
 *
 * --appletalk router:HOST[:PORT] reaches a router's LToUDP hub by unicast,
 * for networks that carry no multicast (a Tailscale tailnet, the
 * internet): the same datagrams (sender ID and LLAP frame) go to the hub,
 * which repeats them onto its LocalTalk segment and sends the segment's
 * back; a datagram of the sender ID alone, sent at the start and after
 * 20 seconds without sending, keeps the hub (and the tailnet's flow)
 * knowing this node.  Node acquisition waits longer for an answer there.
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
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "forks.h"
#include "machine.h"
#include "tb_internal.h"
#include "trap.h"

enum { LTOUDP_PORT = 1954 };
#define LTOUDP_GROUP "239.192.76.84"
enum { LLAP_DDP_SHORT = 1, LLAP_DDP_LONG = 2, LLAP_ENQ = 0x81, LLAP_ACK = 0x82 };
enum { DDP_RTMP = 1, DDP_NBP = 2, DDP_AEP = 4, DDP_RTMP_REQ = 5, DDP_ZIP = 6 };
enum { RTMP_SOCKET = 1, NIS_SOCKET = 2, AEP_SOCKET = 4, ZIS_SOCKET = 6, MAX_DATA = 586 };
enum { NBP_BRRQ = 1, NBP_LKUP = 2, NBP_LKUP_REPLY = 3 };
/* ZIP's own packets (DDP type 6): GetNetInfo and its reply, Notify; the
 * reply's flags. */
enum { ZIP_GETNETINFO = 5, ZIP_NETINFO_REPLY = 6, ZIP_NOTIFY = 7 };
enum { ZONE_INVALID = 0x80, USE_BROADCAST = 0x40, ONLY_ONE_ZONE = 0x20 };
/* A-ROUTER is forgotten 50 seconds after its last RTMP data (the MPP's
 * timer, Inside AppleTalk 4-11). */
enum { ROUTER_AGE = 50000 };
/* .MPP control codes */
enum {
    writeLAP = 243, detachPH = 244, attachPH = 245, writeDDP = 246, closeSkt = 247, openSkt = 248, loadNBP = 249, confirmName = 250, lookupName = 251,
    removeName = 252, registerName = 253, killNBP = 254, unloadNBP = 255, setSelfSend = 256,
    SetMyZone = 257, GetATalkInfo = 258, ATalkClosePrep = 259,
};
enum {
    ddpSktErr = -91, ddpLenErr = -92, lapProtErr = -94, nbpBuffOvr = -1024, nbpNoConfirm = -1025, nbpConfDiff = -1026,
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
enum { SYS_LAP_ADDR = 0, TO_RHA = 1, SYS_A_BRIDGE = 0x19, SYS_NET_NUM = 0x1A, VARS_SIZE = 64 };

static int mode = 1; /* 0 off, 1 this host, 2 the local network, 3 EtherTalk (bridged), 4 a router's hub */
static int mode_on = 1; /* the mode the Chooser's Active turns on again */
static uint16_t router_net; /* the router heard last (RTMP), node 0 if none */
static uint8_t router_node;
static uint64_t router_ticks;
static uint16_t net;  /* THIS-NET: 0 on LocalTalk until a router says (RTMP); from AARP and RTMP on EtherTalk */
static char zone_want[33];  /* the zone asked for on an extended network (MacRoman), "" the default */
static char zone_mine[33];  /* this node's zone, when known (zone_known) */
static bool zone_known;
static uint64_t rtmp_asked; /* when an RTMP request last went */
static bool netinfo_got;    /* GetNetInfo answered (EtherTalk) */
static char ltoudp_group[16]; /* LToUDP's group and port: "" and 0 the standard */
static uint16_t ltoudp_port;
static bool fake_link;      /* tests: LToUDP datagrams on a socket pair */
static char hub_host[256];  /* mode 4: the hub's host name or address, and port */
static uint16_t hub_port;
static uint64_t hub_sent;   /* when a datagram last went to the hub */
static int keepalive_ms = 20000;
static char ifname[16]; /* the network interface for mode 2, or "" for the system's choice */
BB_HOST static int fd = -1;
static bb_host_value32 sender; /* from the process's ID: made again after a resume */
static uint8_t node;
static bool opened, selfsend, booted;
static int selfsend_holds; /* Open Transport's endpoints: always to ourselves too */
static gaddr listeners[256];
static gaddr lap_handlers[256]; /* LAP protocol handlers, by LLAP type */
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
static frame_buf selfq[32];
static int nself;

static bool polling; /* appletalk_poll is running */

/* The packet a listener is reading */
static const uint8_t *cur;
static int cur_left;

static uint64_t now_ms(void) { return host_clock_ms(); }

/* ---------------------------------------------------------------------- */
/* The link */

static const char *group(void) { return ltoudp_group[0] ? ltoudp_group : LTOUDP_GROUP; }
static uint16_t group_port(void) { return ltoudp_port ? ltoudp_port : LTOUDP_PORT; }

/* A frame on the link; on EtherTalk its next hop is (hop_net, frame[0]). */
static void send_frame_via(const uint8_t *frame, int len, uint16_t hop_net)
{
    if (mode == 3) {
        et_send(frame, len, hop_net);
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
    if (fake_link || mode == 4) { /* connected: to the hub */
        (void)!send(fd, buf, (size_t)len + 4, 0);
        hub_sent = now_ms();
        return;
    }
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(group_port());
    inet_pton(AF_INET, group(), &to.sin_addr);
    sendto(fd, buf, (size_t)len + 4, 0, (struct sockaddr *)&to, sizeof to);
}

static void send_frame(const uint8_t *frame, int len) { send_frame_via(frame, len, net); }

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

/* A hub's keepalive: the sender ID alone. */
static void hub_keepalive(void)
{
    if (mode != 4 || fd < 0)
        return;
    uint8_t id[4] = { (uint8_t)(sender >> 24), (uint8_t)(sender >> 16), (uint8_t)(sender >> 8), (uint8_t)sender };
    (void)!send(fd, id, 4, 0);
    hub_sent = now_ms();
}

/* Mode 4: a UDP socket connected to the hub, its name resolved now (IPv4
 * or IPv6). */
static bool open_hub(void)
{
    char port[8];
    snprintf(port, sizeof port, "%u", hub_port ? hub_port : 1955);
    struct addrinfo hints, *list = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    int e = getaddrinfo(hub_host, port, &hints, &list);
    if (e) {
        fprintf(stderr, "bluebox: AppleTalk: the router %s: %s\n", hub_host, gai_strerror(e));
        return false;
    }
    /* IPv4 first: a connected UDP socket can't tell that no one listens
     * at an address, and a name with both (localhost, a tailnet's
     * MagicDNS name) more often has the hub on IPv4. */
    for (int pass = 0; pass < 2 && fd < 0; pass++)
        for (struct addrinfo *a = list; a && fd < 0; a = a->ai_next) {
            if ((a->ai_family == AF_INET) != (pass == 0))
                continue;
            fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (fd >= 0 && connect(fd, a->ai_addr, a->ai_addrlen) < 0) {
                close(fd);
                fd = -1;
            }
        }
    freeaddrinfo(list);
    if (fd < 0) {
        fprintf(stderr, "bluebox: AppleTalk: the router %s can't be reached\n", hub_host);
        return false;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return true;
}

/* The interface EtherTalk is bridged onto, or NULL. */
const char *at_bridge_interface(void) { return mode == 3 ? ifname : NULL; }

static bool open_link(void)
{
    if (mode == 3) {
        if (!et_open(ifname))
            return false;
        fd = et_fd();
        return true;
    }
    if (fake_link)
        return fd >= 0;
    if (mode == 4)
        return open_hub();
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return false;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(group_port());
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    struct ip_mreq mr;
    memset(&mr, 0, sizeof mr);
    inet_pton(AF_INET, group(), &mr.imr_multiaddr);
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
    for (int errors = 0;;) {
        ssize_t r = recv(fd, buf, sizeof buf, 0);
        if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && ++errors < 8)
            continue; /* a hub not (yet) there: ICMP's refusal, reported once */
        if (r < 0)
            return 0;
        if (r < 4 + 3)
            continue; /* a keepalive, or too short to be a frame */
        uint32_t id = (uint32_t)buf[0] << 24 | (uint32_t)buf[1] << 16 | (uint32_t)buf[2] << 8 | buf[3];
        if (id == sender)
            continue;
        int n = (int)r - 4 < cap ? (int)r - 4 : cap;
        memcpy(frame, buf + 4, (size_t)n);
        return n;
    }
}

/* LLAP node address acquisition: claim a node unless someone answers an
 * enquiry for it, trying `first` first (0: one the sender ID picks). */
static void acquire_node_from(uint8_t first)
{
    if (mode == 3) {
        et_acquire(sender, &net, &node);
        return;
    }
    net = 0;
    /* Through a hub the answer crosses the internet: more enquiries, further
     * apart (an ACK to any of them counts). */
    int enqs = mode == 4 ? 4 : 3, wait_ms = mode == 4 ? 300 : 30;
    uint8_t want = first >= 1 && first <= 254 ? first : (uint8_t)(1 + (sender % 253));
    for (int attempt = 0; attempt < 20; attempt++, want = (uint8_t)(1 + (want % 253))) {
        bool taken = false;
        for (int i = 0; i < enqs && !taken; i++) {
            uint8_t enq[3] = { want, want, LLAP_ENQ };
            send_frame(enq, 3);
            uint64_t until = now_ms() + (uint64_t)wait_ms;
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

static void acquire_node(void) { acquire_node_from(0); }

/* ---------------------------------------------------------------------- */
/* Names */

/* AppleTalk's case folding (Inside AppleTalk, appendix D): ASCII's
 * letters and MacRoman's accented ones. */
static int upper(int ch)
{
    static const uint8_t lower[] = { 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F, 0x90, 0x91, 0x92, 0x93, 0x94,
                                     0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F, 0xBE, 0xBF, 0xCF };
    static const uint8_t upper_of[] = { 0xE7, 0xCB, 0xE5, 0x80, 0xCC, 0x81, 0x82, 0x83, 0xE9, 0xE6, 0xE8, 0xEA, 0xED, 0xEB,
                                        0xEC, 0x84, 0xEE, 0xF1, 0xEF, 0x85, 0xCD, 0xF2, 0xF4, 0xF3, 0x86, 0xAE, 0xAF, 0xCE };
    if (ch >= 'a' && ch <= 'z')
        return ch - 32;
    for (size_t i = 0; ch >= 0x80 && i < sizeof lower; i++)
        if (lower[i] == ch)
            return upper_of[i];
    return ch;
}

static bool same_name(const uint8_t *a, int alen, const uint8_t *b, int blen)
{
    if (alen != blen)
        return false;
    for (int i = 0; i < alen; i++)
        if (upper(a[i]) != upper(b[i]))
            return false;
    return true;
}

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

static bool router_up(void) { return router_node && now_ms() - router_ticks < ROUTER_AGE; }

/* Whether a lookup in zone z is one this node answers: "*" (this zone),
 * its own zone by name, and on LocalTalk, a nonextended network with one
 * zone, whatever zone a router's LkUp names (Inside AppleTalk 7-12: the
 * router only sends it to that zone's networks). */
static bool zone_ours(const uint8_t *z)
{
    if (zone_match(z))
        return true;
    if (zone_known && same_name(z + 1, z[0], (const uint8_t *)zone_mine, (int)strlen(zone_mine)))
        return true;
    return mode != 3 && router_up();
}

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

static void send_ddp(at_peer to, uint8_t dskt, uint8_t sskt, uint8_t type, const uint8_t *data, int n);

static void nbp_send(uint8_t fn, uint8_t id, at_peer to, uint8_t dskt, const uint8_t *tuples, int ntuples, int len)
{
    uint8_t pkt[MAX_DATA];
    pkt[0] = (uint8_t)(fn << 4 | (ntuples & 15));
    pkt[1] = id;
    memcpy(pkt + 2, tuples, (size_t)len);
    send_ddp(to, dskt, NIS_SOCKET, DDP_NBP, pkt, len + 2);
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

/* A lookup: to one node (a confirm), or in a zone (to = node 255 on
 * network 0).  With a router the zone's lookup is a broadcast request to
 * it (on an extended network "*" named as this node's zone, which the
 * router can't tell from the network, Inside AppleTalk 7-11); without
 * one, only this network's zone has names to find. */
static void send_lookup(uint8_t id, const entity *e, at_peer to)
{
    uint8_t t[5 + 99];
    t[0] = (uint8_t)(net >> 8);
    t[1] = (uint8_t)net;
    t[2] = node;
    t[3] = NIS_SOCKET;
    t[4] = 0;
    bool zone_wide = to == AT_PEER(0, 255);
    if (zone_wide && !zone_match(e->zone) && !router_up())
        at_router_wait();
    if (zone_wide && router_up()) {
        entity z = *e;
        if (zone_match(z.zone) && zone_known) {
            z.zone[0] = (uint8_t)strlen(zone_mine);
            memcpy(z.zone + 1, zone_mine, z.zone[0]);
        }
        int len = 5 + put_entity(t + 5, &z);
        nbp_send(NBP_BRRQ, id, AT_PEER(router_net, router_node), NIS_SOCKET, t, 1, len);
        return;
    }
    if (zone_wide && !zone_ours(e->zone))
        return; /* another zone, and no router to reach it */
    int len = 5 + put_entity(t + 5, e);
    nbp_send(NBP_LKUP, id, to, NIS_SOCKET, t, 1, len);
}

static at_peer confirm_peer(gaddr pb) { return AT_PEER(rd16(pb + PB_CONFADDR), rd8(pb + PB_CONFADDR + 2)); }

static void lookup_request(nbp_op *o)
{
    entity e;
    guest_entity(&e, rd32(o->pb + PB_ENTITY) + (o->kind == OP_REGISTER ? 9 : 0)); /* a names table entry's entity */
    send_lookup(o->id, &e, o->kind == OP_CONFIRM ? confirm_peer(o->pb) : AT_PEER(0, 255));
}

/* A lookup by host code (at_nbp_lookup): the first tuple answering it. */
static struct {
    bool used, found;
    uint8_t id;
    uint8_t tuple[5];
} host_lookup;

/* Lookups by host code that go on while the program runs (Open
 * Transport's mapper): every tuple answering goes to fn. */
typedef struct {
    bool used, done;
    uint8_t id;
    entity e;
    int tries;
    uint64_t next, interval;
    at_nbp_fn fn;
    void *ctx;
} host_op;
static host_op hops[8];

/* A synchronous call that gave up waiting for a drain (at_sync_wait):
 * the call made again, with the same parameter block, only waits for the
 * one under way (the poll finishes it meanwhile: its result is kept). */
static struct {
    gaddr pb;
    bool done;
    int16_t result;
} gave;

static void complete(gaddr pb, int16_t r)
{
    if (pb && pb == gave.pb) {
        gave.done = true;
        gave.result = r;
    }
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
    wr16(nte + 4, net);
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

/* NBP's packets to this node's names information socket.  A broadcast
 * request is a router's to handle (this node is none); lookups are
 * answered to the requester's address in the tuple, which may be on
 * another network. */
static void nbp_receive(const uint8_t *pkt, int n, at_peer from)
{
    if (n < 2)
        return;
    int fn = pkt[0] >> 4, count = pkt[0] & 15;
    uint8_t id = pkt[1];
    const uint8_t *t = pkt + 2;
    int left = n - 2;
    if (fn == NBP_LKUP) {
        if (left < 5 || !count)
            return;
        entity want;
        parse_entity(&want, t + 5, left - 5);
        if (!zone_ours(want.zone))
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
            nbp_send(NBP_LKUP_REPLY, id, AT_PEER(t[0] << 8 | t[1], t[2]), t[3], reply, k, len);
        return;
    }
    if (fn != NBP_LKUP_REPLY)
        return;
    if (host_lookup.used && host_lookup.id == id && count && left >= 5 && !host_lookup.found) {
        memcpy(host_lookup.tuple, t, 5);
        host_lookup.found = true;
    }
    for (int j = 0; j < (int)(sizeof hops / sizeof hops[0]); j++) {
        host_op *h = &hops[j];
        if (!h->used || h->done || h->id != id)
            continue;
        const uint8_t *p = t;
        int l = left;
        for (int i = 0; i < count && l >= 5; i++) {
            entity e;
            int tl = 5 + parse_entity(&e, p + 5, l - 5);
            if (h->fn)
                h->fn(h->ctx, p, tl);
            p += tl;
            l -= tl;
        }
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
                if (at_same_peer(AT_PEER(p[0] << 8 | p[1], p[2]), confirm_peer(o->pb))) {
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
    (void)from;
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

/* A frame of another LLAP type, to the protocol handler attached for it:
 * called as a listener is, A3 just past the LAP header, D1 the rest. */
static void deliver_lap(const uint8_t *f, int n)
{
    gaddr fn = lap_handlers[f[2]];
    if (!fn || !g_cpu)
        return;
    cpu68k *c = g_cpu;
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d);
    memcpy(a, c->a, sizeof a);
    uint16_t sr = c->sr;
    gput(vars + TO_RHA, f, 3);
    cur = f + 3;
    cur_left = n - 3;
    c->a[0] = c->a[1] = 0;
    c->a[2] = vars;
    c->a[3] = vars + TO_RHA + 3;
    c->a[4] = readers;
    c->d[1] = (uint32_t)(n - 3);
    guest_call_regs(fn);
    memcpy(c->d, d, sizeof d);
    memcpy(c->a, a, sizeof a);
    c->sr = sr;
    cur = NULL;
    cur_left = 0;
}

/* DDP's checksum (Inside AppleTalk 4-17), over the long header from its
 * destination network on and the data; 0 means none, so a sum of 0 is
 * sent as $FFFF. */
static uint16_t ddp_checksum(const uint8_t *p, int n)
{
    uint32_t sum = 0;
    for (int i = 0; i < n; i++) {
        sum = (sum + p[i]) & 0xFFFF;
        sum = (sum << 1 | sum >> 15) & 0xFFFF;
    }
    return sum ? (uint16_t)sum : 0xFFFF;
}

static void set_this_net(uint16_t n)
{
    if (n == net)
        return;
    LOGF(LOG_TRAPS, "  AppleTalk: this network is %u (was %u)\n", n, net);
    net = n;
    zone_known = false; /* another network's zone, to ask again */
    for (int i = 0; i < nnames; i++)
        wr16(names[i] + 4, net);
    if (vars)
        wr16(vars + SYS_NET_NUM, net);
}

/* RTMP data or an answer to our request (Inside AppleTalk 5-14): the
 * sender's network, its node ID's length (8 bits) and the ID: A-ROUTER.
 * On LocalTalk the router's network is THIS-NET. */
static void rtmp_heard(const uint8_t *d, int len, uint8_t llap_src)
{
    if (len < 4 || d[2] != 8)
        return;
    uint16_t rnet = (uint16_t)(d[0] << 8 | d[1]);
    bool was = router_up();
    router_net = rnet;
    router_node = d[3] ? d[3] : llap_src;
    router_ticks = now_ms();
    if (!was)
        LOGF(LOG_TRAPS, "  AppleTalk: router node %u on network %u\n", router_node, router_net);
    if (mode != 3 && rnet)
        set_this_net(rnet);
    if (vars)
        wr8(vars + SYS_A_BRIDGE, router_node);
}

void at_router_wait(void)
{
    if (!opened || router_up() || (rtmp_asked && now_ms() - rtmp_asked < 5000))
        return;
    rtmp_asked = now_ms();
    uint8_t req = 1; /* RTMP request */
    send_ddp(AT_PEER(0, 255), RTMP_SOCKET, RTMP_SOCKET, DDP_RTMP_REQ, &req, 1);
    if (polling)
        return; /* the answer comes at a later poll */
    for (uint64_t until = now_ms() + 250; !router_up() && now_ms() < until;) {
        struct pollfd pf = { fd, POLLIN, 0 };
        poll(&pf, fd >= 0 ? 1 : 0, 5);
        appletalk_poll();
    }
}

/* GetNetInfo (Inside AppleTalk 8-15), on an extended network: the zone
 * asked for, "" for the default. */
static void get_net_info(void)
{
    uint8_t p[7 + 32] = { ZIP_GETNETINFO, 0, 0, 0, 0, 0 };
    int zl = (int)strlen(zone_want);
    p[6] = (uint8_t)zl;
    memcpy(p + 7, zone_want, (size_t)zl);
    send_ddp(AT_PEER(0, 255), ZIS_SOCKET, ZIS_SOCKET, DDP_ZIP, p, 7 + zl);
}

static void put_zone_string(char out[33], const uint8_t *z, int avail)
{
    int l = avail > 0 ? z[0] : 0;
    if (l > 32)
        l = 32;
    if (l > avail - 1)
        l = avail - 1 < 0 ? 0 : avail - 1;
    memcpy(out, z + 1, (size_t)l);
    out[l] = 0;
}

/* GetNetInfo's reply, or a Notify that the zone changed: flags, the cable
 * range (Notify: zeros), a zone, the multicast address, and the default
 * (Notify: the new) zone when the one asked for isn't valid. */
static void zip_heard(const uint8_t *d, int len)
{
    if (len < 7 || (d[0] != ZIP_NETINFO_REPLY && d[0] != ZIP_NOTIFY))
        return;
    int flags = d[1];
    uint16_t lo = (uint16_t)(d[2] << 8 | d[3]), hi = (uint16_t)(d[4] << 8 | d[5]);
    int k = 6;
    char zone[33];
    put_zone_string(zone, d + k, len - k);
    k += 1 + d[k];
    const uint8_t *mc = NULL;
    if (k < len) {
        if (d[k] == 6 && k + 7 <= len)
            mc = d + k + 1;
        k += 1 + d[k];
    }
    if (d[0] == ZIP_NOTIFY || (flags & ZONE_INVALID)) {
        if (k >= len)
            return;
        put_zone_string(zone, d + k, len - k);
    }
    if (d[0] == ZIP_NETINFO_REPLY) {
        netinfo_got = true;
        if (lo && hi >= lo && mode == 3)
            et_set_range(lo, hi);
    }
    snprintf(zone_mine, sizeof zone_mine, "%s", zone);
    zone_known = true;
    if (mode == 3) {
        uint8_t computed[6];
        if (!mc && !(flags & USE_BROADCAST)) {
            at_zone_multicast((const uint8_t *)zone, (int)strlen(zone), computed);
            mc = computed;
        }
        et_set_zone_multicast(flags & USE_BROADCAST ? NULL : mc);
    }
    LOGF(LOG_TRAPS, "  AppleTalk: zone %s, network range %u-%u\n", zone_mine, lo, hi);
}

/* A DDP datagram for this node: frame is LLAP header and DDP header (a
 * long one from another network, or on EtherTalk). */
static void deliver(const uint8_t *f, int n)
{
    if (n >= 3 && f[2] != LLAP_DDP_SHORT && f[2] != LLAP_DDP_LONG && f[2] < 0x80) {
        deliver_lap(f, n);
        return;
    }
    if (n < 3 + 5)
        return;
    int hl;
    uint8_t dskt, sskt, type, snode = f[1];
    uint16_t snet = net;
    if (f[2] == LLAP_DDP_SHORT) {
        hl = 5;
        dskt = f[3 + 2];
        sskt = f[3 + 3];
        type = f[3 + 4];
    } else if (f[2] == LLAP_DDP_LONG && n >= 3 + 13) {
        hl = 13;
        uint16_t dnet = (uint16_t)(f[3 + 4] << 8 | f[3 + 5]);
        snet = (uint16_t)(f[3 + 6] << 8 | f[3 + 7]);
        uint8_t dnode = f[3 + 8];
        snode = f[3 + 9];
        dskt = f[3 + 10];
        sskt = f[3 + 11];
        type = f[3 + 12];
        if (dnet && net && dnet != net && mode != 3)
            return; /* another network's, for a router */
        if (dnode != node && dnode != 255)
            return;
    } else {
        return;
    }
    int len = ((f[3] & 3) << 8 | f[4]) - hl;
    if (len < 0 || 3 + hl + len > n)
        len = n - 3 - hl;
    if (hl == 13) {
        uint16_t sum = (uint16_t)(f[3 + 2] << 8 | f[3 + 3]);
        if (sum && ddp_checksum(f + 3 + 4, 9 + len) != sum)
            return; /* damaged on the way */
    }
    const uint8_t *data = f + 3 + hl;
    at_peer from = AT_PEER(snet, snode);
    if (dskt == NIS_SOCKET && type == DDP_NBP) {
        nbp_receive(data, len, from);
        return;
    }
    /* RTMP data from a router (to the RTMP socket): it is the one ZIP
     * and other zones' lookups go to. */
    if (dskt == RTMP_SOCKET && type == DDP_RTMP) {
        rtmp_heard(data, len, f[1]);
        return;
    }
    if (dskt == ZIS_SOCKET && type == DDP_ZIP) {
        zip_heard(data, len);
        return;
    }
    /* AEP: the echoer on socket 4 sends a request back as a reply. */
    if (dskt == AEP_SOCKET && type == DDP_AEP) {
        if (len > 0 && data[0] == 1) {
            uint8_t echo[MAX_DATA];
            memcpy(echo, data, (size_t)len);
            echo[0] = 2;
            send_ddp(from, sskt, AEP_SOCKET, DDP_AEP, echo, len);
        }
        return;
    }
    if (sockets[dskt] && handlers[dskt]) {
        handlers[dskt](dskt, from, sskt, type, data, len);
        return;
    }
    gaddr fn = listeners[dskt];
    LOGF(LOG_TRAPS, "  DDP in: node %u.%u socket %u -> socket %u, type %u, %d bytes%s\n", snet, snode, sskt, dskt, type, len,
         sockets[dskt] && fn ? "" : " (no listener)");
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

void at_send_ddp(at_peer to, uint8_t dskt, uint8_t sskt, uint8_t type, const uint8_t *data, int n)
{
    send_ddp(to, dskt, sskt, type, data, n);
}

bool at_same_peer(at_peer a, at_peer b)
{
    uint16_t an = AT_PEER_NET(a) ? AT_PEER_NET(a) : net, bn = AT_PEER_NET(b) ? AT_PEER_NET(b) : net;
    return AT_PEER_NODE(a) == AT_PEER_NODE(b) && an == bn;
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

bool at_sync_again(gaddr pb) { return pb && pb == gave.pb; }

int16_t at_sync_wait(gaddr pb)
{
    for (;;) {
        if (gave.pb == pb) {
            if (gave.done) {
                gave.pb = 0;
                return gave.result;
            }
        } else if ((int16_t)rd16(pb + IOParam_ioResult_off) != ioInProgress) {
            return (int16_t)rd16(pb + IOParam_ioResult_off);
        }
        if (g_cpu->halted)
            return (int16_t)rd16(pb + IOParam_ioResult_off);
        /* At the program's own level the wait gives up for a drain, as
         * Delay does: the call made again only waits for this one. */
        if (drain_give_up()) {
            if (gave.pb != pb) {
                gave.pb = pb;
                gave.done = false;
            }
            return ioInProgress;
        }
        struct timespec ts = { 0, 2000000 };
        nanosleep(&ts, NULL);
        appletalk_poll();
    }
}

/* Whether a network number is this one's: 0 is, and on an extended
 * network the whole cable range is (its nodes are reached directly). */
static bool on_this_net(uint16_t n)
{
    if (!n || n == net)
        return true;
    if (mode != 3)
        return false;
    uint16_t lo, hi;
    at_cable_range(&lo, &hi);
    return n >= lo && n <= hi;
}

/* A datagram: a short header to a node of this LocalTalk network, a long
 * one otherwise (always on EtherTalk), through A-ROUTER when it is for
 * another network (Inside AppleTalk 4-10).  With no router known it goes
 * on this network, in case the node is here after all. */
static void send_ddp(at_peer to, uint8_t dskt, uint8_t sskt, uint8_t type, const uint8_t *data, int n)
{
    uint16_t dnet = AT_PEER_NET(to);
    uint8_t dnode = AT_PEER_NODE(to);
    bool local = on_this_net(dnet);
    uint8_t f[3 + 13 + MAX_DATA];
    int hl;
    uint16_t hop_net = dnet ? dnet : net;
    f[1] = node;
    if (mode != 3 && local) {
        hl = 5;
        f[0] = dnode;
        f[2] = LLAP_DDP_SHORT;
        f[3] = (uint8_t)((n + hl) >> 8 & 3);
        f[4] = (uint8_t)(n + hl);
        f[5] = dskt;
        f[6] = sskt;
        f[7] = type;
    } else {
        hl = 13;
        if (!dnet && dnode != 255)
            dnet = net;
        f[0] = dnode;
        if (!local && router_up()) {
            f[0] = router_node;
            hop_net = router_net;
        }
        f[2] = LLAP_DDP_LONG;
        f[3] = (uint8_t)((n + hl) >> 8 & 3); /* hop count 0 */
        f[4] = (uint8_t)(n + hl);
        f[7] = (uint8_t)(dnet >> 8);
        f[8] = (uint8_t)dnet;
        f[9] = (uint8_t)(net >> 8);
        f[10] = (uint8_t)net;
        f[11] = dnode;
        f[12] = node;
        f[13] = dskt;
        f[14] = sskt;
        f[15] = type;
    }
    memcpy(f + 3 + hl, data, (size_t)n);
    if (hl == 13) {
        uint16_t sum = ddp_checksum(f + 7, 9 + n);
        f[5] = (uint8_t)(sum >> 8);
        f[6] = (uint8_t)sum;
    }
    if (type != DDP_NBP)
        LOGF(LOG_TRAPS, "  DDP out: socket %u -> node %u.%u socket %u, type %u, %d bytes%s\n", sskt, dnet, dnode, dskt, type, n,
             local ? "" : " (through the router)");
    bool to_us = local && (dnode == node || dnode == 255);
    if (!local || dnode != node)
        send_frame_via(f, 3 + hl + n, hop_net);
    if (to_us && (selfsend || selfsend_holds || dskt == NIS_SOCKET) && nself < (int)(sizeof selfq / sizeof selfq[0])) {
        memcpy(selfq[nself].frame, f, (size_t)(3 + hl + n));
        selfq[nself++].len = 3 + hl + n;
    }
}

/* ---------------------------------------------------------------------- */
/* Interrupt time */

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
    lookup_request(o);
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
    BB_HOST static uint64_t last; /* the host's clock */
    uint64_t t = now_ms();
    if (t == last && !nself)
        return;
    last = t;
    polling = true;
    if (mode == 4 && t - hub_sent >= (uint64_t)keepalive_ms)
        hub_keepalive();
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
    for (int i = 0; i < (int)(sizeof hops / sizeof hops[0]); i++) {
        host_op *h = &hops[i];
        if (!h->used || h->done || t < h->next)
            continue;
        if (h->tries-- <= 0) {
            h->done = true; /* the last interval has passed */
            continue;
        }
        send_lookup(h->id, &h->e, AT_PEER(0, 255));
        h->next = t + h->interval;
    }
    polling = false;
}

/* Whether AppleTalk is open: datagrams for its sockets and NBP's answers
 * are served every few ms. */
bool appletalk_in_flight(void) { return opened; }

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
            if (table_match(names[i], &want, &got) && zone_ours(want.zone)) {
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
        /* At the program's level the wait gives up for a drain: the poll
         * finishes the call (o stays), and the call made again waits. */
        if (drain_give_up()) {
            gave.pb = pb;
            gave.done = false;
            wr16(pb + IOParam_ioResult_off, ioInProgress);
            return ioInProgress;
        }
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
            send_lookup(host_lookup.id, &e, AT_PEER(0, 255));
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

/* A lookup by host code that runs on with the stack: `tries` requests
 * `interval_ms` apart, each answer's tuple (address, enumerator, entity)
 * to fn.  A handle, or 0 if none is free. */
int at_nbp_start(const uint8_t *packed, int len, int tries, int interval_ms, at_nbp_fn fn, void *ctx)
{
    if (!opened)
        return 0;
    for (int i = 0; i < (int)(sizeof hops / sizeof hops[0]); i++) {
        host_op *h = &hops[i];
        if (h->used)
            continue;
        memset(h, 0, sizeof *h);
        h->used = true;
        h->id = ++nbp_id;
        parse_entity(&h->e, packed, len);
        if (!h->e.zone[0]) { /* this zone */
            h->e.zone[0] = 1;
            h->e.zone[1] = '*';
        }
        h->tries = tries > 0 ? tries : 1;
        h->interval = (uint64_t)(interval_ms > 0 ? interval_ms : 250);
        h->fn = fn;
        h->ctx = ctx;
        LOGF(LOG_TRAPS, "  NBP host lookup %.*s:%.*s@%.*s\n", h->e.obj[0], h->e.obj + 1, h->e.type[0], h->e.type + 1,
             h->e.zone[0], h->e.zone + 1);
        appletalk_poll(); /* the first request goes now */
        return i + 1;
    }
    return 0;
}

/* Whether a host lookup is still asking. */
bool at_nbp_busy(int handle)
{
    return handle > 0 && handle <= (int)(sizeof hops / sizeof hops[0]) && hops[handle - 1].used &&
           !hops[handle - 1].done;
}

void at_nbp_stop(int handle)
{
    if (handle > 0 && handle <= (int)(sizeof hops / sizeof hops[0]))
        hops[handle - 1].used = false;
}

/* A name of host code's, on one of its sockets: a names table entry made
 * for it, so lookups find it as they find the program's.  The entry, or 0
 * when the table is full. */
gaddr at_register_name(const uint8_t *packed, int len, uint8_t skt)
{
    if (nnames >= (int)(sizeof names / sizeof names[0]))
        return 0;
    gaddr nte = tb_NewPtr(9 + 99, true, true);
    if (!nte)
        return 0;
    wr8(nte + 7, skt);
    gput(nte + 9, packed, (uint32_t)(len > 99 ? 99 : len));
    add_name(nte);
    return nte;
}

void at_remove_name(gaddr nte)
{
    for (int i = 0; i < nnames; i++)
        if (names[i] == nte) {
            memmove(names + i, names + i + 1, sizeof names[0] * (size_t)(nnames - i - 1));
            nnames--;
            tb_DisposePtr(nte);
            return;
        }
}

/* Whether this node has registered a name matching the entity already. */
bool at_name_taken(const uint8_t *packed, int len)
{
    entity want;
    parse_entity(&want, packed, len);
    for (int i = 0; i < nnames; i++) {
        entity got;
        if (table_match(names[i], &want, &got))
            return true;
    }
    return false;
}

/* Open Transport's endpoints hear what they send to this node. */
void at_hold_selfsend(bool on) { selfsend_holds += on ? 1 : selfsend_holds > 0 ? -1 : 0; }
uint16_t at_network(void) { return net; }

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
    send_ddp(AT_PEER(rd16(hdr + 7), rd8(hdr + 11)), rd8(hdr + 13), sskt, rd8(hdr + 15), data, n);
    return noErr;
}

/* writeLAP: a frame of the program's own LLAP type, its header (the
 * destination node and the type) in the write data structure's first
 * entry, the data after. */
static int16_t write_lap(gaddr pb)
{
    gaddr wds = rd32(pb + PB_WDS);
    if (!wds || !rd32(wds + 2) || rd16(wds) < 3)
        return paramErr;
    gaddr hdr = rd32(wds + 2);
    uint8_t f[3 + 600];
    f[0] = rd8(hdr);
    f[1] = node;
    f[2] = rd8(hdr + 2);
    if (f[2] == 0 || f[2] >= 0x80)
        return lapProtErr;
    int n = 0;
    for (gaddr e = wds; rd16(e); e += 6) {
        int skip = e == wds ? 3 : 0, l = rd16(e) - skip;
        if (n + l > 600)
            return ddpLenErr;
        gget(f + 3 + n, rd32(e + 2) + (uint32_t)skip, (uint32_t)l);
        n += l;
    }
    send_frame(f, 3 + n);
    return noErr;
}

static int16_t mpp_control(gaddr pb, bool async)
{
    if (!async && at_sync_again(pb)) /* made again after giving up for a drain */
        return at_sync_wait(pb);
    at_use();
    switch ((int16_t)rd16(pb + CntrlParam_csCode_off)) {
    case attachPH: { /* a handler for an LLAP type other than DDP's */
        uint8_t t = rd8(pb + PB_SOCKET);
        if (t == 0 || t == LLAP_DDP_SHORT || t == LLAP_DDP_LONG || t >= 0x80 || lap_handlers[t])
            return lapProtErr;
        lap_handlers[t] = rd32(pb + PB_LISTENER);
        return noErr;
    }
    case detachPH: {
        uint8_t t = rd8(pb + PB_SOCKET);
        if (!lap_handlers[t])
            return lapProtErr;
        lap_handlers[t] = 0;
        return noErr;
    }
    case writeLAP: return write_lap(pb);
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
    case GetATalkInfo: { /* GetAppleTalkInfo: the cable range, our and the router's addresses, the zone */
        gaddr zone = rd32(pb + 76);
        gmemset(pb + 28, 0, 48);
        wr32(pb + 30, vars);
        wr16(pb + 44, selfsend);
        uint16_t lo, hi;
        at_cable_range(&lo, &hi);
        wr16(pb + 46, lo);
        wr16(pb + 48, hi);
        wr16(pb + 50, net);
        wr8(pb + 52, node);
        if (router_up()) {
            wr16(pb + 54, router_net);
            wr8(pb + 56, router_node);
        }
        if (zone) {
            char z[33];
            at_my_zone(z);
            wr8(zone, (uint8_t)strlen(z));
            gput(zone + 1, z, (uint32_t)strlen(z));
        }
        return noErr;
    }
    case loadNBP: case unloadNBP: case SetMyZone: case ATalkClosePrep:
        return noErr;
    default: return -17; /* controlErr */
    }
}

static int16_t mpp_open(gaddr pb);
int16_t at_open(void) { return mpp_open(0); }

/* An extended network's start (Inside AppleTalk 4-8): GetNetInfo for the
 * zone, and when the router's range doesn't hold the network the node
 * took in the startup range, a node of that range instead. */
static void start_extended(void)
{
    netinfo_got = false;
    for (int i = 0; i < 3 && !netinfo_got; i++) {
        get_net_info();
        for (uint64_t until = now_ms() + 300; !netinfo_got && now_ms() < until;) {
            struct pollfd pf = { fd, POLLIN, 0 };
            poll(&pf, fd >= 0 ? 1 : 0, 5);
            polling = true;
            pump();
            polling = false;
        }
        if (!router_up() && !et_range(&(uint16_t){ 0 }, &(uint16_t){ 0 }))
            break; /* no router heard: no one to answer */
    }
    uint16_t lo, hi;
    if (et_range(&lo, &hi) && (net < lo || net > hi)) {
        et_acquire(sender, &net, &node);
        wr8(vars + SYS_LAP_ADDR, node);
        wr16(vars + SYS_NET_NUM, net);
        LOGF(LOG_TRAPS, "  AppleTalk: network %u node %u, in the router's range\n", net, node);
    }
}

/* The system opened .MPP at startup when AppleTalk was active, and
 * programs of the time (Chatter) call the drivers without opening them:
 * the first call opens it, as the driver it reached was already (once:
 * a link that cannot be opened is not tried at every call). */
static bool use_failed;

void at_use(void)
{
    if (!opened && mode && !use_failed && at_open() != noErr)
        use_failed = true;
}

static int16_t mpp_open(gaddr pb)
{
    if (opened)
        return noErr;
    if (!mode)
        return -98; /* portNotCf */
    if (fd < 0 && !open_link())
        return -98;
    sender = (uint32_t)getpid() ^ (uint32_t)now_ms() << 16;
    hub_keepalive(); /* the hub learns this node before it asks for a node ID */
    acquire_node();
    wr8(vars + SYS_LAP_ADDR, node);
    LOGF(LOG_TRAPS, "  AppleTalk: node %u\n", node);
    wr16(vars + SYS_NET_NUM, net);
    opened = true;
    sockets[NIS_SOCKET] = true;
    wr32(LM_ABusVars, vars);
    wr8(LM_PortBUse, 1); /* AppleTalk */
    if (mode == 3)
        start_extended();
    else
        at_router_wait(); /* THIS-NET and A-ROUTER, if there is a router */
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

/* The transition queue for InterfaceLib's LAPAddATQ and LAPRmvATQ. */
int16_t at_transition_queue(gaddr entry, bool add)
{
    cpu68k *c = g_cpu;
    uint32_t d0 = c->d[0], a0 = c->a[0];
    c->d[0] = add ? LAddAEQ : LRmvAEQ;
    c->a[0] = entry;
    int n = ntransition;
    lap_manager(c, 0);
    c->d[0] = d0;
    c->a[0] = a0;
    return add || ntransition < n ? noErr : -1; /* qErr: not in the queue */
}

/* An event to every entry's routine, C convention (event, entry, info),
 * oldest first.  When a routine refuses (a nonzero answer) and cancel is
 * an event, those already told are sent it and the refusal returned. */
int32_t at_transition_event(int32_t event, gaddr info, bool preflight, int32_t cancel)
{
    cpu68k *c = g_cpu;
    for (int i = 0; i < ntransition; i++) {
        gaddr q = transition_queue[i], fn = rd32(q + 6);
        if (!fn)
            continue;
        uint32_t sp = c->a[7];
        cpu_push32(c, info);
        cpu_push32(c, q);
        cpu_push32(c, (uint32_t)event);
        uint32_t d[8], a[8];
        memcpy(d, c->d, sizeof d);
        memcpy(a, c->a, sizeof a);
        guest_call_regs(fn);
        int32_t r = (int32_t)c->d[0];
        memcpy(c->d, d, sizeof d);
        memcpy(c->a, a, sizeof a);
        c->a[7] = sp;
        if (preflight && r) {
            for (int k = 0; k < i; k++) {
                gaddr p = transition_queue[k];
                cpu_push32(c, info);
                cpu_push32(c, p);
                cpu_push32(c, (uint32_t)cancel);
                guest_call(rd32(p + 6));
                c->a[7] = sp;
            }
            return r;
        }
    }
    return 0;
}

/* router:HOST[:PORT]: a name, an IPv4 address, or an IPv6 one (bare, or
 * in brackets when a port follows). */
static bool set_hub(const char *spec)
{
    char host[256];
    unsigned long port = 1955;
    const char *colon = strrchr(spec, ':');
    size_t hl;
    if (spec[0] == '[') {
        const char *close = strchr(spec, ']');
        if (!close || (close[1] && close[1] != ':'))
            return false;
        hl = (size_t)(close - spec - 1);
        memmove(host, spec + 1, hl < sizeof host ? hl : 0);
        colon = close[1] ? close + 1 : NULL;
    } else {
        if (colon && strchr(spec, ':') != colon)
            colon = NULL; /* a bare IPv6 address */
        hl = colon ? (size_t)(colon - spec) : strlen(spec);
        if (hl < sizeof host)
            memcpy(host, spec, hl);
    }
    if (!hl || hl >= sizeof host)
        return false;
    host[hl] = 0;
    if (colon) {
        char *end;
        port = strtoul(colon + 1, &end, 10);
        if (!colon[1] || *end || !port || port > 65535)
            return false;
    }
    snprintf(hub_host, sizeof hub_host, "%s", host);
    hub_port = (uint16_t)port;
    mode = mode_on = 4;
    ifname[0] = 0;
    return true;
}

/* Tests: the hub's keepalive interval. */
void at_set_keepalive_ms(int ms) { keepalive_ms = ms > 0 ? ms : 20000; }

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
    else if (!strncmp(how, "router:", 7) && how[7])
        return set_hub(how + 7);
    else
        return false;
    const char *colon = strchr(how, ':');
    snprintf(ifname, sizeof ifname, "%s", mode >= 2 && colon ? colon + 1 : "");
    if (mode)
        mode_on = mode;
    return true;
}

bool appletalk_enabled(void) { return mode != 0; }

/* A-ROUTER: the router RTMP last said is there (in the last 50 s). */
bool at_router(uint16_t *rnet, uint8_t *rnode)
{
    if (!router_up())
        return false;
    *rnet = router_net;
    *rnode = router_node;
    return true;
}

void machine_set_appletalk_zone(const char *zone)
{
    size_t n = utf8_to_macroman(zone ? zone : "", (uint8_t *)zone_want, 32);
    zone_want[n] = 0;
}

const char *appletalk_wanted_zone(void) { return zone_want; }

bool machine_set_ltoudp(const char *spec)
{
    char g[64] = "";
    unsigned port = 0;
    const char *colon = strchr(spec, ':');
    size_t gl = colon ? (size_t)(colon - spec) : strlen(spec);
    if (gl >= sizeof g)
        return false;
    memcpy(g, spec, gl);
    g[gl] = 0;
    if (colon) {
        char *end;
        unsigned long p = strtoul(colon + 1, &end, 10);
        if (!colon[1] || *end || p == 0 || p > 65535)
            return false;
        port = (unsigned)p;
    }
    struct in_addr a;
    if (g[0] && (inet_pton(AF_INET, g, &a) != 1 || (ntohl(a.s_addr) >> 28) != 0xE))
        return false; /* not an IPv4 multicast group */
    snprintf(ltoudp_group, sizeof ltoudp_group, "%s", g);
    ltoudp_port = (uint16_t)port;
    return true;
}

bool at_known_zone(char out[33])
{
    if (!zone_known)
        return false;
    snprintf(out, 33, "%s", zone_mine);
    return true;
}

void at_set_known_zone(const char *zone)
{
    snprintf(zone_mine, sizeof zone_mine, "%s", zone);
    zone_known = true;
}

void at_cable_range(uint16_t *lo, uint16_t *hi)
{
    if (mode == 3 && et_range(lo, hi) && net >= *lo && net <= *hi)
        return;
    *lo = *hi = net;
}

/* A zone's multicast address on EtherTalk (Inside AppleTalk 8-17): the DDP
 * checksum of its name in upper case, modulo 253, after 09:00:07:00:00. */
void at_zone_multicast(const uint8_t *zone, int len, uint8_t out[6])
{
    uint8_t up[32];
    if (len > 32)
        len = 32;
    for (int i = 0; i < len; i++)
        up[i] = (uint8_t)upper(zone[i]);
    uint16_t sum = len ? ddp_checksum(up, len) : 0;
    static const uint8_t base[5] = { 0x09, 0x00, 0x07, 0x00, 0x00 };
    memcpy(out, base, 5);
    out[5] = (uint8_t)(sum % 253);
}

/* Tests: LToUDP's datagrams (sender ID and LLAP frame) on a socket pair. */
void at_use_fake(int sock)
{
    if (fd >= 0 && mode != 3 && !fake_link)
        close(fd);
    fd = sock;
    fake_link = sock >= 0;
    if (sock >= 0)
        fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
}

/* ---------------------------------------------------------------------- */
/* Resuming (reattach.c) */

/* What of AppleTalk a snapshot cannot carry: EtherTalk bridged onto an
 * interface needs the administrator's password to open its device again. */
const char *appletalk_unresumable(void)
{
    return opened && mode == 3 && appletalk_in_use() ? "has AppleTalk open on EtherTalk (bridged to an interface)"
                                                     : NULL;
}

/* The link is the old process's: opened again on this host, with a sender
 * ID of its own (the old process may still be on the network), and this
 * node claimed again, the old address if no one answers for it (another
 * node took it meanwhile: a new one, and the names table, the driver's
 * variables and the sockets' users see the new one).  DDP sockets, names,
 * NBP's calls and ATP's transactions are the machine's: they go on, the
 * calls by their retries. */
void appletalk_after_resume(void)
{
    if (fd >= 0 && mode != 3) /* (this process's own: a test resumed in the process that saved) */
        close(fd);
    fd = -1;
    if (!opened || !mode || mode == 3)
        return;
    if (!open_link()) {
        LOGF(LOG_IDLE, "resume: AppleTalk: the link could not be opened again\n");
        return;
    }
    sender = (uint32_t)getpid() ^ (uint32_t)now_ms() << 16;
    hub_keepalive();
    uint8_t old = node;
    acquire_node_from(old);
    if (node != old) {
        for (int i = 0; i < nnames; i++)
            wr8(names[i] + 6, node);
        LOGF(LOG_IDLE, "resume: AppleTalk: node %u was taken: this node is %u now\n", old, node);
    }
    if (vars) {
        wr8(vars + SYS_LAP_ADDR, node);
        wr16(vars + SYS_NET_NUM, net);
    }
    LOGF(LOG_IDLE, "resume: AppleTalk: node %u again, %d name%s registered\n", node, nnames, nnames == 1 ? "" : "s");
    adsp_after_resume();
}

bool appletalk_in_use(void)
{
    if (!opened)
        return false;
    for (int s = 1; s < 255; s++)
        if (sockets[s] && s != NIS_SOCKET)
            return true;
    return nnames > 0;
}

static const char *mode_name(void)
{
    static char out[280];
    switch (mode) {
    case 0: return "off";
    case 1: return "local";
    case 2: snprintf(out, sizeof out, ifname[0] ? "lan:%s" : "lan", ifname); return out;
    case 4: snprintf(out, sizeof out, strchr(hub_host, ':') ? "router:[%s]:%u" : "router:%s:%u", hub_host, hub_port); return out;
    default: snprintf(out, sizeof out, "bridge:%s", ifname); return out;
    }
}

/* Inactive: the programs' transition queue told, the link closed and
 * everything on it forgotten; Active: the mode it had, the link opened
 * again when .MPP is. */
const char *appletalk_set_active(bool on)
{
    if (on && !mode) {
        mode = mode_on ? mode_on : 1;
        gestalt_set(fourcc("atlk"), 58);
    } else if (!on && mode) {
        if (opened)
            at_transition_event(2 /* ATTransClose */, 0, false, 0);
        if (fd >= 0 && mode != 3)
            close(fd);
        if (mode == 3)
            et_close();
        fd = -1;
        opened = selfsend = use_failed = false;
        node = 0;
        nnames = 0;
        memset(sockets, 0, sizeof sockets);
        memset(handlers, 0, sizeof handlers);
        memset(listeners, 0, sizeof listeners);
        memset(ops, 0, sizeof ops);
        memset(hops, 0, sizeof hops);
        router_node = 0;
        zone_known = netinfo_got = false;
        rtmp_asked = 0;
        atp_reset();
        adsp_reset();
        wr8(LM_PortBUse, 0xFF);
        if (vars)
            wr8(vars + SYS_LAP_ADDR, 0);
        mode_on = mode;
        mode = 0;
        gestalt_set(fourcc("atlk"), 0);
    }
    return mode_name();
}
uint8_t appletalk_node(void) { return node; }

void appletalk_reset(void)
{
    if (booted && fd >= 0 && mode != 3)
        close(fd);
    if (booted && mode == 3)
        et_close();
    booted = true;
    fd = -1;
    fake_link = false;
    opened = selfsend = use_failed = false;
    node = 0;
    net = 0;
    router_node = 0;
    zone_known = netinfo_got = false;
    rtmp_asked = 0;
    nnames = nself = 0;
    memset(sockets, 0, sizeof sockets);
    memset(handlers, 0, sizeof handlers);
    atp_reset();
    adsp_reset();
    memset(listeners, 0, sizeof listeners);
    memset(lap_handlers, 0, sizeof lap_handlers);
    memset(ops, 0, sizeof ops);
    memset(&host_lookup, 0, sizeof host_lookup);
    memset(hops, 0, sizeof hops);
    selfsend_holds = 0;
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
