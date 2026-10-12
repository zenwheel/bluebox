/*
 * MIDI Manager (SoundDispatch tool 4).
 *
 * Clients sign in and add ports as usual, but there is no MIDI hardware and
 * no Patchbay: nothing is ever connected, so data written goes nowhere and
 * no input arrives.  Time base ports work, since applications (Director)
 * keep their own time with them: a started port counts milliseconds from
 * its initial clock, and MIDIWakeUp's time procedure runs at interrupt time
 * once the port's time reaches the wake-up time, then every period.
 */
#include <string.h>

#include "m68k.h"
#include "tb_internal.h"
#include "trap.h"

#define MAX_CLIENTS 16
#define MAX_PORTS 32
#define FIRST_REFNUM 1

enum {
    midiPortTypeTime = 0,
    midiFormatMSec = 0,
    midiInvalidClientErr = -251,
    midiInvalidPortErr = -252,
    midiTooManyPortsErr = -254,
    midiTooManyConsErr = -255,
};

typedef struct {
    bool used;
    uint32_t id;
    uint32_t refcon;
    gaddr icon;
    uint8_t name[256];
} client;

typedef struct {
    bool used;
    unsigned owner;      /* client index */
    uint32_t id;
    int16_t type;
    uint32_t refcon;
    gaddr read_hook;
    int16_t sync, format;
    int32_t offset;
    uint8_t name[256];
    /* Time base. */
    bool running;
    int64_t base_ms;     /* time when stopped, or time at start_ns */
    uint64_t start_ns;
    bool wake_set;
    int32_t wake_time, period;
    gaddr time_proc;
} port;

static client clients[MAX_CLIENTS];
static port ports[MAX_PORTS];
static bool in_pump;

static int find_client(uint32_t id)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].used && clients[i].id == id)
            return i;
    return -1;
}

static port *port_of(int16_t refnum)
{
    int i = refnum - FIRST_REFNUM;
    return i >= 0 && i < MAX_PORTS && ports[i].used ? &ports[i] : NULL;
}

static port *find_port(uint32_t client_id, uint32_t port_id)
{
    int ci = find_client(client_id);
    for (int i = 0; ci >= 0 && i < MAX_PORTS; i++)
        if (ports[i].used && ports[i].owner == (unsigned)ci && ports[i].id == port_id)
            return &ports[i];
    return NULL;
}

static int32_t port_time(const port *p)
{
    if (!p->running)
        return (int32_t)p->base_ms;
    return (int32_t)(p->base_ms + (int64_t)((machine_time_ns() - p->start_ns) / 1000000u));
}

static void set_port_time(port *p, int32_t t)
{
    p->base_ms = t;
    p->start_ns = machine_time_ns();
}

static void copy_name(uint8_t *dst, gaddr src)
{
    unsigned n = src ? rd8(src) : 0;
    dst[0] = (uint8_t)n;
    for (unsigned i = 0; i < n; i++)
        dst[1 + i] = rd8(src + 1 + i);
}

static void put_name(gaddr dst, const uint8_t *src)
{
    if (dst)
        gput(dst, src, src[0] + 1u);
}

/* An IDList handle: count, then the OSTypes. */
static gaddr id_list(const uint32_t *ids, unsigned n)
{
    gaddr h = tb_NewHandle((int32_t)(2 + 4 * n), false, true);
    if (!h || !rd32(h))
        return h;
    wr16(rd32(h), (uint16_t)n);
    for (unsigned i = 0; i < n; i++)
        wr32(rd32(h) + 2 + 4 * i, ids[i]);
    return h;
}

/* ---- Clients ---- */

int32_t tb_DirectorUnknown3(void) { return 0x02008000; } /* MIDI Manager 2.0 */

/* MIDISignIn(clientID, refCon, icon, name): OSErr */
int16_t tb_FinaleUnknown2(int32_t id, int32_t refcon, gaddr icon, gaddr name)
{
    int ci = find_client((uint32_t)id);
    if (ci < 0) {
        for (int i = 0; i < MAX_CLIENTS && ci < 0; i++)
            if (!clients[i].used)
                ci = i;
        if (ci < 0)
            return midiTooManyPortsErr;
    }
    client *c = &clients[ci];
    c->used = true;
    c->id = (uint32_t)id;
    c->refcon = (uint32_t)refcon;
    c->icon = icon;
    copy_name(c->name, name);
    return noErr;
}

void tb_MIDISignOut(int32_t id)
{
    int ci = find_client((uint32_t)id);
    if (ci < 0)
        return;
    for (int i = 0; i < MAX_PORTS; i++)
        if (ports[i].used && ports[i].owner == (unsigned)ci)
            ports[i].used = false;
    clients[ci].used = false;
}

gaddr tb_MIDIGetClients(void)
{
    uint32_t ids[MAX_CLIENTS];
    unsigned n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].used)
            ids[n++] = clients[i].id;
    return id_list(ids, n);
}

void tb_MIDIGetClientName(int32_t id, gaddr name)
{
    int ci = find_client((uint32_t)id);
    if (ci >= 0)
        put_name(name, clients[ci].name);
    else if (name)
        wr8(name, 0);
}

void tb_MIDISetClientName(int32_t id, gaddr name)
{
    int ci = find_client((uint32_t)id);
    if (ci >= 0)
        copy_name(clients[ci].name, name);
}

gaddr tb_MIDIGetClientIcon(int32_t id)
{
    int ci = find_client((uint32_t)id);
    return ci >= 0 ? clients[ci].icon : 0;
}

int32_t tb_MIDIGetClRefCon(int32_t id)
{
    int ci = find_client((uint32_t)id);
    return ci >= 0 ? (int32_t)clients[ci].refcon : 0;
}

void tb_MIDISetClRefCon(int32_t id, int32_t refcon)
{
    int ci = find_client((uint32_t)id);
    if (ci >= 0)
        clients[ci].refcon = (uint32_t)refcon;
}

int8_t tb_MIDIWorldChanged(int32_t id) { return 0; }

/* ---- Ports ---- */

/* MIDIAddPort(clientID, bufSize, VAR refnum, init): OSErr.  MIDIPortParams:
 * portID, portType, timeBase, offsetTime, readHook, refCon, initClock
 * (sync, curTime, format), name. */
int16_t tb_DirectorUnknown4(int32_t id, int16_t bufsize, gaddr refnum, gaddr init)
{
    int ci = find_client((uint32_t)id);
    if (ci < 0)
        return midiInvalidClientErr;
    int pi = -1;
    for (int i = 0; i < MAX_PORTS && pi < 0; i++)
        if (!ports[i].used)
            pi = i;
    if (pi < 0)
        return midiTooManyPortsErr;
    port *p = &ports[pi];
    memset(p, 0, sizeof *p);
    p->used = true;
    p->owner = (unsigned)ci;
    if (init) {
        p->id = rd32(init);
        p->type = (int16_t)rd16(init + 4);
        p->offset = (int32_t)rd32(init + 8);
        p->read_hook = rd32(init + 12);
        p->refcon = rd32(init + 16);
        p->sync = (int16_t)rd16(init + 20);
        p->base_ms = (int32_t)rd32(init + 22);
        p->format = (int16_t)rd16(init + 26);
        copy_name(p->name, init + 28);
    }
    if (refnum)
        wr16(refnum, (uint16_t)(pi + FIRST_REFNUM));
    return noErr;
}

void tb_MIDIRemovePort(int16_t refnum)
{
    port *p = port_of(refnum);
    if (p)
        p->used = false;
}

gaddr tb_MIDIGetPorts(int32_t id)
{
    uint32_t ids[MAX_PORTS];
    unsigned n = 0;
    int ci = find_client((uint32_t)id);
    for (int i = 0; ci >= 0 && i < MAX_PORTS; i++)
        if (ports[i].used && ports[i].owner == (unsigned)ci)
            ids[n++] = ports[i].id;
    return id_list(ids, n);
}

/* MIDIPortInfo: portType, timeBase (MIDIPortInfo-less: client, port),
 * numConnects, then connections.  Nothing is connected. */
gaddr tb_MIDIGetPortInfo(int32_t id, int32_t port_id)
{
    port *p = find_port((uint32_t)id, (uint32_t)port_id);
    if (!p)
        return 0;
    gaddr h = tb_NewHandle(14, false, true);
    if (h && rd32(h))
        wr16(rd32(h), (uint16_t)p->type);
    return h;
}

void tb_MIDIGetPortName(int32_t id, int32_t port_id, gaddr name)
{
    port *p = find_port((uint32_t)id, (uint32_t)port_id);
    if (p)
        put_name(name, p->name);
    else if (name)
        wr8(name, 0);
}

void tb_MIDISetPortName(int32_t id, int32_t port_id, gaddr name)
{
    port *p = find_port((uint32_t)id, (uint32_t)port_id);
    if (p)
        copy_name(p->name, name);
}

/* Connections need another client's port: with no Patchbay or drivers
 * there is none, apart from our own ports, which we let connect. */
static int16_t connect(int32_t src_client, int32_t src_port, int32_t dst_client, int32_t dst_port)
{
    if (!find_port((uint32_t)src_client, (uint32_t)src_port) || !find_port((uint32_t)dst_client, (uint32_t)dst_port))
        return midiInvalidPortErr;
    return noErr;
}

int16_t tb_MIDIConnectData(int32_t a, int32_t b, int32_t c, int32_t d) { return connect(a, b, c, d); }
int16_t tb_MIDIUnConnectData(int32_t a, int32_t b, int32_t c, int32_t d) { return connect(a, b, c, d); }
int16_t tb_MIDIConnectTime(int32_t a, int32_t b, int32_t c, int32_t d) { return connect(a, b, c, d); }
int16_t tb_MIDIUnConnectTime(int32_t a, int32_t b, int32_t c, int32_t d) { return connect(a, b, c, d); }

void tb_MIDIFlush(int16_t refnum) {}
void tb_MIDIPoll(int16_t refnum, int32_t offset) {}
int16_t tb_MIDIWritePacket(int16_t refnum, gaddr packet) { return port_of(refnum) ? noErr : midiInvalidPortErr; }

gaddr tb_MIDIGetReadHook(int16_t refnum)
{
    port *p = port_of(refnum);
    return p ? p->read_hook : 0;
}

void tb_MIDISetReadHook(int16_t refnum, gaddr hook)
{
    port *p = port_of(refnum);
    if (p)
        p->read_hook = hook;
}

int32_t tb_MIDIGetRefCon(int16_t refnum)
{
    port *p = port_of(refnum);
    return p ? (int32_t)p->refcon : 0;
}

void tb_MIDISetRefCon(int16_t refnum, int32_t refcon)
{
    port *p = port_of(refnum);
    if (p)
        p->refcon = (uint32_t)refcon;
}

int16_t tb_MIDIGetSync(int16_t refnum)
{
    port *p = port_of(refnum);
    return p ? p->sync : 0;
}

void tb_MIDISetSync(int16_t refnum, int16_t sync)
{
    port *p = port_of(refnum);
    if (p)
        p->sync = sync;
}

int16_t tb_MIDIGetTCFormat(int16_t refnum)
{
    port *p = port_of(refnum);
    return p ? p->format : 0;
}

void tb_MIDISetTCFormat(int16_t refnum, int16_t format)
{
    port *p = port_of(refnum);
    if (p)
        p->format = format;
}

int32_t tb_MIDIGetOffsetTime(int16_t refnum)
{
    port *p = port_of(refnum);
    return p ? p->offset : 0;
}

void tb_MIDISetOffsetTime(int16_t refnum, int32_t offset)
{
    port *p = port_of(refnum);
    if (p)
        p->offset = offset;
}

/* ---- Time ---- */

int32_t tb_MIDIGetCurTime(int16_t refnum)
{
    port *p = port_of(refnum);
    return p ? port_time(p) : 0;
}

void tb_MIDISetCurTime(int16_t refnum, int32_t time)
{
    port *p = port_of(refnum);
    if (p)
        set_port_time(p, time);
}

void tb_MIDIStartTime(int16_t refnum)
{
    port *p = port_of(refnum);
    if (p && !p->running) {
        p->start_ns = machine_time_ns();
        p->running = true;
    }
}

void tb_MIDIStopTime(int16_t refnum)
{
    port *p = port_of(refnum);
    if (p && p->running) {
        p->base_ms = port_time(p);
        p->running = false;
    }
}

void tb_MIDISetRunRate(int16_t refnum, int16_t rate, int32_t time)
{
    port *p = port_of(refnum);
    if (!p)
        return;
    set_port_time(p, time);
    if (rate)
        tb_MIDIStartTime(refnum);
    else
        tb_MIDIStopTime(refnum);
}

/* Formats other than milliseconds are SMPTE frames and beats; we keep
 * milliseconds and convert nothing else. */
int32_t tb_MIDIConvertTime(int16_t src_format, int16_t dst_format, int32_t time) { return time; }

void tb_MIDIWakeUp(int16_t refnum, int32_t time, int32_t period, gaddr proc)
{
    port *p = port_of(refnum);
    if (!p)
        return;
    p->wake_time = time;
    p->period = period;
    p->time_proc = proc;
    p->wake_set = proc != 0;
}

/* From time_pump: run due time procedures, PROCEDURE (curTime, refCon). */
void midi_pump(void)
{
    if (in_pump)
        return;
    in_pump = true;
    for (int i = 0; i < MAX_PORTS; i++) {
        port *p = &ports[i];
        if (!p->used || !p->running || !p->wake_set)
            continue;
        int32_t now = port_time(p);
        if (now < p->wake_time)
            continue;
        if (p->period > 0) {
            p->wake_time += p->period;
            if (p->wake_time <= now) /* fell behind: don't run a backlog */
                p->wake_time = now + p->period;
        } else {
            p->wake_set = false;
        }
        /* As an interrupt handler: every register preserved. */
        cpu68k *c = g_cpu;
        uint32_t d[8], a[8];
        uint16_t sr = c->sr;
        memcpy(d, c->d, sizeof d);
        memcpy(a, c->a, sizeof a);
        c->sr = (uint16_t)((c->sr & ~0x0700) | 0x0100);
        cpu_push32(c, (uint32_t)now);
        cpu_push32(c, p->refcon);
        guest_call(p->time_proc);
        memcpy(c->d, d, sizeof d);
        memcpy(c->a, a, sizeof a);
        c->sr = sr;
    }
    in_pump = false;
}

void midi_reset(void)
{
    memset(clients, 0, sizeof clients);
    memset(ports, 0, sizeof ports);
    in_pump = false;
}
