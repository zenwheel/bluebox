/*
 * MIDI Manager (SoundDispatch tool 4).
 *
 * Clients sign in and add ports; an output port's packets go to the input
 * ports it is connected to, whose read hooks take them at interrupt time
 * once they are due (by time stamp and the port's offset time), or at
 * MIDIPoll.  Time base ports count milliseconds from their initial clock,
 * and MIDIWakeUp's time procedure runs at interrupt time once the port's
 * time reaches the wake-up time, then every period.
 *
 * There is no PatchBay.  In its place a client of our own, 'host', stands
 * where the Apple MIDI Driver did: what reaches its input port goes to the
 * host (MIDI destinations, else the General MIDI synthesizer), and what the
 * host's MIDI sources play leaves its output port.  Every port another
 * client adds is connected to it, as PatchBay would have remembered, and
 * the client may undo that with MIDIUnConnectData.
 */
#include <pthread.h>
#include <string.h>

#include "m68k.h"
#include "machine.h"
#include "tb_internal.h"
#include "trap.h"

#define MAX_CLIENTS 16
#define MAX_PORTS 32
#define MAX_CONNS 128
#define FIRST_REFNUM 1
#define QLEN 64                    /* packets waiting at an input port */
#define HOST_CLIENT 0x686F7374u    /* 'host' */
#define HOST_IN 0x696E2020u        /* 'in  ': to the host */
#define HOST_OUT 0x6F757420u       /* 'out ': from the host */
#define HOST_SYNTH 0x4D490000      /* 'MI', the host synthesizer it plays */

enum {
    midiPortTypeTime = 0,
    midiPortTypeInput = 1,
    midiPortTypeOutput = 2,
    midiPortTypeTimeInv = 3,
    midiKeepPacket = 0,
    midiMorePacket = 1,
    midiNoMorePacket = 2,
    midiGetEverything = 0x7FFFFFFF,
    /* MIDIPacket: flags, len (the header's 6 bytes included), tcStamp, data */
    midiContMask = 0x03,
    midiStartCont = 0x01,
    midiMidCont = 0x03,
    midiEndCont = 0x02,
    midiTimeStampCurrent = 0x80,
    PKT_HDR = 6,
    PKT_MAX = 255,
    midiNoClientErr = -250,
    midiNoPortErr = -251,
    midiTooManyPortsErr = -252,
    midiTooManyConsErr = -253,
    midiNoConErr = -257,
    midiDupIDErr = -260,
    midiInvalidCmdErr = -261,
};
#define midiGetNothing ((int32_t)0x80000000)

typedef struct {
    bool used;
    uint32_t id;
    uint32_t refcon;
    gaddr icon;
    uint8_t name[256];
} client;

typedef struct {
    uint64_t due;              /* machine_time_ns when it is due */
    uint8_t pkt[PKT_MAX + 1];
} queued;

typedef struct {
    bool used;
    unsigned owner;      /* client index */
    uint32_t id;
    int16_t type;
    int16_t timebase;    /* refnum of the time port it keeps time by, or 0 */
    uint32_t refcon;
    gaddr read_hook;
    gaddr conn_proc;     /* MIDISetConnectionProc's, and its refCon */
    uint32_t conn_refcon;
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
    /* Input: packets waiting, and the one the read hook is given. */
    queued q[QLEN];
    int qhead, qn;
    gaddr packet;
    bool delivering;
} port;

typedef struct {
    bool used, time;
    uint8_t src, dst;    /* port indexes */
} conn;

/* MIDI bytes into whole messages: running status, System Exclusive
 * gathered up to `chunk` bytes at a time, real-time bytes on their own. */
typedef struct {
    uint8_t status;
    int need;            /* data bytes the message takes */
    uint8_t buf[4096];
    int n, chunk;
    bool sysex, sysex_cont;
} parser;
typedef void (*emit_fn)(const uint8_t *msg, int n, int cont);

static client clients[MAX_CLIENTS];
static port ports[MAX_PORTS];
static conn conns[MAX_CONNS];
static bool in_pump;
static int host_in = -1, host_out = -1;
static parser out_parser, in_parser;

/* From the host's MIDI sources, on any thread. */
static pthread_mutex_t in_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t in_bytes[8192];
static int in_len;

static machine_midi_out_fn out_fn;
static void *out_ctx;

void machine_set_midi_out_handler(machine_midi_out_fn fn, void *ctx)
{
    out_fn = fn;
    out_ctx = ctx;
}

void machine_midi_in(const uint8_t *bytes, int n)
{
    pthread_mutex_lock(&in_lock);
    if (n > (int)sizeof in_bytes - in_len)
        n = (int)sizeof in_bytes - in_len; /* nobody is reading: drop the rest */
    if (n > 0) {
        memcpy(in_bytes + in_len, bytes, (size_t)n);
        in_len += n;
    }
    pthread_mutex_unlock(&in_lock);
}

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

static int16_t refnum_of(const port *p) { return (int16_t)(p - ports + FIRST_REFNUM); }

static port *find_port(uint32_t client_id, uint32_t port_id)
{
    int ci = find_client(client_id);
    for (int i = 0; ci >= 0 && i < MAX_PORTS; i++)
        if (ports[i].used && ports[i].owner == (unsigned)ci && ports[i].id == port_id)
            return &ports[i];
    return NULL;
}

static bool is_time_port(const port *p) { return p->type == midiPortTypeTime || p->type == midiPortTypeTimeInv; }

/* A time port's own clock, or that of the time port it follows; other
 * ports without one keep milliseconds since startup. */
static int32_t port_time_depth(const port *p, int depth)
{
    port *tb = p->timebase ? port_of(p->timebase) : NULL;
    if (tb && tb != p && is_time_port(tb) && depth < 4)
        return port_time_depth(tb, depth + 1);
    if (!is_time_port(p))
        return (int32_t)(machine_time_ns() / 1000000u);
    if (!p->running)
        return (int32_t)p->base_ms;
    return (int32_t)(p->base_ms + (int64_t)((machine_time_ns() - p->start_ns) / 1000000u));
}

static int32_t port_time(const port *p) { return port_time_depth(p, 0); }

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

static void set_name(uint8_t *dst, const char *s)
{
    size_t n = strlen(s);
    dst[0] = (uint8_t)n;
    memcpy(dst + 1, s, n);
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

/* ---- Parsing ---- */

static int data_bytes(uint8_t status)
{
    switch (status & 0xF0) {
    case 0xC0: case 0xD0: return 1;
    case 0xF0:
        return status == 0xF2 ? 2 : status == 0xF1 || status == 0xF3 ? 1 : 0;
    default: return 2;
    }
}

static void parse(parser *ps, const uint8_t *bytes, int n, emit_fn emit)
{
    for (int i = 0; i < n; i++) {
        uint8_t b = bytes[i];
        if (b >= 0xF8) { /* real time: anywhere, even inside System Exclusive */
            emit(&b, 1, 0);
            continue;
        }
        if (ps->sysex && b != 0xF7 && b & 0x80) { /* ended without its F7 */
            emit(ps->buf, ps->n, ps->sysex_cont ? midiEndCont : 0);
            ps->sysex = false;
        }
        if (ps->sysex) {
            ps->buf[ps->n++] = b;
            if (b == 0xF7 || ps->n == ps->chunk) {
                bool end = b == 0xF7;
                emit(ps->buf, ps->n, end ? (ps->sysex_cont ? midiEndCont : 0) : ps->sysex_cont ? midiMidCont : midiStartCont);
                ps->sysex_cont = !end;
                ps->sysex = !end;
                ps->n = 0;
            }
            continue;
        }
        if (b == 0xF0) {
            ps->sysex = true;
            ps->sysex_cont = false;
            ps->buf[0] = b;
            ps->n = 1;
            ps->status = 0;
            continue;
        }
        if (b & 0x80) {
            ps->status = b >= 0xF0 ? 0 : b; /* system common cancels running status */
            ps->need = data_bytes(b);
            ps->buf[0] = b;
            ps->n = 1;
        } else {
            if (!ps->n) { /* running status */
                if (!ps->status)
                    continue; /* stray data */
                ps->buf[0] = ps->status;
                ps->need = data_bytes(ps->status);
                ps->n = 1;
            }
            ps->buf[ps->n++] = b;
        }
        if (ps->n == ps->need + 1) {
            emit(ps->buf, ps->n, 0);
            ps->n = 0;
        }
    }
}

static void reset_parser(parser *ps, int chunk)
{
    memset(ps, 0, sizeof *ps);
    ps->chunk = chunk;
}

/* ---- Delivery ---- */

/* As an interrupt handler: every register preserved, at level 1. */
static void interrupt_regs(uint32_t *d, uint32_t *a, uint16_t *sr)
{
    cpu68k *c = g_cpu;
    memcpy(d, c->d, 8 * sizeof *d);
    memcpy(a, c->a, 8 * sizeof *a);
    *sr = c->sr;
    c->sr = (uint16_t)((c->sr & ~0x0700) | 0x0100);
}

static void restore_regs(const uint32_t *d, const uint32_t *a, uint16_t sr)
{
    cpu68k *c = g_cpu;
    memcpy(c->d, d, 8 * sizeof *d);
    memcpy(c->a, a, 8 * sizeof *a);
    c->sr = sr;
}

/* FUNCTION readHook(packet: MIDIPacketPtr; refCon: LONGINT): INTEGER */
static int16_t call_read_hook(port *p)
{
    cpu68k *c = g_cpu;
    uint32_t d[8], a[8];
    uint16_t sr;
    interrupt_regs(d, a, &sr);
    cpu_push16(c, midiMorePacket);
    cpu_push32(c, p->packet);
    cpu_push32(c, p->refcon);
    guest_call(p->read_hook);
    int16_t r = (int16_t)cpu_pop16(c);
    restore_regs(d, a, sr);
    return r;
}

static void play_on_synth(const uint8_t *msg, int n, int cont)
{
    if (msg[0] >= 0xF0 || n < 2)
        return; /* the synthesizer takes channel messages */
    machine_midi_event e = { 0, msg[0], msg[1], n > 2 ? msg[2] : 0 };
    sound_host_midi(HOST_SYNTH, &e, 1);
}

static void play_on_host(const uint8_t *msg, int n, int cont)
{
    if (!out_fn || !out_fn(msg, n, out_ctx))
        play_on_synth(msg, n, cont);
}

static bool ready(const queued *e, int32_t offset)
{
    if (offset == midiGetEverything)
        return true;
    if (offset == midiGetNothing)
        return false;
    return (int64_t)e->due - (int64_t)offset * 1000000 <= (int64_t)machine_time_ns();
}

/* Hand an input port the packets due by `offset`; the host's input port
 * plays them. */
static void deliver(port *p, int32_t offset)
{
    if (p->delivering)
        return;
    p->delivering = true;
    while (p->qn && p->used) {
        queued *e = &p->q[p->qhead];
        if (!ready(e, offset))
            break;
        int r = midiMorePacket;
        if (p - ports == host_in) {
            parse(&out_parser, e->pkt + PKT_HDR, e->pkt[1] - PKT_HDR, play_on_host);
        } else {
            if (!p->read_hook || !g_cpu)
                break; /* they wait for one */
            if (!p->packet && !(p->packet = tb_NewPtr(PKT_MAX + 1, true, true)))
                break;
            if (e->pkt[0] & midiTimeStampCurrent) {
                uint32_t now = (uint32_t)port_time(p);
                e->pkt[0] &= (uint8_t)~midiTimeStampCurrent;
                e->pkt[2] = (uint8_t)(now >> 24), e->pkt[3] = (uint8_t)(now >> 16);
                e->pkt[4] = (uint8_t)(now >> 8), e->pkt[5] = (uint8_t)now;
            }
            gput(p->packet, e->pkt, e->pkt[1]);
            r = call_read_hook(p);
            if (r == midiKeepPacket)
                break;
        }
        p->qhead = (p->qhead + 1) % QLEN;
        p->qn--;
        if (r == midiNoMorePacket)
            break;
    }
    p->delivering = false;
}

/* A packet leaving output port src: to each input port connected to it,
 * due when its time stamp comes round in src's time. */
static void send_packet(port *src, const uint8_t *pkt)
{
    uint64_t now = machine_time_ns();
    uint64_t due = now;
    if (!(pkt[0] & midiTimeStampCurrent)) {
        int32_t stamp = (int32_t)((uint32_t)pkt[2] << 24 | (uint32_t)pkt[3] << 16 | (uint32_t)pkt[4] << 8 | pkt[5]);
        int64_t ahead = (int64_t)stamp - port_time(src);
        if (ahead > 0)
            due = now + (uint64_t)ahead * 1000000u;
    }
    for (int i = 0; i < MAX_CONNS; i++) {
        if (!conns[i].used || conns[i].time || &ports[conns[i].src] != src)
            continue;
        port *dst = &ports[conns[i].dst];
        if (dst->qn == QLEN)
            continue; /* overflow: dropped */
        queued *e = &dst->q[(dst->qhead + dst->qn++) % QLEN];
        e->due = due;
        memcpy(e->pkt, pkt, pkt[1]);
    }
    for (int i = 0; i < MAX_CONNS; i++)
        if (conns[i].used && !conns[i].time && &ports[conns[i].src] == src)
            deliver(&ports[conns[i].dst], ports[conns[i].dst].offset);
}

/* What the host's sources played, a packet a message, now. */
static void from_host(const uint8_t *msg, int n, int cont)
{
    if (host_out < 0 || n > PKT_MAX - PKT_HDR)
        return;
    uint8_t pkt[PKT_MAX + 1] = { (uint8_t)(midiTimeStampCurrent | cont), (uint8_t)(n + PKT_HDR) };
    memcpy(pkt + PKT_HDR, msg, (size_t)n);
    send_packet(&ports[host_out], pkt);
}

/* ---- Clients ---- */

int32_t tb_DirectorUnknown3(void) { return 0x02008000; } /* MIDIVersion: MIDI Manager 2.0 */

static int sign_in(uint32_t id, uint32_t refcon, gaddr icon)
{
    int ci = find_client(id);
    if (ci < 0) {
        for (int i = 0; i < MAX_CLIENTS && ci < 0; i++)
            if (!clients[i].used)
                ci = i;
        if (ci < 0)
            return -1;
    }
    client *c = &clients[ci];
    c->used = true;
    c->id = id;
    c->refcon = refcon;
    c->icon = icon;
    return ci;
}

/* MIDISignIn(clientID, refCon, icon, name): OSErr */
int16_t tb_FinaleUnknown2(int32_t id, int32_t refcon, gaddr icon, gaddr name)
{
    int ci = sign_in((uint32_t)id, (uint32_t)refcon, icon);
    if (ci < 0)
        return midiTooManyPortsErr;
    copy_name(clients[ci].name, name);
    return noErr;
}

static void remove_port(port *p)
{
    int pi = (int)(p - ports);
    for (int i = 0; i < MAX_CONNS; i++)
        if (conns[i].used && (conns[i].src == pi || conns[i].dst == pi))
            conns[i].used = false;
    for (int i = 0; i < MAX_PORTS; i++)
        if (ports[i].used && ports[i].timebase == refnum_of(p))
            ports[i].timebase = 0;
    p->used = false;
    p->qn = 0;
}

void tb_MIDISignOut(int32_t id)
{
    int ci = find_client((uint32_t)id);
    if (ci < 0 || (uint32_t)id == HOST_CLIENT)
        return;
    for (int i = 0; i < MAX_PORTS; i++)
        if (ports[i].used && ports[i].owner == (unsigned)ci)
            remove_port(&ports[i]);
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

static int connect_ports(int src, int dst, bool time)
{
    for (int i = 0; i < MAX_CONNS; i++)
        if (conns[i].used && conns[i].time == time && conns[i].src == src && conns[i].dst == dst)
            return noErr;
    for (int i = 0; i < MAX_CONNS; i++)
        if (!conns[i].used) {
            conns[i] = (conn){ true, time, (uint8_t)src, (uint8_t)dst };
            return noErr;
        }
    return midiTooManyConsErr;
}

static int add_port(int ci, uint32_t id, int16_t type)
{
    int pi = -1;
    for (int i = 0; i < MAX_PORTS && pi < 0; i++)
        if (!ports[i].used)
            pi = i;
    if (pi < 0)
        return -1;
    port *p = &ports[pi];
    memset(p, 0, sizeof *p);
    p->used = true;
    p->owner = (unsigned)ci;
    p->id = id;
    p->type = type;
    return pi;
}

/* MIDIAddPort(clientID, bufSize, VAR refnum, init): OSErr.  MIDIPortParams:
 * portID, portType, timeBase, offsetTime, readHook, refCon, initClock
 * (sync, curTime, format), name. */
int16_t tb_DirectorUnknown4(int32_t id, int16_t bufsize, gaddr refnum, gaddr init)
{
    int ci = find_client((uint32_t)id);
    if (ci < 0)
        return midiNoClientErr;
    if (init && find_port((uint32_t)id, rd32(init)))
        return midiDupIDErr;
    int pi = add_port(ci, init ? rd32(init) : 0, init ? (int16_t)rd16(init + 4) : midiPortTypeTime);
    if (pi < 0)
        return midiTooManyPortsErr;
    port *p = &ports[pi];
    if (init) {
        p->timebase = (int16_t)rd16(init + 6);
        p->offset = (int32_t)rd32(init + 8);
        p->read_hook = rd32(init + 12);
        p->refcon = rd32(init + 16);
        p->sync = (int16_t)rd16(init + 20);
        p->base_ms = (int32_t)rd32(init + 22);
        p->format = (int16_t)rd16(init + 26);
        copy_name(p->name, init + 28);
    }
    if (p->type == midiPortTypeInput)
        p->packet = tb_NewPtr(PKT_MAX + 1, true, true);
    /* As PatchBay would: the host's MIDI in and out. */
    if (p->type == midiPortTypeOutput && host_in >= 0)
        connect_ports(pi, host_in, false);
    else if (p->type == midiPortTypeInput && host_out >= 0)
        connect_ports(host_out, pi, false);
    if (refnum)
        wr16(refnum, (uint16_t)(pi + FIRST_REFNUM));
    return noErr;
}

void tb_MIDIRemovePort(int16_t refnum)
{
    port *p = port_of(refnum);
    if (p && (p - ports) != host_in && (p - ports) != host_out)
        remove_port(p);
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

/* MIDIPortInfo: portType, timeBase (a MIDIIDRec: client, port),
 * numConnects, then the MIDIIDRecs of the ports it is connected to (data
 * ones, a time port's time ones). */
gaddr tb_MIDIGetPortInfo(int32_t id, int32_t port_id)
{
    port *p = find_port((uint32_t)id, (uint32_t)port_id);
    if (!p)
        return 0;
    int pi = (int)(p - ports);
    bool time = is_time_port(p);
    int peers[MAX_CONNS], n = 0;
    for (int i = 0; i < MAX_CONNS; i++) {
        if (!conns[i].used || conns[i].time != time)
            continue;
        if (conns[i].src == pi)
            peers[n++] = conns[i].dst;
        else if (conns[i].dst == pi)
            peers[n++] = conns[i].src;
    }
    gaddr h = tb_NewHandle(12 + 8 * n, false, true);
    if (!h || !rd32(h))
        return h;
    gaddr r = rd32(h);
    wr16(r, (uint16_t)p->type);
    port *tb = p->timebase ? port_of(p->timebase) : NULL;
    if (tb) {
        wr32(r + 2, clients[tb->owner].id);
        wr32(r + 6, tb->id);
    }
    wr16(r + 10, (uint16_t)n);
    for (int i = 0; i < n; i++) {
        wr32(r + 12 + 8 * i, clients[ports[peers[i]].owner].id);
        wr32(r + 16 + 8 * i, ports[peers[i]].id);
    }
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

/* Data goes from an output port to an input port; time from a time port to
 * any port, which keeps time by it after. */
static int16_t connect(int32_t src_client, int32_t src_port, int32_t dst_client, int32_t dst_port, bool time, bool make)
{
    port *s = find_port((uint32_t)src_client, (uint32_t)src_port);
    port *d = find_port((uint32_t)dst_client, (uint32_t)dst_port);
    if (!s || !d)
        return find_client((uint32_t)src_client) < 0 || find_client((uint32_t)dst_client) < 0 ? midiNoClientErr
                                                                                              : midiNoPortErr;
    if (time ? !is_time_port(s) || s == d : s->type != midiPortTypeOutput || d->type != midiPortTypeInput)
        return midiInvalidCmdErr;
    int si = (int)(s - ports), di = (int)(d - ports);
    if (make) {
        int16_t err = (int16_t)connect_ports(si, di, time);
        if (!err && time)
            d->timebase = refnum_of(s);
        return err;
    }
    for (int i = 0; i < MAX_CONNS; i++)
        if (conns[i].used && conns[i].time == time && conns[i].src == si && conns[i].dst == di) {
            conns[i].used = false;
            if (time && d->timebase == refnum_of(s))
                d->timebase = 0;
            return noErr;
        }
    return midiNoConErr;
}

/* A port's connection procedure: kept for MIDIGetConnectionProc.  Only
 * other clients' patching (PatchBay's) calls one, and nothing here
 * patches the program's ports but the program. */
void tb_MIDISetConnectionProc(int16_t refnum, gaddr proc, int32_t refcon)
{
    port *p = port_of(refnum);
    if (p) {
        p->conn_proc = proc;
        p->conn_refcon = (uint32_t)refcon;
    }
}

void tb_MIDIGetConnectionProc(int16_t refnum, gaddr proc, gaddr refcon)
{
    port *p = port_of(refnum);
    if (proc)
        wr32(proc, p ? p->conn_proc : 0);
    if (refcon)
        wr32(refcon, p ? p->conn_refcon : 0);
}

/* MIDIDiscardPacket: the read hook's packet is done with; ours are
 * the port's own buffer, used again for the next. */
void tb_MIDIDiscardPacket(int16_t refnum, gaddr packet)
{
    (void)refnum;
    (void)packet;
}

int16_t tb_MIDIConnectData(int32_t a, int32_t b, int32_t c, int32_t d) { return connect(a, b, c, d, false, true); }
int16_t tb_MIDIUnConnectData(int32_t a, int32_t b, int32_t c, int32_t d) { return connect(a, b, c, d, false, false); }
int16_t tb_MIDIConnectTime(int32_t a, int32_t b, int32_t c, int32_t d) { return connect(a, b, c, d, true, true); }
int16_t tb_MIDIUnConnectTime(int32_t a, int32_t b, int32_t c, int32_t d) { return connect(a, b, c, d, true, false); }

/* Throw away what is waiting at an input port. */
void tb_MIDIFlush(int16_t refnum)
{
    port *p = port_of(refnum);
    if (p)
        p->qn = 0;
}

/* Hand the read hook what is due by the port's time plus offsetTime. */
void tb_MIDIPoll(int16_t refnum, int32_t offset)
{
    port *p = port_of(refnum);
    if (p && p->type == midiPortTypeInput)
        deliver(p, offset);
}

int16_t tb_MIDIWritePacket(int16_t refnum, gaddr packet)
{
    port *p = port_of(refnum);
    if (!p)
        return midiNoPortErr;
    if (p->type != midiPortTypeOutput || !packet)
        return midiInvalidCmdErr;
    uint8_t pkt[PKT_MAX + 1];
    unsigned len = rd8(packet + 1);
    if (len < PKT_HDR)
        return noErr;
    gget(pkt, packet, len);
    send_packet(p, pkt);
    return noErr;
}

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

/* From time_pump: what the host's sources played, packets come due at
 * input ports, and due time procedures, PROCEDURE (curTime, refCon). */
void midi_pump(void)
{
    if (in_pump)
        return;
    in_pump = true;
    uint8_t bytes[sizeof in_bytes];
    int n = 0;
    pthread_mutex_lock(&in_lock);
    if (in_len) {
        memcpy(bytes, in_bytes, (size_t)in_len);
        n = in_len;
        in_len = 0;
    }
    pthread_mutex_unlock(&in_lock);
    if (n)
        parse(&in_parser, bytes, n, from_host);
    for (int i = 0; i < MAX_PORTS; i++)
        if (ports[i].used && ports[i].qn)
            deliver(&ports[i], ports[i].offset);
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
        cpu68k *c = g_cpu;
        uint32_t d[8], a[8];
        uint16_t sr;
        interrupt_regs(d, a, &sr);
        cpu_push32(c, (uint32_t)now);
        cpu_push32(c, p->refcon);
        guest_call(p->time_proc);
        restore_regs(d, a, sr);
    }
    in_pump = false;
}

void midi_reset(void)
{
    memset(clients, 0, sizeof clients);
    memset(ports, 0, sizeof ports);
    memset(conns, 0, sizeof conns);
    in_pump = false;
    reset_parser(&out_parser, sizeof out_parser.buf);
    reset_parser(&in_parser, PKT_MAX - PKT_HDR);
    pthread_mutex_lock(&in_lock);
    in_len = 0;
    pthread_mutex_unlock(&in_lock);
    /* The host's client, signed in from the start as a driver would be. */
    int ci = sign_in(HOST_CLIENT, 0, 0);
    set_name(clients[ci].name, "Blue Box MIDI");
    host_in = add_port(ci, HOST_IN, midiPortTypeInput);
    set_name(ports[host_in].name, "MIDI Out");
    ports[host_in].offset = 0; /* midiGetCurrent */
    host_out = add_port(ci, HOST_OUT, midiPortTypeOutput);
    set_name(ports[host_out].name, "MIDI In");
}
