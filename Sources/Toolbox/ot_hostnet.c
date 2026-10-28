/*
 * What Open Transport reports of the network is the host's: its interfaces,
 * addresses, default route and name servers.  Used for the port registry
 * (ot_ports.c) and the Internet services (ot_inet.c).
 */
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <resolv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __APPLE__
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/route.h>
#else
#include <netpacket/packet.h>
#endif

#include "ot_internal.h"

/* The host's default route: the gateway's address and the interface's name. */
static uint32_t default_route(char ifname[16])
{
    ifname[0] = 0;
#ifdef __APPLE__
    int mib[6] = { CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_FLAGS, RTF_GATEWAY };
    size_t need = 0;
    if (sysctl(mib, 6, NULL, &need, NULL, 0) < 0 || !need)
        return 0;
    char *buf = malloc(need);
    if (!buf)
        return 0;
    uint32_t gw = 0;
    if (sysctl(mib, 6, buf, &need, NULL, 0) == 0) {
        for (char *p = buf; p < buf + need && !gw;) {
            struct rt_msghdr *rtm = (struct rt_msghdr *)p;
            if (rtm->rtm_msglen == 0)
                break;
            struct sockaddr *sa = (struct sockaddr *)(rtm + 1);
            struct sockaddr_in *dst = NULL, *gate = NULL, *mask = NULL;
            for (int bit = 0; bit < RTAX_MAX; bit++) {
                if (!(rtm->rtm_addrs & (1 << bit)))
                    continue;
                if (bit == RTAX_DST)
                    dst = (struct sockaddr_in *)sa;
                else if (bit == RTAX_GATEWAY)
                    gate = (struct sockaddr_in *)sa;
                else if (bit == RTAX_NETMASK)
                    mask = (struct sockaddr_in *)sa;
                size_t len = sa->sa_len ? (size_t)((sa->sa_len + 3) & ~3u) : 4;
                sa = (struct sockaddr *)((char *)sa + len);
            }
            if (dst && dst->sin_family == AF_INET && dst->sin_addr.s_addr == 0 && gate &&
                gate->sin_family == AF_INET && (!mask || mask->sin_len == 0 || mask->sin_addr.s_addr == 0) &&
                !(rtm->rtm_flags & RTF_IFSCOPE)) {
                gw = ntohl(gate->sin_addr.s_addr);
                if_indextoname(rtm->rtm_index, ifname);
            }
            p += rtm->rtm_msglen;
        }
    }
    free(buf);
    return gw;
#else
    FILE *f = fopen("/proc/net/route", "r");
    if (!f)
        return 0;
    char line[256];
    uint32_t gw = 0;
    while (fgets(line, sizeof line, f)) {
        char name[16];
        unsigned dest, gate, flags;
        if (sscanf(line, "%15s %x %x %x", name, &dest, &gate, &flags) == 4 && dest == 0 && (flags & 2)) {
            gw = ntohl(gate);
            snprintf(ifname, 16, "%s", name);
            break;
        }
    }
    fclose(f);
    return gw;
#endif
}

/* The first IPv4 name server and the default domain from the host's resolver. */
static void resolver_info(uint32_t *dns, char *domain, size_t domainsize)
{
    *dns = 0;
    domain[0] = 0;
    struct __res_state st;
    memset(&st, 0, sizeof st);
    if (res_ninit(&st) == 0) {
        for (int i = 0; i < st.nscount && !*dns; i++)
            if (st.nsaddr_list[i].sin_family == AF_INET)
                *dns = ntohl(st.nsaddr_list[i].sin_addr.s_addr);
        snprintf(domain, domainsize, "%s", st.defdname);
        if (!domain[0] && st.dnsrch[0])
            snprintf(domain, domainsize, "%s", st.dnsrch[0]);
        res_nclose(&st);
    }
}

/* Whether an interface name is one a Mac would have had an Ethernet port
 * for: the host's wired and wireless adapters, not its tunnels, bridges,
 * loopback, peer-to-peer links and virtual devices. */
static bool ethernet_like(const char *name)
{
#ifdef __APPLE__
    return !strncmp(name, "en", 2) && name[2] >= '0' && name[2] <= '9';
#else
    return (!strncmp(name, "eth", 3) || !strncmp(name, "en", 2) || !strncmp(name, "wl", 2)) && strncmp(name, "env", 3);
#endif
}

int ot_host_interfaces(ot_ifinfo *out, int max)
{
    struct ifaddrs *all = NULL;
    int n = 0;
    char gwif[16];
    uint32_t gateway = default_route(gwif), dns;
    char domain[256];
    resolver_info(&dns, domain, sizeof domain);
    if (getifaddrs(&all) == 0) {
        /* First the link-level entries make the interfaces... */
        for (struct ifaddrs *a = all; a && n < max; a = a->ifa_next) {
            if (!a->ifa_addr || (a->ifa_flags & IFF_LOOPBACK) || !ethernet_like(a->ifa_name))
                continue;
#ifdef __APPLE__
            if (a->ifa_addr->sa_family != AF_LINK)
                continue;
            struct sockaddr_dl *dl = (struct sockaddr_dl *)a->ifa_addr;
            if (dl->sdl_type != IFT_ETHER)
                continue;
            const uint8_t *mac = (const uint8_t *)LLADDR(dl);
            size_t maclen = dl->sdl_alen;
#else
            if (a->ifa_addr->sa_family != AF_PACKET)
                continue;
            struct sockaddr_ll *ll = (struct sockaddr_ll *)a->ifa_addr;
            const uint8_t *mac = ll->sll_addr;
            size_t maclen = ll->sll_halen;
#endif
            bool dup = false;
            for (int i = 0; i < n; i++)
                dup |= !strcmp(out[i].name, a->ifa_name);
            if (dup)
                continue;
            ot_ifinfo *e = &out[n++];
            memset(e, 0, sizeof *e);
            snprintf(e->name, sizeof e->name, "%s", a->ifa_name);
            if (maclen == 6)
                memcpy(e->mac, mac, 6);
            e->up = a->ifa_flags & IFF_UP;
            e->running = (a->ifa_flags & IFF_RUNNING) != 0;
            e->mtu = 1500;
            e->domain[0] = 0;
        }
        /* ...and the IPv4 entries give them addresses. */
        for (struct ifaddrs *a = all; a; a = a->ifa_next) {
            if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET)
                continue;
            for (int i = 0; i < n; i++) {
                if (strcmp(out[i].name, a->ifa_name))
                    continue;
                uint32_t ip = ntohl(((struct sockaddr_in *)a->ifa_addr)->sin_addr.s_addr);
                if (!out[i].addr) {
                    out[i].addr = ip;
                    if (a->ifa_netmask)
                        out[i].mask = ntohl(((struct sockaddr_in *)a->ifa_netmask)->sin_addr.s_addr);
                    if (a->ifa_broadaddr && (a->ifa_flags & IFF_BROADCAST))
                        out[i].bcast = ntohl(((struct sockaddr_in *)a->ifa_broadaddr)->sin_addr.s_addr);
                } else if (out[i].nsecondary < 16) {
                    out[i].secondary[out[i].nsecondary++] = ip;
                }
            }
        }
        freeifaddrs(all);
    }
    for (int i = 0; i < n; i++) {
        if (out[i].addr && !out[i].bcast)
            out[i].bcast = out[i].addr | ~out[i].mask;
        /* The default route and the resolver belong to the interface the
         * route leaves by, or to the first one with an address. */
        out[i].primary = false;
    }
    int primary = -1;
    for (int i = 0; i < n && primary < 0; i++)
        if (gwif[0] && !strcmp(out[i].name, gwif))
            primary = i;
    for (int i = 0; i < n && primary < 0; i++)
        if (out[i].addr)
            primary = i;
    if (primary > 0) { /* the primary interface comes first, as port "enet" */
        ot_ifinfo t = out[primary];
        memmove(out + 1, out, (size_t)primary * sizeof *out);
        out[0] = t;
        primary = 0;
    }
    if (primary == 0 && n > 0) {
        out[0].primary = true;
        out[0].gateway = gateway;
        out[0].dns = dns;
        snprintf(out[0].domain, sizeof out[0].domain, "%s", domain);
    }
    if (n == 0 && max > 0) {
        /* A host with no adapter of the kind: the built-in Ethernet a Mac
         * has, with no cable in it. */
        memset(&out[0], 0, sizeof out[0]);
        snprintf(out[0].name, sizeof out[0].name, "en0");
        out[0].mtu = 1500;
        n = 1;
    }
    return n;
}

/* The Internet services' view: the interface Open Transport reports first. */
bool ot_primary_interface(ot_ifinfo *out)
{
    ot_ifinfo all[16];
    int n = ot_host_interfaces(all, 16);
    if (n < 1)
        return false;
    *out = all[0];
    return true;
}

/* The host's own name for DNS queries about it, and its domain. */
void ot_host_name(char *name, size_t size)
{
    if (gethostname(name, size) != 0)
        snprintf(name, size, "localhost");
    name[size - 1] = 0;
}
