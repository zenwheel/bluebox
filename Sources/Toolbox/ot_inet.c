/*
 * Open Transport's Internet services beyond name-to-address lookups, on the
 * host's resolver and network: DNS record queries (OTInetQuery), mail
 * exchangers, host information, addresses to names, the interface's
 * description, its secondary addresses and its DHCP configuration.
 *
 * Calls on an Internet services provider in asynchronous mode finish at
 * once and the provider's notifier is told (T_DNR...COMPLETE, the result,
 * the cookie the reply structure), as the string-to-address lookup does.
 */
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <resolv.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ot_internal.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

enum {
    T_DNRADDRTONAMECOMPLETE = 0x10000002, T_DNRSYSINFOCOMPLETE = 0x10000003, T_DNRQUERYCOMPLETE = 0x10000005,
    kDefaultInetInterface = -1, kInetInterfaceInfoVersion = 3, kAllDHCPOptions = -1,
};

/* The call done: now for a synchronous provider, by its notifier for an
 * asynchronous one. */
static void finish(ppc_cpu *c, provider *p, uint32_t event, int32_t result, gaddr cookie)
{
    if (p->async) {
        ot_post(p, event, result, cookie);
        RETURN(kOTNoError);
    } else {
        RETURN((uint32_t)result);
    }
}

static void cstr_arg(char *out, size_t size, gaddr a)
{
    size_t i = 0;
    while (a && i < size - 1 && rd8(a + (uint32_t)i)) {
        out[i] = (char)rd8(a + (uint32_t)i);
        i++;
    }
    out[i] = 0;
}

/* ---- DNS ---- */

/* The answer's records, flattened to what a client can use. */
typedef struct {
    uint16_t type, cls, section;
    uint32_t ttl;
    char name[256];
    uint16_t len;
    uint8_t data[1024];
} dns_rr;

/* A name in a record's data, uncompressed, as a dotted C string. */
static int expand_name(const uint8_t *msg, const uint8_t *end, const uint8_t *at, char *out, size_t size)
{
    int n = ns_name_uncompress(msg, end, at, out, size);
    if (n < 0)
        return -1;
    if (!out[0])
        snprintf(out, size, ".");
    return n;
}

/* Turn a record's wire data into the form OTInetQuery returns: a domain
 * name for NS, CNAME, PTR and the like, preference and name for MX, the
 * strings as they are for TXT and HINFO, the address for A. */
static uint16_t convert_rdata(const uint8_t *msg, const uint8_t *end, int type, const uint8_t *rd, int rdlen,
                              uint8_t *out, size_t size)
{
    char nm[256], nm2[256];
    int n;
    switch (type) {
    case ns_t_ns: case ns_t_cname: case ns_t_ptr: case ns_t_mb: case ns_t_mg: case ns_t_mr: case ns_t_md:
    case ns_t_mf:
        if (expand_name(msg, end, rd, nm, sizeof nm) < 0)
            break;
        n = (int)strlen(nm) + 1;
        if ((size_t)n > size)
            return 0;
        memcpy(out, nm, (size_t)n);
        return (uint16_t)n;
    case ns_t_mx:
        if (rdlen < 3 || expand_name(msg, end, rd + 2, nm, sizeof nm) < 0)
            break;
        n = 2 + (int)strlen(nm) + 1;
        if ((size_t)n > size)
            return 0;
        out[0] = rd[0];
        out[1] = rd[1];
        memcpy(out + 2, nm, strlen(nm) + 1);
        return (uint16_t)n;
    case ns_t_soa: { /* mname, rname, five longs */
        int a = expand_name(msg, end, rd, nm, sizeof nm);
        if (a < 0)
            break;
        int b = expand_name(msg, end, rd + a, nm2, sizeof nm2);
        if (b < 0 || a + b + 20 > rdlen)
            break;
        n = (int)strlen(nm) + 1 + (int)strlen(nm2) + 1 + 20;
        if ((size_t)n > size)
            return 0;
        uint8_t *o = out;
        memcpy(o, nm, strlen(nm) + 1);
        o += strlen(nm) + 1;
        memcpy(o, nm2, strlen(nm2) + 1);
        o += strlen(nm2) + 1;
        memcpy(o, rd + a + b, 20);
        return (uint16_t)n;
    }
    default: break;
    }
    if ((size_t)rdlen > size)
        return 0;
    memcpy(out, rd, (size_t)rdlen);
    return (uint16_t)rdlen;
}

/* A name server to ask instead of the host's (the tests', or a program's
 * environment): 0 for the host's own. */
static uint32_t dns_override_ip;
static uint16_t dns_override_port;

void ot_inet_set_dns(uint32_t ip, uint16_t port)
{
    dns_override_ip = ip;
    dns_override_port = port;
}

/* Ask the host's name servers (with its search list) for records; the
 * records found, or -1 with an OSStatus in *err. */
static int dns_ask(const char *name, int cls, int type, dns_rr *out, int max, int32_t *err)
{
    uint8_t ans[4096];
    struct __res_state st;
    memset(&st, 0, sizeof st);
    if (res_ninit(&st) != 0) {
        *err = kOTBadNameErr;
        return -1;
    }
    if (dns_override_ip) {
        st.nscount = 1;
        memset(&st.nsaddr_list[0], 0, sizeof st.nsaddr_list[0]);
        st.nsaddr_list[0].sin_family = AF_INET;
        st.nsaddr_list[0].sin_len = sizeof st.nsaddr_list[0];
        st.nsaddr_list[0].sin_addr.s_addr = htonl(dns_override_ip);
        st.nsaddr_list[0].sin_port = htons(dns_override_port ? dns_override_port : 53);
        st.retrans = 1;
        st.retry = 2;
        st.ndots = 1;
        st.dnsrch[0] = NULL; /* no search list: the name is as given */
        st.options &= ~(unsigned)RES_DNSRCH;
    }
    int len = res_nsearch(&st, name, cls, type, ans, sizeof ans);
    int herr = st.res_h_errno;
    res_nclose(&st);
    if (len < 0) {
        *err = herr == HOST_NOT_FOUND ? kOTBadNameErr : herr == NO_DATA ? kOTNoDataErr : kETIMEDOUTErr;
        return -1;
    }
    ns_msg msg;
    if (ns_initparse(ans, len, &msg) < 0) {
        *err = kOTBadNameErr;
        return -1;
    }
    int n = 0;
    for (int sec = ns_s_an; sec <= ns_s_ar; sec++) {
        int count = ns_msg_count(msg, (ns_sect)sec);
        for (int i = 0; i < count && n < max; i++) {
            ns_rr rr;
            if (ns_parserr(&msg, (ns_sect)sec, i, &rr) < 0)
                continue;
            dns_rr *o = &out[n];
            o->type = (uint16_t)ns_rr_type(rr);
            o->cls = (uint16_t)ns_rr_class(rr);
            o->section = (uint16_t)sec;
            o->ttl = ns_rr_ttl(rr);
            snprintf(o->name, sizeof o->name, "%s", ns_rr_name(rr));
            o->len = convert_rdata(ans, ans + len, o->type, ns_rr_rdata(rr), ns_rr_rdlen(rr), o->data, sizeof o->data);
            n++;
        }
    }
    if (n == 0) {
        *err = kOTNoDataErr;
        return -1;
    }
    return n;
}

/* ---- OTInetQuery ---- */

/* DNSQueryInfo: qType, qClass, ttl, name[256], responseType, resourceLen, then
 * resourceLen bytes of data. */
enum { kQueryHeader = 268 };

/* OTInetQuery(ref, name, qClass, qType, buf, buflen, argv, argvlen, flags) */
static void i_InetQuery(ppc_cpu *c)
{
    provider *p = ot_provider_of(R(0));
    if (!p) {
        RETURN((uint32_t)kOTBadReferenceErr);
        return;
    }
    char name[256];
    cstr_arg(name, sizeof name, R(1));
    uint32_t qclass = R(2), qtype = R(3), buflen = R(5), argvlen = R(7);
    gaddr buf = R(4), argv = R(6);
    static dns_rr rrs[64];
    int32_t err = kOTNoError;
    int n = name[0] ? dns_ask(name, (int)qclass, (int)qtype, rrs, 64, &err) : (err = kOTBadNameErr, -1);
    uint32_t used = 0;
    int stored = 0;
    if (n > 0) {
        for (int i = 0; i < n && err == kOTNoError; i++) {
            uint32_t size = (kQueryHeader + rrs[i].len + 3) & ~3u;
            if (!buf || used + size > buflen || (argv && (uint32_t)(stored + 1) * 4 > argvlen)) {
                err = kOTBufferOverflowErr;
                break;
            }
            gaddr r = buf + used;
            gmemset(r, 0, size);
            wr16(r, rrs[i].type);
            wr16(r + 2, rrs[i].cls);
            wr32(r + 4, rrs[i].ttl);
            gput(r + 8, rrs[i].name, (uint32_t)strlen(rrs[i].name));
            wr16(r + 264, rrs[i].section);
            wr16(r + 266, rrs[i].len);
            gput(r + kQueryHeader, rrs[i].data, rrs[i].len);
            if (argv)
                wr32(argv + 4 * (uint32_t)stored, r);
            stored++;
            used += size;
        }
        if (argv && (uint32_t)(stored + 1) * 4 <= argvlen)
            wr32(argv + 4 * (uint32_t)stored, 0);
    }
    finish(c, p, T_DNRQUERYCOMPLETE, err, buf);
}

/* ---- Mail exchangers and host information ---- */

/* OTInetMailExchange(ref, name, &num, mx): at most *num exchangers (each a
 * preference and a name), the most preferred first; *num the number found. */
static void i_InetMailExchange(ppc_cpu *c)
{
    provider *p = ot_provider_of(R(0));
    if (!p) {
        RETURN((uint32_t)kOTBadReferenceErr);
        return;
    }
    char name[256];
    cstr_arg(name, sizeof name, R(1));
    gaddr nump = R(2), mx = R(3);
    uint32_t max = nump ? rd16(nump) : 0;
    static dns_rr rrs[64];
    int32_t err = kOTNoError;
    int n = dns_ask(name, ns_c_in, ns_t_mx, rrs, 64, &err);
    int found = 0;
    if (n > 0) {
        int idx[64], k = 0;
        for (int i = 0; i < n; i++)
            if (rrs[i].type == ns_t_mx && rrs[i].len > 2)
                idx[k++] = i;
        for (int a = 1; a < k; a++) /* by preference */
            for (int b = a; b > 0 && (rrs[idx[b]].data[0] << 8 | rrs[idx[b]].data[1]) <
                                          (rrs[idx[b - 1]].data[0] << 8 | rrs[idx[b - 1]].data[1]); b--) {
                int t = idx[b];
                idx[b] = idx[b - 1];
                idx[b - 1] = t;
            }
        for (int i = 0; i < k && (uint32_t)found < max && mx; i++) {
            gaddr e = mx + 258 * (uint32_t)found;
            gmemset(e, 0, 258);
            wr16(e, (uint16_t)(rrs[idx[i]].data[0] << 8 | rrs[idx[i]].data[1]));
            size_t len = strlen((const char *)rrs[idx[i]].data + 2);
            gput(e + 2, rrs[idx[i]].data + 2, (uint32_t)(len > 255 ? 255 : len));
            found++;
        }
        if (k == 0)
            err = kOTNoDataErr;
    }
    if (nump)
        wr16(nump, (uint16_t)(err == kOTNoError ? found : 0));
    finish(c, p, T_DNRMAILEXCHANGECOMPLETE, err, mx);
}

/* OTInetSysInfo(ref, name, &sysinfo): the host's HINFO record: CPU type and
 * operating system.  For an empty name, this machine's. */
static void i_InetSysInfo(ppc_cpu *c)
{
    provider *p = ot_provider_of(R(0));
    if (!p) {
        RETURN((uint32_t)kOTBadReferenceErr);
        return;
    }
    char name[256];
    cstr_arg(name, sizeof name, R(1));
    gaddr info = R(2);
    int32_t err = kOTNoError;
    char cpu[32] = "", os[32] = "";
    if (!name[0]) {
        struct utsname u;
        if (uname(&u) == 0) {
            snprintf(cpu, sizeof cpu, "%s", u.machine);
            snprintf(os, sizeof os, "%s", u.sysname);
        }
        for (char *s = cpu; *s; s++)
            *s = (char)toupper((unsigned char)*s);
        for (char *s = os; *s; s++)
            *s = (char)toupper((unsigned char)*s);
    } else {
        static dns_rr rrs[16];
        int n = dns_ask(name, ns_c_in, ns_t_hinfo, rrs, 16, &err);
        err = n > 0 ? kOTNoDataErr : err;
        for (int i = 0; i < n; i++) {
            if (rrs[i].type != ns_t_hinfo || rrs[i].len < 2)
                continue;
            int a = rrs[i].data[0];
            if (1 + a >= rrs[i].len)
                continue;
            int b = rrs[i].data[1 + a];
            if (2 + a + b > rrs[i].len)
                continue;
            snprintf(cpu, sizeof cpu, "%.*s", a > 31 ? 31 : a, (const char *)rrs[i].data + 1);
            snprintf(os, sizeof os, "%.*s", b > 31 ? 31 : b, (const char *)rrs[i].data + 2 + a);
            err = kOTNoError;
            break;
        }
    }
    if (info && err == kOTNoError) {
        gmemset(info, 0, 64);
        gput(info, cpu, (uint32_t)strlen(cpu));
        gput(info + 32, os, (uint32_t)strlen(os));
    }
    finish(c, p, T_DNRSYSINFOCOMPLETE, err, info);
}

/* OTInetAddressToName(ref, addr, name): the host's name for the address. */
static void i_InetAddressToName(ppc_cpu *c)
{
    provider *p = ot_provider_of(R(0));
    if (!p) {
        RETURN((uint32_t)kOTBadReferenceErr);
        return;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
#ifdef __APPLE__
    sa.sin_len = sizeof sa;
#endif
    sa.sin_addr.s_addr = htonl(R(1));
    char host[NI_MAXHOST];
    int32_t err = kOTNoError;
    if (getnameinfo((struct sockaddr *)&sa, sizeof sa, host, sizeof host, NULL, 0, NI_NAMEREQD) != 0)
        err = kOTBadNameErr;
    else if (R(2)) {
        size_t n = strlen(host) > 255 ? 255 : strlen(host);
        gmemset(R(2), 0, 256);
        gput(R(2), host, (uint32_t)n);
    }
    finish(c, p, T_DNRADDRTONAMECOMPLETE, err, R(2));
}

/* ---- The interface ---- */

/* The interface at index (kDefaultInetInterface: the primary): false if
 * there is none or it has no address, as with TCP/IP not configured. */
static bool interface_at(int32_t index, ot_ifinfo *out)
{
    ot_ifinfo all[16];
    int n = ot_host_interfaces(all, 16);
    if (index == kDefaultInetInterface)
        index = 0;
    if (index < 0 || index >= n || !all[index].addr)
        return false;
    *out = all[index];
    return true;
}

/* The hardware address lives in a block of its own for each interface. */
static gaddr hw_blocks[16];

static gaddr hwaddr_block(int index, const uint8_t mac[6])
{
    if (index < 0 || index >= 16)
        index = 0;
    if (!hw_blocks[index])
        hw_blocks[index] = tb_NewPtr(8, true, true);
    if (hw_blocks[index])
        gput(hw_blocks[index], mac, 6);
    return hw_blocks[index];
}

/* The heap went with the machine. */
void ot_inet_reset(void) { memset(hw_blocks, 0, sizeof hw_blocks); }

/* OTInetGetInterfaceInfo(info, index): InetInterfaceInfo - address, mask,
 * broadcast, gateway, name server, hardware address, MTU, domain name and the
 * number of secondary addresses. */
static void i_InetGetInterfaceInfo(ppc_cpu *c)
{
    gaddr info = R(0);
    ot_ifinfo e;
    if (!info || !interface_at((int32_t)R(1), &e)) {
        RETURN((uint32_t)kOTNotFoundErr);
        return;
    }
    gmemset(info, 0, 552);
    wr32(info + 0, e.addr);
    wr32(info + 4, e.mask);
    wr32(info + 8, e.bcast);
    wr32(info + 12, e.gateway);
    wr32(info + 16, e.dns);
    wr16(info + 20, kInetInterfaceInfoVersion);
    wr16(info + 22, 6);
    wr32(info + 24, hwaddr_block((int32_t)R(1) < 0 ? 0 : (int)R(1), e.mac));
    wr32(info + 28, e.mtu);
    size_t n = strlen(e.domain) > 255 ? 255 : strlen(e.domain);
    gput(info + 40, e.domain, (uint32_t)n);
    wr32(info + 296, (uint32_t)e.nsecondary);
    RETURN(kOTNoError);
}

/* OTInetGetSecondaryAddresses(addrs, &count, index): up to *count of the
 * interface's additional addresses; *count the number given. */
static void i_InetGetSecondaryAddresses(ppc_cpu *c)
{
    ot_ifinfo e;
    if (!interface_at((int32_t)R(2), &e)) {
        RETURN((uint32_t)kOTNotFoundErr);
        return;
    }
    gaddr addrs = R(0), countp = R(1);
    uint32_t max = countp ? rd32(countp) : 0, n = 0;
    for (; n < max && (int)n < e.nsecondary && addrs; n++)
        wr32(addrs + 4 * n, e.secondary[n]);
    if (countp)
        wr32(countp, n);
    RETURN((int)n < e.nsecondary && n == max && max ? kOTBufferOverflowErr : kOTNoError);
}

/* ---- DHCP ---- */

/* The DHCP lease's options as the host's ipconfig prints them: tag, the
 * name ipconfig uses, the kind of value. */
typedef struct {
    uint8_t tag;
    const char *name;
    char kind; /* 'a' one address, 'A' addresses, 's' a string, 'l' a long, 'b' a byte */
} dhcp_opt;

static const dhcp_opt dhcp_opts[] = {
    { 1, "subnet_mask", 'a' }, { 2, "time_offset", 'l' }, { 3, "router", 'A' }, { 4, "time_server", 'A' },
    { 6, "domain_name_server", 'A' }, { 7, "log_server", 'A' }, { 12, "host_name", 's' }, { 15, "domain_name", 's' },
    { 26, "interface_mtu", 'l' }, { 28, "broadcast_address", 'a' }, { 42, "ntp_server", 'A' },
    { 44, "netbios_name_server", 'A' }, { 51, "lease_time", 'l' }, { 53, "message_type", 'b' },
    { 54, "server_identifier", 'a' }, { 58, "renewal_t1_time_value", 'l' }, { 59, "rebinding_t2_time_value", 'l' },
};

#ifdef __APPLE__
extern char **environ;

/* The first line ipconfig prints for "getoption <if> <name>", run directly
 * (no shell: nothing a program says reaches a command line). */
static bool ipconfig_option(const char *ifname, const char *name, char *line, size_t size)
{
    int fd[2];
    if (pipe(fd) != 0)
        return false;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&fa, fd[0]);
    posix_spawn_file_actions_addclose(&fa, fd[1]);
    char *argv[] = { "/usr/sbin/ipconfig", "getoption", (char *)ifname, (char *)name, NULL };
    pid_t pid;
    int err = posix_spawn(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fd[1]);
    if (err != 0) {
        close(fd[0]);
        return false;
    }
    FILE *f = fdopen(fd[0], "r");
    bool ok = f && fgets(line, (int)size, f) != NULL;
    if (f)
        fclose(f);
    else
        close(fd[0]);
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return ok;
}
#endif

static bool dhcp_fetch(const char *ifname, uint8_t tag, uint8_t *val, int *len)
{
#ifdef __APPLE__
    for (unsigned i = 0; i < sizeof dhcp_opts / sizeof dhcp_opts[0]; i++) {
        if (dhcp_opts[i].tag != tag)
            continue;
        char line[512];
        if (!ipconfig_option(ifname, dhcp_opts[i].name, line, sizeof line))
            return false;
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0])
            return false;
        int n = 0;
        switch (dhcp_opts[i].kind) {
        case 's':
            n = (int)strlen(line) > 255 ? 255 : (int)strlen(line);
            memcpy(val, line, (size_t)n);
            break;
        case 'a': case 'A':
            for (char *tok = strtok(line, " "); tok && n + 4 <= 255; tok = strtok(NULL, " ")) {
                struct in_addr a;
                if (!inet_aton(tok, &a))
                    return false;
                memcpy(val + n, &a.s_addr, 4);
                n += 4;
            }
            break;
        case 'l': {
            uint32_t x = (uint32_t)strtoul(line, NULL, 0);
            val[0] = (uint8_t)(x >> 24);
            val[1] = (uint8_t)(x >> 16);
            val[2] = (uint8_t)(x >> 8);
            val[3] = (uint8_t)x;
            n = 4;
            break;
        }
        default:
            val[0] = (uint8_t)strtoul(line, NULL, 0);
            n = 1;
        }
        *len = n;
        return n > 0;
    }
#else
    (void)ifname;
    (void)tag;
    (void)val;
    (void)len;
#endif
    return false;
}

/* OTInetGetDHCPConfigInfo(buf, bufSize, index, option): the DHCP options the
 * interface's lease holds, as tag, length and value one after the other
 * (kAllDHCPOptions), or the one asked for.  An interface that isn't leased
 * has none: kOTNotFoundErr. */
static void i_InetGetDHCPConfigInfo(ppc_cpu *c)
{
    gaddr buf = R(0);
    uint32_t size = R(1);
    int32_t opt = (int32_t)R(3);
    ot_ifinfo e;
    if (!buf || !interface_at((int32_t)R(2), &e)) {
        RETURN((uint32_t)kOTNotFoundErr);
        return;
    }
    uint32_t used = 0;
    bool any = false;
    for (unsigned i = 0; i < sizeof dhcp_opts / sizeof dhcp_opts[0]; i++) {
        if (opt != kAllDHCPOptions && (uint8_t)opt != dhcp_opts[i].tag)
            continue;
        uint8_t v[256];
        int len = 0;
        if (!dhcp_fetch(e.name, dhcp_opts[i].tag, v, &len))
            continue;
        if (used + 2 + (uint32_t)len > size) {
            RETURN((uint32_t)kOTBufferOverflowErr);
            return;
        }
        wr8(buf + used, dhcp_opts[i].tag);
        wr8(buf + used + 1, (uint8_t)len);
        gput(buf + used + 2, v, (uint32_t)len);
        used += 2 + (uint32_t)len;
        any = true;
    }
    RETURN(any ? kOTNoError : kOTNotFoundErr);
}

const ppc_export oti_exports[] = {
    { "OTInetQuery", i_InetQuery }, { "OTInetMailExchange", i_InetMailExchange },
    { "OTInetSysInfo", i_InetSysInfo }, { "OTInetAddressToName", i_InetAddressToName },
    { "OTInetGetInterfaceInfo", i_InetGetInterfaceInfo }, { "OTGetInterfaceInfo", i_InetGetInterfaceInfo },
    { "OTInetGetSecondaryAddresses", i_InetGetSecondaryAddresses },
    { "OTGetSecondaryAddresses", i_InetGetSecondaryAddresses },
    { "OTInetGetDHCPConfigInfo", i_InetGetDHCPConfigInfo }, { "OTGetDHCPConfigInfo", i_InetGetDHCPConfigInfo },
};
const unsigned oti_export_count = sizeof oti_exports / sizeof oti_exports[0];
