/*
 * Open Transport's port registry and its registered clients.
 *
 * A port is a network adapter Open Transport can run protocols on.  The
 * ports here are the host's: one Ethernet port for each of its Ethernet-like
 * adapters (the first, the one the default route leaves by, is "enet"), so a
 * program that lists them (OTGetIndexedPort, OTFindPort) sees a Mac with
 * Ethernet, and a program that registers its own (OTRegisterPort) sees it
 * listed beside them.  OTScanPorts looks at the host again.  Programs that
 * called OTRegisterAsClient hear of ports coming, going, and changing state,
 * and of the TCP/IP stack loading, as Open Transport told its clients.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "ot_internal.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

enum {
    kOTPortDisabled = 0x25000001, kOTPortEnabled = 0x25000002, kOTPortOffline = 0x25000003,
    kOTPortOnline = 0x25000004, kOTNewPortRegistered = 0x25000006, kOTPortNetworkChange = 0x25000007,
    kOTStackIsLoading = 0x27000001, kOTStackWasLoaded = 0x27000002,
    kOTMotherboardBus = 1, kOTEthernetDevice = 10,
    kOTPortIsActive = 1, kOTPortIsDisabled = 2, kOTPortIsUnavailable = 4, kOTPortIsOffline = 8,
    kOTPortIsDLPI = 1, kOTPortIsSystemRegistered = 0x4000,
};

enum { kPortRecordSize = 296, MAX_PORTS = 32, MAX_CLIENTS = 16, MAX_CLIENT_EVENTS = 64 };

typedef struct {
    bool used, host;
    uint32_t ref, portflags, infoflags, caps;
    char name[36], module[32], slot[8], resinfo[32];
    char user[64];
    gaddr regref;
    char ifname[16];
    uint32_t addr;
} port_t;

static port_t ports[MAX_PORTS];
static bool scanned;
static uint64_t last_scan_ns;

typedef struct {
    bool used, m68k;
    gaddr proc;
    uint8_t name[32]; /* the Pascal string */
} client_t;
static client_t clients[MAX_CLIENTS];

typedef struct {
    uint32_t code;
    int32_t result;
    gaddr cookie;
} client_event;
static client_event cevents[MAX_CLIENT_EVENTS];
static int ncevents;
static bool stack_loaded, delivering_clients;

/* ---- The ports ---- */

static port_t *port_by_name(const char *name)
{
    for (int i = 0; i < MAX_PORTS; i++)
        if (ports[i].used && !strcmp(ports[i].name, name))
            return &ports[i];
    return NULL;
}

static port_t *port_by_ref(uint32_t ref)
{
    for (int i = 0; i < MAX_PORTS; i++)
        if (ports[i].used && ports[i].ref == ref)
            return &ports[i];
    return NULL;
}

static port_t *port_new(void)
{
    for (int i = 0; i < MAX_PORTS; i++)
        if (!ports[i].used) {
            memset(&ports[i], 0, sizeof ports[i]);
            ports[i].used = true;
            return &ports[i];
        }
    return NULL;
}

/* A name no port has yet: the base, then the base with 1, 2, ... */
static void unique_name(char name[36])
{
    char base[36];
    snprintf(base, sizeof base, "%s", name);
    if (!base[0])
        snprintf(base, sizeof base, "port");
    for (int n = 0; n < 1000; n++) {
        char num[8] = "";
        if (n)
            snprintf(num, sizeof num, "%d", n);
        size_t room = sizeof base - 1 - strlen(num);
        snprintf(name, 36, "%.*s%s", (int)(strlen(base) < room ? strlen(base) : room), base, num);
        if (!port_by_name(name))
            return;
    }
}

static void host_flags(port_t *p, const ot_ifinfo *e)
{
    p->portflags = 0;
    if (e->up && e->addr)
        p->portflags |= kOTPortIsActive;
    if (!e->up || !e->running)
        p->portflags |= kOTPortIsOffline;
    p->addr = e->addr;
}

/* The host's adapters as ports: new ones added (and announced if this is a
 * rescan), changed ones updated, ones that have gone removed. */
static void scan_host(bool announce)
{
    ot_ifinfo ifs[16];
    int n = ot_host_interfaces(ifs, 16);
    bool seen[MAX_PORTS] = { false };
    for (int i = 0; i < n; i++) {
        port_t *p = NULL;
        for (int k = 0; k < MAX_PORTS && !p; k++)
            if (ports[k].used && ports[k].host && !strcmp(ports[k].ifname, ifs[i].name))
                p = &ports[k];
        if (p) {
            uint32_t was = p->portflags, wasaddr = p->addr;
            host_flags(p, &ifs[i]);
            seen[p - ports] = true;
            if (announce && ((was ^ p->portflags) & kOTPortIsOffline))
                ot_clients_post((p->portflags & kOTPortIsOffline) ? kOTPortOffline : kOTPortOnline, 0, p->ref);
            else if (announce && wasaddr != p->addr)
                ot_clients_post(kOTPortNetworkChange, 0, p->ref);
            continue;
        }
        p = port_new();
        if (!p)
            break;
        p->host = true;
        snprintf(p->ifname, sizeof p->ifname, "%s", ifs[i].name);
        snprintf(p->name, sizeof p->name, "enet");
        p->used = false; /* not yet listed, so its own name doesn't count */
        unique_name(p->name);
        p->used = true;
        snprintf(p->module, sizeof p->module, "enet");
        snprintf(p->slot, sizeof p->slot, "%s", i == 0 ? "" : ifs[i].name);
        snprintf(p->resinfo, sizeof p->resinfo, "Ethernet");
        if (i == 0)
            snprintf(p->user, sizeof p->user, "Ethernet (built-in)");
        else
            snprintf(p->user, sizeof p->user, "Ethernet (%s)", ifs[i].name);
        p->ref = ot_make_port_ref(kOTMotherboardBus, kOTEthernetDevice, 0, (uint32_t)i);
        p->infoflags = kOTPortIsDLPI | kOTPortIsSystemRegistered;
        host_flags(p, &ifs[i]);
        seen[p - ports] = true;
        if (announce)
            ot_clients_post(kOTNewPortRegistered, 0, p->ref);
    }
    for (int k = 0; k < MAX_PORTS; k++)
        if (ports[k].used && ports[k].host && !seen[k])
            ports[k].used = false;
}

static void ensure_scanned(void)
{
    if (!scanned) {
        scanned = true;
        scan_host(false);
    }
}

/* The port record in guest memory: the 296 bytes OTPortRecord is. */
static void put_port(gaddr rec, const port_t *p)
{
    gmemset(rec, 0, kPortRecordSize);
    wr32(rec, p->ref);
    wr32(rec + 4, p->portflags);
    wr32(rec + 8, p->infoflags);
    wr32(rec + 12, p->caps);
    gput(rec + 24, p->name, (uint32_t)strnlen(p->name, 35));
    gput(rec + 60, p->module, (uint32_t)strnlen(p->module, 31));
    gput(rec + 92, p->slot, (uint32_t)strnlen(p->slot, 7));
    gput(rec + 100, p->resinfo, (uint32_t)strnlen(p->resinfo, 31));
}

static void get_str(char *out, size_t size, gaddr a, size_t max)
{
    size_t i = 0;
    for (; i < max && i < size - 1 && rd8(a + (uint32_t)i); i++)
        out[i] = (char)rd8(a + (uint32_t)i);
    out[i] = 0;
}

static void guest_cstr(char *out, size_t size, gaddr a)
{
    out[0] = 0;
    if (a)
        get_str(out, size, a, size - 1);
}

uint32_t ot_default_port_ref(void)
{
    ensure_scanned();
    for (int i = 0; i < MAX_PORTS; i++)
        if (ports[i].used)
            return ports[i].ref;
    return 0;
}

/* OTGetIndexedPort(portRecord, index): Boolean */
static void p_GetIndexedPort(ppc_cpu *c)
{
    ensure_scanned();
    uint32_t want = R(1), seen = 0;
    for (int i = 0; i < MAX_PORTS; i++) {
        if (!ports[i].used)
            continue;
        if (seen++ == want) {
            if (R(0))
                put_port(R(0), &ports[i]);
            RETURN(1);
            return;
        }
    }
    RETURN(0);
}

/* OTFindPort(portRecord, portName), OTFindPortByRef(portRecord, ref) */
static void p_FindPort(ppc_cpu *c)
{
    ensure_scanned();
    char name[40];
    guest_cstr(name, sizeof name, R(1));
    port_t *p = port_by_name(name);
    if (p && R(0))
        put_port(R(0), p);
    RETURN(p != NULL);
}

static void p_FindPortByRef(ppc_cpu *c)
{
    ensure_scanned();
    port_t *p = port_by_ref(R(1));
    if (p && R(0))
        put_port(R(0), p);
    RETURN(p != NULL);
}

/* OTRegisterPort(portRecord, ref): the port listed, under its name or the
 * next free one, which is written back to fPortName. */
static void p_RegisterPort(ppc_cpu *c)
{
    ensure_scanned();
    gaddr rec = R(0);
    if (!rec) {
        RETURN((uint32_t)kOTBadReferenceErr);
        return;
    }
    port_t *p = port_new();
    if (!p) {
        RETURN((uint32_t)kOTOutOfMemoryErr);
        return;
    }
    p->ref = rd32(rec);
    p->portflags = rd32(rec + 4);
    p->infoflags = rd32(rec + 8);
    p->caps = rd32(rec + 12);
    get_str(p->name, sizeof p->name, rec + 24, 35);
    get_str(p->module, sizeof p->module, rec + 60, 31);
    get_str(p->slot, sizeof p->slot, rec + 92, 7);
    get_str(p->resinfo, sizeof p->resinfo, rec + 100, 31);
    p->used = false; /* not yet, so its own name is free to compare against */
    unique_name(p->name);
    p->used = true;
    p->regref = R(1);
    snprintf(p->user, sizeof p->user, "%s", p->name);
    gmemset(rec + 24, 0, 36);
    gput(rec + 24, p->name, (uint32_t)strlen(p->name));
    ot_clients_post(kOTNewPortRegistered, 0, p->ref);
    RETURN(kOTNoError);
}

/* OTUnregisterPort(portName, void **ref): the port removed, its ref given
 * back. */
static void p_UnregisterPort(ppc_cpu *c)
{
    ensure_scanned();
    char name[40];
    guest_cstr(name, sizeof name, R(0));
    port_t *p = port_by_name(name);
    if (!p) {
        RETURN((uint32_t)kOTNotFoundErr);
        return;
    }
    if (R(1))
        wr32(R(1), p->regref);
    p->used = false;
    RETURN(kOTNoError);
}

/* OTChangePortState(portRef, event, why): the port's state flags changed and
 * every client told. */
static void p_ChangePortState(ppc_cpu *c)
{
    ensure_scanned();
    port_t *p = port_by_ref(R(0));
    uint32_t ev = R(1);
    if (!p) {
        RETURN((uint32_t)kOTNotFoundErr);
        return;
    }
    switch (ev) {
    case kOTPortDisabled: p->portflags |= kOTPortIsDisabled; p->portflags &= ~(uint32_t)kOTPortIsActive; break;
    case kOTPortEnabled: p->portflags &= ~(uint32_t)kOTPortIsDisabled; break;
    case kOTPortOffline: p->portflags |= kOTPortIsOffline; break;
    case kOTPortOnline: p->portflags &= ~(uint32_t)kOTPortIsOffline; break;
    case kOTPortNetworkChange: break;
    default:
        RETURN((uint32_t)kOTNotSupportedErr);
        return;
    }
    ot_clients_post(ev, (int32_t)R(2), p->ref);
    RETURN(kOTNoError);
}

/* OTScanPorts(): look at the host's adapters again. */
static void p_ScanPorts(ppc_cpu *c)
{
    ensure_scanned();
    scan_host(true);
    last_scan_ns = machine_time_ns();
    RETURN(kOTNoError);
}

/* OTGetUserPortNameFromPortRef(ref, Str255): the name a person would call
 * it, empty when there's no such port. */
static void p_GetUserPortName(ppc_cpu *c)
{
    ensure_scanned();
    port_t *p = port_by_ref(R(0));
    if (R(1)) {
        const char *s = p ? p->user : "";
        size_t n = strlen(s) > 255 ? 255 : strlen(s);
        wr8(R(1), (uint8_t)n);
        gput(R(1) + 1, s, (uint32_t)n);
    }
    c->pc = c->lr;
}

/* OTGetPortIconFromPortRef(ref, OTResourceLocator *): the file and resource
 * ID of the port's icon family.  The host's adapters have none to point at. */
static void p_GetPortIcon(ppc_cpu *c) { RETURN(0); }

/* OTIsPortCompatibleWith(port, protocolName): whether the port can carry
 * the protocol: network adapters carry the network protocols, serial and
 * modem ports the ones that run over a link, and any other port only the
 * protocol of its own name. */
static void p_IsPortCompatibleWith(ppc_cpu *c)
{
    char proto[40];
    guest_cstr(proto, sizeof proto, R(1));
    char module[32] = "";
    if (R(0))
        get_str(module, sizeof module, R(0) + 60, 31);
    static const char *const net[] = { "enet", "enet8022x", "fenet", "tokn", "fddi", "atm", "firewire", "fibre", NULL };
    static const char *const netprotos[] = { "tcp", "udp", "ip", "rawip", "dnr", "ddp", "adsp", "atp", "asp", "pap",
                                             "nbp", "zip", "aarp", "atlk", "tpi8022x", "dlpi", "ipx", NULL };
    static const char *const linkprotos[] = { "ppp", "slip", "ip", "tcp", "udp", "rawip", "dnr", "ltlk", "atlk", "ddp",
                                              "adsp", "atp", "nbp", "zip", NULL };
    bool ok = !strcmp(proto, module);
    for (int i = 0; !ok && net[i]; i++)
        if (!strcmp(module, net[i]))
            for (int k = 0; !ok && netprotos[k]; k++)
                ok = !strcmp(proto, netprotos[k]);
    if (!ok && (!strcmp(module, "serial") || !strcmp(module, "modem") || !strcmp(module, "serialAB") ||
                !strcmp(module, "ppp")))
        for (int k = 0; !ok && linkprotos[k]; k++)
            ok = !strcmp(proto, linkprotos[k]);
    RETURN(ok);
}

/* OTIsDependentPort(parent, child): whether child is among the ports parent
 * is built on; the host's ports have no children. */
static void p_IsDependentPort(ppc_cpu *c) { RETURN(0); }

/* OTGetProviderPortRef(ref): the port a provider runs on. */
static void p_GetProviderPortRef(ppc_cpu *c)
{
    provider *p = ot_provider_of(R(0));
    RETURN(p ? ot_default_port_ref() : 0);
}

/* ---- Clients ---- */

/* OTRegisterAsClient(name, proc): proc hears of events for all clients.  The
 * name is a Pascal string; a name already registered has its proc replaced. */
static void p_RegisterAsClient(ppc_cpu *c)
{
    gaddr name = R(0);
    client_t *slot = NULL;
    uint8_t buf[32] = { 0 };
    if (name) {
        buf[0] = rd8(name) > 31 ? 31 : rd8(name);
        for (int i = 1; i <= buf[0]; i++)
            buf[i] = rd8(name + (uint32_t)i);
    }
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].used && !memcmp(clients[i].name, buf, 32))
            slot = &clients[i];
    for (int i = 0; i < MAX_CLIENTS && !slot; i++)
        if (!clients[i].used)
            slot = &clients[i];
    if (!slot) {
        RETURN((uint32_t)kOTOutOfMemoryErr);
        return;
    }
    slot->used = true;
    slot->proc = R(1);
    slot->m68k = ot_from_68k;
    memcpy(slot->name, buf, 32);
    RETURN(kOTNoError);
}

/* OTUnregisterAsClient(): the program's registration ends (the most recent
 * one, a program having one). */
static void p_UnregisterAsClient(ppc_cpu *c)
{
    for (int i = MAX_CLIENTS - 1; i >= 0; i--)
        if (clients[i].used) {
            clients[i].used = false;
            RETURN(kOTNoError);
            return;
        }
    RETURN((uint32_t)kOTNotFoundErr);
}

void ot_clients_post(uint32_t code, int32_t result, gaddr cookie)
{
    bool any = false;
    for (int i = 0; i < MAX_CLIENTS; i++)
        any |= clients[i].used;
    if (any && ncevents < MAX_CLIENT_EVENTS)
        cevents[ncevents++] = (client_event){ code, result, cookie };
}

/* OTNotifyAllClients(code, result, cookie) */
static void p_NotifyAllClients(ppc_cpu *c)
{
    ot_clients_post(R(0), (int32_t)R(1), R(2));
    c->pc = c->lr;
}

void ot_stack_loading(void)
{
    if (stack_loaded)
        return;
    stack_loaded = true;
    ot_clients_post(kOTStackIsLoading, 0, 0);
    ot_clients_post(kOTStackWasLoaded, 0, 0);
}

/* The registered clients told what's waiting; the host's adapters looked at
 * again every few seconds while there are any. */
void ot_ports_poll(void)
{
    if (delivering_clients)
        return;
    bool any = false;
    for (int i = 0; i < MAX_CLIENTS; i++)
        any |= clients[i].used;
    if (!any) {
        ncevents = 0;
        return;
    }
    uint64_t now = machine_time_ns();
    if (scanned && now - last_scan_ns > 3000000000ull) {
        last_scan_ns = now;
        scan_host(true);
    }
    delivering_clients = true;
    for (int k = 0; k < ncevents && !g_cpu->halted; k++) {
        client_event e = cevents[k];
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].used || !clients[i].proc)
                continue;
            uint32_t args[4] = { 0, e.code, (uint32_t)e.result, e.cookie };
            ot_callproc_as(clients[i].m68k, clients[i].proc, 0x00003FC0 /* Pascal, four longs */, args, 4);
        }
    }
    ncevents = 0;
    delivering_clients = false;
}

void ot_ports_reset(void)
{
    memset(ports, 0, sizeof ports);
    memset(clients, 0, sizeof clients);
    ncevents = 0;
    scanned = stack_loaded = delivering_clients = false;
    last_scan_ns = 0;
}

const ppc_export otp_exports[] = {
    { "OTGetIndexedPort", p_GetIndexedPort }, { "OTFindPort", p_FindPort }, { "OTFindPortByRef", p_FindPortByRef },
    { "OTRegisterPort", p_RegisterPort }, { "OTUnregisterPort", p_UnregisterPort },
    { "OTChangePortState", p_ChangePortState }, { "OTScanPorts", p_ScanPorts },
    { "OTGetUserPortNameFromPortRef", p_GetUserPortName }, { "OTGetPortIconFromPortRef", p_GetPortIcon },
    { "OTIsPortCompatibleWith", p_IsPortCompatibleWith }, { "OTIsDependentPort", p_IsDependentPort },
    { "OTGetProviderPortRef", p_GetProviderPortRef },
    { "OTRegisterAsClient", p_RegisterAsClient }, { "OTRegisterAsClientPriv", p_RegisterAsClient },
    { "OTRegisterAsClientInContext", p_RegisterAsClient },
    { "OTUnregisterAsClient", p_UnregisterAsClient }, { "OTUnregisterAsClientPriv", p_UnregisterAsClient },
    { "OTUnregisterAsClientInContext", p_UnregisterAsClient },
    { "OTNotifyAllClients", p_NotifyAllClients },
};
const unsigned otp_export_count = sizeof otp_exports / sizeof otp_exports[0];
