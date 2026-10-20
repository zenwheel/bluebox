/*
 * EtherTalk (AppleTalk Phase 2 on Ethernet) for --appletalk bridge:IF:
 * AppleTalk with real Macs, file servers and routers on a wired network.
 *
 * Raw Ethernet needs a BPF device, which only root may open.  The helper
 * is this program run as root through the administrator password prompt
 * (machine_bpf_helper): it opens a BPF device on the interface, keeps only
 * AppleTalk's frames, passes the open descriptor back over a Unix socket
 * and exits.  Nothing stays running as root, and nothing runs as root
 * unless AppleTalk is bridged.
 *
 * Frames are 802.3 with 802.2 SNAP: DDP (08 00 07 80 9B) with long
 * headers, and AARP (00 00 00 80 F3), which finds this node an address
 * (probing a tentative one ten times) and maps other nodes' addresses to
 * Ethernet addresses.  The network number comes from a router's RTMP
 * broadcasts, or from the startup range ($FF00-$FFFE) when there is none.
 * appletalk.c sees LLAP-like frames as on LocalTalk: dst node, src node,
 * type 2, then the long DDP header.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <ifaddrs.h>
#include <mach-o/dyld.h>
#include <net/bpf.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <spawn.h>
extern char **environ;
#endif

#include "tb_internal.h"

enum { ETH_HDR = 14, SNAP = 8, DDP_LONG = 13, AARP_LEN = 28, MAX_FRAME = 1514 };
static const uint8_t snap_ddp[SNAP] = { 0xAA, 0xAA, 0x03, 0x08, 0x00, 0x07, 0x80, 0x9B };
static const uint8_t snap_aarp[SNAP] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x80, 0xF3 };
static const uint8_t broadcast[6] = { 0x09, 0x00, 0x07, 0xFF, 0xFF, 0xFF };
enum { AARP_REQUEST = 1, AARP_RESPONSE = 2, AARP_PROBE = 3 };

static int bfd = -1;
static bool fake;             /* tests: whole frames on a datagram socket */
static uint8_t mac[6];
static uint16_t my_net;
static uint8_t my_node;
static uint16_t range_lo = 0xFF00, range_hi = 0xFFFE;
static bool probing, conflict;
static int heard;             /* AppleTalk frames from other machines */
static uint8_t rbuf[65536];   /* BPF's read buffer */
static int rlen, rpos;

/* Addresses heard: AppleTalk (net, node) to Ethernet. */
static struct { uint16_t net; uint8_t node; uint8_t mac[6]; } amt[64];
static int namt;
/* Frames waiting for an address. */
static struct { uint16_t net; uint8_t node; uint8_t frame[MAX_FRAME]; int len; uint64_t since; } waiting[8];
static int nwaiting;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void learn(uint16_t net, uint8_t node, const uint8_t *hw)
{
    for (int i = 0; i < namt; i++)
        if (amt[i].net == net && amt[i].node == node) {
            memcpy(amt[i].mac, hw, 6);
            return;
        }
    if (namt == 64)
        memmove(amt, amt + 1, sizeof amt[0] * (size_t)--namt);
    amt[namt].net = net;
    amt[namt].node = node;
    memcpy(amt[namt++].mac, hw, 6);
}

/* The network a node was last heard on (appletalk.c addresses by node). */
static uint16_t net_of(uint8_t node)
{
    for (int i = namt - 1; i >= 0; i--)
        if (amt[i].node == node)
            return amt[i].net;
    return my_net;
}

static const uint8_t *lookup(uint16_t net, uint8_t node)
{
    for (int i = 0; i < namt; i++)
        if (amt[i].net == net && amt[i].node == node)
            return amt[i].mac;
    return NULL;
}

static void put_frame(const uint8_t *dst, const uint8_t *snap, const uint8_t *payload, int n)
{
    uint8_t f[MAX_FRAME];
    memcpy(f, dst, 6);
    memcpy(f + 6, mac, 6);
    int len = SNAP + n;
    f[12] = (uint8_t)(len >> 8);
    f[13] = (uint8_t)len;
    memcpy(f + ETH_HDR, snap, SNAP);
    memcpy(f + ETH_HDR + SNAP, payload, (size_t)n);
    int total = ETH_HDR + len;
    if (total < 60) {
        memset(f + total, 0, (size_t)(60 - total));
        total = 60;
    }
    if (bfd >= 0)
        (void)!write(bfd, f, (size_t)total);
}

static void aarp(int fn, const uint8_t *to_hw, uint16_t to_net, uint8_t to_node, uint16_t from_net, uint8_t from_node)
{
    uint8_t p[AARP_LEN];
    p[0] = 0; p[1] = 1;             /* Ethernet */
    p[2] = 0x80; p[3] = 0x9B;       /* AppleTalk */
    p[4] = 6; p[5] = 4;
    p[6] = 0; p[7] = (uint8_t)fn;
    memcpy(p + 8, mac, 6);
    p[14] = 0; p[15] = (uint8_t)(from_net >> 8); p[16] = (uint8_t)from_net; p[17] = from_node;
    if (to_hw)
        memcpy(p + 18, to_hw, 6);
    else
        memset(p + 18, 0, 6);
    p[24] = 0; p[25] = (uint8_t)(to_net >> 8); p[26] = (uint8_t)to_net; p[27] = to_node;
    put_frame(fn == AARP_RESPONSE && to_hw ? to_hw : broadcast, snap_aarp, p, AARP_LEN);
}

/* ---------------------------------------------------------------------- */
/* Receiving */

static int read_raw(uint8_t *out, int cap)
{
    if (bfd < 0)
        return 0;
    if (fake) {
        ssize_t r = recv(bfd, out, (size_t)cap, 0);
        return r > 0 ? (int)r : 0;
    }
#ifdef __APPLE__
    for (;;) {
        if (rpos >= rlen) {
            ssize_t r = read(bfd, rbuf, sizeof rbuf);
            if (r <= 0)
                return 0;
            rlen = (int)r;
            rpos = 0;
        }
        struct bpf_hdr *h = (struct bpf_hdr *)(rbuf + rpos);
        int n = (int)h->bh_caplen < cap ? (int)h->bh_caplen : cap;
        memcpy(out, rbuf + rpos + h->bh_hdrlen, (size_t)n);
        rpos += (int)BPF_WORDALIGN(h->bh_hdrlen + h->bh_caplen);
        return n;
    }
#else
    return 0;
#endif
}

static void got_aarp(const uint8_t *src_hw, const uint8_t *p)
{
    int fn = p[7];
    uint16_t snet = (uint16_t)(p[15] << 8 | p[16]), tnet = (uint16_t)(p[25] << 8 | p[26]);
    uint8_t snode = p[17], tnode = p[27];
    if (fn != AARP_PROBE)
        learn(snet, snode, p + 8);
    bool ours = tnode == my_node && (tnet == my_net || !tnet);
    if (probing && ((fn == AARP_RESPONSE && ours) || (fn == AARP_PROBE && ours && memcmp(src_hw, mac, 6))))
        conflict = true; /* someone has, or wants, the address we were trying */
    if (!probing && ours && (fn == AARP_REQUEST || fn == AARP_PROBE) && memcmp(src_hw, mac, 6))
        aarp(AARP_RESPONSE, p + 8, snet, snode, my_net, my_node);
    /* answered: send what was waiting for it */
    if (fn == AARP_RESPONSE)
        for (int i = 0; i < nwaiting;) {
            if (waiting[i].net == snet && waiting[i].node == snode) {
                put_frame(p + 8, snap_ddp, waiting[i].frame, waiting[i].len);
                memmove(waiting + i, waiting + i + 1, sizeof waiting[0] * (size_t)(--nwaiting - i));
            } else {
                i++;
            }
        }
}

/* RTMP data (DDP type 1 to socket 1): a router's network range. */
static void got_rtmp(const uint8_t *d, int n)
{
    if (n < 4 + 6)
        return;
    /* sender's net (2), id length (1), id (len bytes), then the first tuple:
     * extended ones start with the range (net, distance | $80, end net). */
    int k = 3 + d[2];
    if (k + 6 > n || !(d[k + 2] & 0x80))
        return;
    uint16_t lo = (uint16_t)(d[k] << 8 | d[k + 1]), hi = (uint16_t)(d[k + 3] << 8 | d[k + 4]);
    if (lo && hi >= lo) {
        range_lo = lo;
        range_hi = hi;
    }
}

/* A frame for appletalk.c (LLAP-like), or 0 when there is none. */
int et_receive(uint8_t *out, int cap)
{
    uint8_t f[MAX_FRAME + 64];
    for (;;) {
        int n = read_raw(f, sizeof f);
        if (n <= 0)
            return 0;
        if (n < ETH_HDR + SNAP || !memcmp(f + 6, mac, 6))
            continue; /* ours, seen sent */
        heard++;
        const uint8_t *snap = f + ETH_HDR, *p = f + ETH_HDR + SNAP;
        int len = (f[12] << 8 | f[13]) - SNAP;
        if (len < 0 || len > n - ETH_HDR - SNAP)
            len = n - ETH_HDR - SNAP;
        if (!memcmp(snap, snap_aarp, SNAP) && len >= AARP_LEN) {
            got_aarp(f + 6, p);
            continue;
        }
        if (memcmp(snap, snap_ddp, SNAP) || len < DDP_LONG)
            continue;
        uint16_t dnet = (uint16_t)(p[4] << 8 | p[5]), snet = (uint16_t)(p[6] << 8 | p[7]);
        uint8_t dnode = p[8], snode = p[9];
        if (snode && snode != 255)
            learn(snet, snode, f + 6);
        if (p[10] == 1 && p[12] == 1) /* RTMP data to socket 1 */
            got_rtmp(p + DDP_LONG, len - DDP_LONG);
        if (probing)
            continue;
        if (dnet && dnet != my_net && dnet != 0xFFFF)
            continue;
        if (dnode != my_node && dnode != 255)
            continue;
        if (3 + len > cap)
            continue;
        out[0] = dnode;
        out[1] = snode;
        out[2] = 2; /* long DDP */
        memcpy(out + 3, p, (size_t)len);
        return 3 + len;
    }
}

/* ---------------------------------------------------------------------- */
/* Sending: appletalk.c's LLAP-like frames as DDP with long headers. */

void et_send(const uint8_t *f, int n)
{
    if (n < 3 || (f[2] != 1 && f[2] != 2))
        return; /* LLAP control frames (ENQ, ACK): LocalTalk only */
    uint8_t p[MAX_FRAME];
    int len;
    uint8_t dnode = f[0];
    if (f[2] == 1) { /* short header: lengthen it */
        int dlen = n - 3 - 5;
        len = DDP_LONG + dlen;
        p[0] = (uint8_t)(len >> 8 & 3);
        p[1] = (uint8_t)len;
        p[2] = p[3] = 0;                                    /* no checksum */
        uint16_t to = dnode == 255 ? 0 : net_of(dnode); /* broadcasts: this network */
        p[4] = (uint8_t)(to >> 8); p[5] = (uint8_t)to;
        p[6] = (uint8_t)(my_net >> 8); p[7] = (uint8_t)my_net;
        p[8] = dnode;
        p[9] = my_node;
        p[10] = f[5];
        p[11] = f[6];
        p[12] = f[7];
        memcpy(p + DDP_LONG, f + 8, (size_t)dlen);
    } else {
        len = n - 3;
        memcpy(p, f + 3, (size_t)len);
    }
    uint16_t dnet = (uint16_t)(p[4] << 8 | p[5]);
    if (dnode == 255) {
        put_frame(broadcast, snap_ddp, p, len);
        return;
    }
    const uint8_t *hw = lookup(dnet ? dnet : my_net, dnode);
    if (hw) {
        put_frame(hw, snap_ddp, p, len);
        return;
    }
    /* Ask for the address; the frame goes when it comes. */
    if (nwaiting == 8)
        memmove(waiting, waiting + 1, sizeof waiting[0] * (size_t)--nwaiting);
    waiting[nwaiting].net = dnet ? dnet : my_net;
    waiting[nwaiting].node = dnode;
    memcpy(waiting[nwaiting].frame, p, (size_t)len);
    waiting[nwaiting].len = len;
    waiting[nwaiting++].since = now_ms();
    aarp(AARP_REQUEST, NULL, dnet ? dnet : my_net, dnode, my_net, my_node);
}

/* ---------------------------------------------------------------------- */
/* Starting: a network number and a node of our own. */

static void listen_for(int ms)
{
    uint64_t until = now_ms() + (uint64_t)ms;
    uint8_t f[MAX_FRAME];
    while (now_ms() < until) {
        struct pollfd pf = { bfd, POLLIN, 0 };
        if (poll(&pf, 1, 20) > 0)
            while (et_receive(f, sizeof f) > 0)
                ;
    }
}

void et_acquire(uint32_t seed, uint16_t *net, uint8_t *node)
{
    probing = true;
    listen_for(fake ? 50 : 1200); /* a router says its range every 10 s; listen a little */
    for (int attempt = 0; attempt < 50; attempt++) {
        uint32_t r = seed + (uint32_t)attempt * 2654435761u;
        my_net = (uint16_t)(range_lo + r % (uint32_t)(range_hi - range_lo + 1));
        my_node = (uint8_t)(1 + (r >> 16) % 253);
        conflict = false;
        for (int i = 0; i < 10 && !conflict; i++) {
            aarp(AARP_PROBE, NULL, my_net, my_node, my_net, my_node);
            listen_for(fake ? 5 : 200);
        }
        if (!conflict)
            break;
    }
    probing = false;
    *net = my_net;
    *node = my_node;
}

int et_fd(void) { return bfd; }
int et_heard(void) { return heard; }

void et_close(void)
{
    if (bfd >= 0)
        close(bfd);
    bfd = -1;
    fake = false;
    namt = nwaiting = rlen = rpos = heard = 0;
    range_lo = 0xFF00;
    range_hi = 0xFFFE;
}

/* Tests: frames on a datagram socket, as from BPF without its headers. */
void et_use_fake(int fd, const uint8_t hw[6])
{
    et_close();
    bfd = fd;
    fake = true;
    memcpy(mac, hw, 6);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

/* ---------------------------------------------------------------------- */
/* The helper and opening the device */

#ifdef __APPLE__
/* Only AppleTalk's frames: DDP and AARP in SNAP. */
static struct bpf_insn program[] = {
    BPF_STMT(BPF_LD + BPF_W + BPF_ABS, 14),
    BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, 0xAAAA0308, 0, 2),
    BPF_STMT(BPF_LD + BPF_W + BPF_ABS, 18),
    BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, 0x0007809B, 4, 5),
    BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, 0xAAAA0300, 0, 4),
    BPF_STMT(BPF_LD + BPF_W + BPF_ABS, 18),
    BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, 0x000080F3, 1, 2),
    BPF_STMT(BPF_RET + BPF_K, 0),
    BPF_STMT(BPF_RET + BPF_K, 65535),
    BPF_STMT(BPF_RET + BPF_K, 0),
};
#endif

/* bluebox bpf-helper SOCKET IF, as root: a BPF device on IF set up for
 * AppleTalk, its descriptor sent to SOCKET.  Returns the exit status. */
int machine_bpf_helper(const char *sockpath, const char *ifname)
{
#ifdef __APPLE__
    int b = -1;
    char dev[32];
    for (int i = 0; i < 256 && b < 0; i++) {
        snprintf(dev, sizeof dev, "/dev/bpf%d", i);
        b = open(dev, O_RDWR);
    }
    if (b < 0) {
        fprintf(stderr, "bluebox: no BPF device could be opened (this needs root)\n");
        return 1;
    }
    u_int blen = sizeof rbuf, one = 1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", ifname);
    struct bpf_program prog = { sizeof program / sizeof program[0], program };
    if (ioctl(b, BIOCSBLEN, &blen) < 0 || ioctl(b, BIOCSETIF, &ifr) < 0 || ioctl(b, BIOCIMMEDIATE, &one) < 0 ||
        ioctl(b, BIOCSHDRCMPLT, &one) < 0 || ioctl(b, BIOCSETF, &prog) < 0) {
        perror("bluebox: bpf setup");
        return 1;
    }
    ioctl(b, BIOCPROMISC, NULL); /* AppleTalk's multicasts */
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sockpath);
    if (s < 0 || connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) {
        perror("bluebox: helper socket");
        return 1;
    }
    char byte = 'B';
    struct iovec iov = { &byte, 1 };
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } ctl;
    memset(&ctl, 0, sizeof ctl);
    struct msghdr m = { 0 };
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = ctl.buf;
    m.msg_controllen = sizeof ctl.buf;
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &b, sizeof(int));
    if (sendmsg(s, &m, 0) < 0) {
        perror("bluebox: helper sendmsg");
        return 1;
    }
    return 0;
#else
    (void)sockpath;
    (void)ifname;
    fprintf(stderr, "bluebox: bridged AppleTalk needs macOS for now\n");
    return 1;
#endif
}

/* The interface's Ethernet address.  macOS gives programs without an
 * entitlement a stand-in (02:00:00:00:00:00) in place of every interface's
 * address, and frames sent from it, or answers to it, are lost.  IOKit
 * still has a physical interface's; a virtual one (a VM's bridge100) has
 * its address in its IPv6 link-local one, when that is the EUI-64 kind. */
static bool interface_mac(const char *ifname)
{
#ifdef __APPLE__
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) < 0)
        return false;
    bool exists = false, found = false;
    for (struct ifaddrs *i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || strcmp(i->ifa_name, ifname))
            continue;
        if (i->ifa_addr->sa_family == AF_LINK) {
            exists = true;
            struct sockaddr_dl *dl = (struct sockaddr_dl *)i->ifa_addr;
            static const uint8_t stand_in[6] = { 0x02, 0, 0, 0, 0, 0 };
            if (dl->sdl_alen == 6 && memcmp(LLADDR(dl), stand_in, 6) && !found) {
                memcpy(mac, LLADDR(dl), 6);
                found = true;
            }
        } else if (i->ifa_addr->sa_family == AF_INET6 && !found) {
            const uint8_t *a = ((struct sockaddr_in6 *)i->ifa_addr)->sin6_addr.s6_addr;
            if (a[0] == 0xFE && (a[1] & 0xC0) == 0x80 && a[11] == 0xFF && a[12] == 0xFE) {
                mac[0] = a[8] ^ 0x02; /* the universal/local bit, inverted in EUI-64 */
                mac[1] = a[9];
                mac[2] = a[10];
                mac[3] = a[13];
                mac[4] = a[14];
                mac[5] = a[15];
                found = true;
            }
        }
    }
    freeifaddrs(list);
    if (exists && host_interface_mac(ifname, mac))
        found = true; /* the controller's own, over a link-local guess */
    return exists && found;
#else
    (void)ifname;
    return false;
#endif
}

/* Tests: the address EtherTalk would use on an interface. */
bool et_interface_mac(const char *ifname, uint8_t out[6])
{
    if (!interface_mac(ifname))
        return false;
    memcpy(out, mac, 6);
    return true;
}

/* The BPF device, from the helper run as root through the administrator
 * password prompt. */
bool et_open(const char *ifname)
{
    if (bfd >= 0)
        return true;
#ifdef __APPLE__
    if (!interface_mac(ifname)) {
        fprintf(stderr, "bluebox: AppleTalk: no Ethernet interface %s (or its address is hidden)\n", ifname);
        return false;
    }
    char dir[] = "/tmp/bluebox-bpf-XXXXXX";
    if (!mkdtemp(dir))
        return false;
    char path[64];
    snprintf(path, sizeof path, "%s/s", dir);
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    bool ok = ls >= 0 && bind(ls, (struct sockaddr *)&sa, sizeof sa) == 0 && listen(ls, 1) == 0;
    if (ok) {
        char exe[1024];
        uint32_t sz = sizeof exe;
        _NSGetExecutablePath(exe, &sz);
        char script[2048];
        snprintf(script, sizeof script,
                 "do shell script quoted form of \"%s\" & \" bpf-helper %s %s\" with administrator privileges "
                 "with prompt \"Blue Box needs your password to bridge AppleTalk onto %s.\"",
                 exe, path, ifname, ifname);
        char *argv[] = { "/usr/bin/osascript", "-e", script, NULL };
        pid_t pid;
        ok = posix_spawn(&pid, argv[0], NULL, NULL, argv, environ) == 0;
        if (ok) {
            struct pollfd pf = { ls, POLLIN, 0 };
            ok = poll(&pf, 1, 300000) > 0; /* time to type the password */
        }
        if (ok) {
            int s = accept(ls, NULL, NULL);
            char byte;
            struct iovec iov = { &byte, 1 };
            union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } ctl;
            struct msghdr m = { 0 };
            m.msg_iov = &iov;
            m.msg_iovlen = 1;
            m.msg_control = ctl.buf;
            m.msg_controllen = sizeof ctl.buf;
            ok = s >= 0 && recvmsg(s, &m, 0) > 0;
            struct cmsghdr *c = ok ? CMSG_FIRSTHDR(&m) : NULL;
            if (c && c->cmsg_type == SCM_RIGHTS)
                memcpy(&bfd, CMSG_DATA(c), sizeof(int));
            if (s >= 0)
                close(s);
        }
    }
    if (ls >= 0)
        close(ls);
    unlink(path);
    rmdir(dir);
    if (bfd < 0) {
        fprintf(stderr, "bluebox: AppleTalk: no access to %s's raw frames\n", ifname);
        return false;
    }
    fcntl(bfd, F_SETFL, fcntl(bfd, F_GETFL) | O_NONBLOCK);
    return true;
#else
    (void)ifname;
    return false;
#endif
}
