/*
 * NetSprocket, as PowerPC games call it (NetSprocketLib, Game Sprockets
 * 1.x): games hosted and joined over Open Transport, on endpoints of its
 * own (tcp for TCP/IP, adsp for AppleTalk, opened and driven through the
 * calls a program makes), so copies of Blue Box on one host, or Macs on a
 * network running Blue Box, play together.
 *
 * The topology is client-server, the only one NetSprocket 1.x had: every
 * player's connection is to the host, which approves joins, numbers the
 * players (its own player is 1) and the groups (-2 down), and passes each
 * message on to the players it is for.  Players learn of each other from
 * the system messages (kNSpPlayerJoined ...), which they apply to their own
 * lists as they read them.  Every message goes over the connection, so all
 * of them arrive and in order (Apple sent junk and normal ones as
 * datagrams, which may be lost).  On AppleTalk a host's game is registered
 * with NBP while it is advertised.
 *
 * The wire is ours (Apple's was never published): frames of a length, a
 * kind and the payload, all big-endian:
 *   JOIN      name[32] password[32] type customLen custom
 *   APPROVED  yourID timeStamp maxPlayers topology gameName[32] count
 *             (id type name[32])... count (id count members...)...
 *   DENIED    reason (Str255)
 *   MESSAGE   flags message (an NSpMessageHeader and what follows it)
 *   REQUEST   token op a b      (a player's group and player calls, for the host)
 *   RESULT    token status value
 *   PEER      id length address (where a player is: NSpPlayer_GetAddress)
 *   PING/PONG sequence          (round-trip times)
 *
 * The messages a program gets live in one buffer of guest memory, the
 * size NSpInitialize asked for, until it releases them.
 *
 * NSpMessageHeader: version 0, what 4, from 8, to 12, id 16, when 20,
 * messageLen 24; 28 bytes.  NSpPlayerInfo: id 0, type 4, name 8,
 * groupCount 40, groups 44.  NSpGroupInfo: id 0, playerCount 4, players 8.
 * NSpGameInfo: maxPlayers 0, currentPlayers 4, currentGroups 8, topology
 * 12, reserved 16, name 20, password 52; 84 bytes.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "as_internal.h"
#include "machine.h"
#include "ot_internal.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

enum {
    kNSpInitializationFailedErr = -30360, kNSpAlreadyInitializedErr = -30361, kNSpTopologyNotSupportedErr = -30362,
    kNSpPipeFullErr = -30364, kNSpHostFailedErr = -30365, kNSpProtocolNotAvailableErr = -30366,
    kNSpInvalidGameRefErr = -30367, kNSpInvalidParameterErr = -30369, kNSpMemAllocationErr = -30373,
    kNSpNotAdvertisingErr = -30376, kNSpInvalidAddressErr = -30377, kNSpFreeQExhaustedErr = -30378,
    kNSpRemovePlayerFailedErr = -30379, kNSpAddressInUseErr = -30380, kNSpInvalidPlayerIDErr = -30383,
    kNSpInvalidGroupIDErr = -30384, kNSpCreateGroupFailedErr = -30388, kNSpInvalidDefinitionErr = -30390,
    kNSpInvalidProtocolRefErr = -30391, kNSpInvalidProtocolListErr = -30392, kNSpTimeoutErr = -30393,
    kNSpGameTerminatedErr = -30394, kNSpConnectFailedErr = -30395, kNSpSendFailedErr = -30396,
    kNSpMessageTooBigErr = -30397, kNSpCantBlockErr = -30398, kNSpJoinFailedErr = -30399,
};

/* Message "what"s: the system's have the high bit set. */
enum {
    kNSpError = 0xFFFFFFFFu, kNSpJoinRequest = 0x80000001u, kNSpJoinApproved = 0x80000002u,
    kNSpJoinDenied = 0x80000003u, kNSpPlayerJoined = 0x80000004u, kNSpPlayerLeft = 0x80000005u,
    kNSpHostChanged = 0x80000006u, kNSpGameTerminated = 0x80000007u, kNSpGroupCreated = 0x80000008u,
    kNSpGroupDeleted = 0x80000009u, kNSpPlayerAddedToGroup = 0x8000000Au, kNSpPlayerRemovedFromGroup = 0x8000000Bu,
    kNSpPlayerTypeChanged = 0x8000000Cu,
};
enum { kNSpAllPlayers = 0, kNSpHostOnly = -1 };
enum { kNSpSendFlag_FailIfPipeFull = 1, kNSpSendFlag_SelfSend = 2, kNSpSendFlag_Blocking = 4 };
enum { kNSpGameFlag_DontAdvertise = 1 };
enum { kNSpClientServer = 1 };

enum { HDR = 28, INFO = 44, GAME_INFO = 84, HDR_VERSION = 1 };
enum { DEFAULT_PORT = 25710 };
enum { AF_INET_OT = 2, AF_DNS_OT = 42, AF_DDP = 0x0100, AF_DDPNBP = 0x0101, AF_NBP = 0x0102 };
enum { ADDR_MAX = 272 };

enum { MAX_PROTOCOLS = 64, MAX_LISTS = 32, MAX_ADDRS = 32, MAX_GAMES = 8, MAX_PLAYERS = 64, MAX_GROUPS = 64,
       MAX_LINKS = 80, MAX_QUEUE = 4096, MAX_FRAME = 4 << 20, MAX_HELD = 4096 };

enum { FR_JOIN = 1, FR_APPROVED, FR_DENIED, FR_MESSAGE, FR_REQUEST, FR_RESULT, FR_PEER, FR_PING, FR_PONG };
enum { OP_GROUP_NEW = 1, OP_GROUP_DISPOSE, OP_GROUP_ADD, OP_GROUP_REMOVE, OP_CHANGE_TYPE, OP_REMOVE_PLAYER };

/* ---- Host time ---- */

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* The guest's clock, for time stamps (a headless run's follows the
 * instructions run). */
static uint32_t guest_ms(void) { return (uint32_t)(machine_time_ns() / 1000000); }

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* A Str31 from guest memory (none: empty). */
static void get_str31(uint8_t out[32], gaddr p)
{
    memset(out, 0, 32);
    if (!p)
        return;
    uint8_t n = rd8(p);
    out[0] = n > 31 ? 31 : n;
    gget(out + 1, p + 1, out[0]);
}

static bool same_str(const uint8_t *a, const uint8_t *b) { return a[0] == b[0] && !memcmp(a + 1, b + 1, a[0]); }

/* ---- Open Transport, called as a program would ---- */

/* Guest memory for the calls' arguments: strings, a TCall, TBinds, two
 * addresses, flags and the data going through. */
enum { S_STR = 0, S_CALL = 64, S_BIND = 104, S_BIND2 = 120, S_ADDR = 136, S_ADDR2 = 136 + ADDR_MAX,
       S_FLAGS = 136 + 2 * ADDR_MAX, S_ERR = S_FLAGS + 4, S_DATA = S_ERR + 4, DATA_MAX = 16384,
       S_SIZE = S_DATA + DATA_MAX };
static gaddr scratch;

static bool have_scratch(void)
{
    if (!scratch)
        scratch = tb_NewPtr(S_SIZE, true, true);
    return scratch != 0;
}

static uint32_t otc(const char *name, int n, ...)
{
    void (*fn)(ppc_cpu *) = cfm_export_fn(name);
    if (!fn)
        return (uint32_t)kOTNotSupportedErr;
    ppc_cpu cpu = g_ppc;
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++)
        cpu.gpr[3 + i] = va_arg(ap, uint32_t);
    va_end(ap);
    cpu.lr = cpu.pc;
    bool was = ot_from_68k;
    ot_from_68k = false; /* our endpoints are PowerPC clients' */
    fn(&cpu);
    ot_from_68k = was;
    return cpu.gpr[3];
}

static void put_cstr(gaddr at, const char *s) { gput(at, s, (uint32_t)strlen(s) + 1); }

/* A non-blocking synchronous endpoint: tcp or adsp. */
static uint32_t ep_open(bool at)
{
    put_cstr(scratch + S_STR, at ? "adsp" : "tcp");
    uint32_t cfg = otc("OTCreateConfiguration", 1, scratch + S_STR);
    if (!cfg)
        return 0;
    wr32(scratch + S_ERR, 0);
    uint32_t ep = otc("OTOpenEndpoint", 4, cfg, 0, 0, scratch + S_ERR);
    if (!ep || rd32(scratch + S_ERR))
        return 0;
    otc("OTSetSynchronous", 1, ep);
    otc("OTSetNonBlocking", 1, ep);
    return ep;
}

static uint32_t addr_len(const uint8_t *a, uint32_t max)
{
    uint16_t t = (uint16_t)(a[0] << 8 | a[1]);
    uint32_t n;
    switch (t) {
    case AF_INET_OT:
        n = 16;
        break;
    case AF_DDP:
        n = 8;
        break;
    case AF_DNS_OT:
    case AF_NBP:
    case AF_DDPNBP:
        n = t == AF_DDPNBP ? 8 : 2;
        while (n < max && a[n])
            n++;
        break;
    default:
        n = 16;
        break;
    }
    return n < max ? n : max;
}

static bool addr_is_atalk(const uint8_t *a)
{
    uint16_t t = (uint16_t)(a[0] << 8 | a[1]);
    return t == AF_DDP || t == AF_DDPNBP || t == AF_NBP;
}

/* ---- Connections ---- */

typedef struct {
    bool used, at;
    bool connecting, dead;
    uint32_t ep;
    uint8_t *in, *out;
    uint32_t inlen, incap, outlen, outcap;
    uint8_t peer[ADDR_MAX]; /* the address at the other end */
    uint32_t peerlen;
    uint64_t opened_ms, ping_ms;
    uint32_t ping_seq, rtt;
    bool pinging;
    bool closing; /* to close once what's waiting has gone (a join denied) */
} link;

static link links[MAX_LINKS];
static int nat_links; /* AppleTalk ones, whose stack needs running */

static int link_new(uint32_t ep, bool at)
{
    for (int i = 0; i < MAX_LINKS; i++)
        if (!links[i].used) {
            memset(&links[i], 0, sizeof links[i]);
            links[i].used = true;
            links[i].at = at;
            links[i].ep = ep;
            links[i].opened_ms = mono_ms();
            if (at)
                nat_links++;
            return i;
        }
    otc("OTCloseProvider", 1, ep);
    return -1;
}

static void link_close(int l)
{
    if (l < 0 || !links[l].used)
        return;
    link *k = &links[l];
    if (k->ep)
        otc("OTCloseProvider", 1, k->ep);
    if (k->at)
        nat_links--;
    free(k->in);
    free(k->out);
    memset(k, 0, sizeof *k);
}

static bool grow(uint8_t **buf, uint32_t *cap, uint32_t need)
{
    if (need <= *cap)
        return true;
    uint32_t n = *cap ? *cap : 1024;
    while (n < need)
        n *= 2;
    uint8_t *b = realloc(*buf, n);
    if (!b)
        return false;
    *buf = b;
    *cap = n;
    return true;
}

/* What's waiting goes to Open Transport, as much as it takes. */
static void link_flush(int l)
{
    link *k = &links[l];
    while (k->used && !k->dead && !k->connecting && k->outlen) {
        uint32_t n = k->outlen < DATA_MAX ? k->outlen : DATA_MAX;
        gput(scratch + S_DATA, k->out, n);
        int32_t r = (int32_t)otc("OTSnd", 4, k->ep, scratch + S_DATA, n, 0);
        if (r > 0) {
            memmove(k->out, k->out + r, k->outlen - (uint32_t)r);
            k->outlen -= (uint32_t)r;
            continue;
        }
        if (r != kOTFlowErr && r != kOTNoDataErr)
            k->dead = true;
        break;
    }
}

static void link_send(int l, uint32_t kind, const uint8_t *a, uint32_t alen, const uint8_t *b, uint32_t blen)
{
    if (l < 0 || !links[l].used || links[l].dead)
        return;
    link *k = &links[l];
    if (!grow(&k->out, &k->outcap, k->outlen + 8 + alen + blen)) {
        k->dead = true;
        return;
    }
    uint8_t *p = k->out + k->outlen;
    put32(p, alen + blen);
    put32(p + 4, kind);
    if (alen)
        memcpy(p + 8, a, alen);
    if (blen)
        memcpy(p + 8 + alen, b, blen);
    k->outlen += 8 + alen + blen;
    link_flush(l);
}

/* Bytes in from Open Transport, or the connection's end. */
static void link_read(int l)
{
    link *k = &links[l];
    if (k->connecting) {
        int32_t r = (int32_t)otc("OTRcvConnect", 2, k->ep, 0);
        if (r == kOTNoError) {
            k->connecting = false;
            link_flush(l);
        } else if (r != kOTNoDataErr) {
            k->dead = true;
        }
        return;
    }
    while (k->used && !k->dead) {
        wr32(scratch + S_FLAGS, 0);
        int32_t r = (int32_t)otc("OTRcv", 4, k->ep, scratch + S_DATA, DATA_MAX, scratch + S_FLAGS);
        if (r > 0) {
            if (!grow(&k->in, &k->incap, k->inlen + (uint32_t)r)) {
                k->dead = true;
                break;
            }
            gget(k->in + k->inlen, scratch + S_DATA, (uint32_t)r);
            k->inlen += (uint32_t)r;
            continue;
        }
        if (r != kOTNoDataErr)
            k->dead = true; /* released, reset or refused */
        break;
    }
}

/* ---- References: blocks of guest memory the program holds ---- */

static gaddr new_ref(uint32_t magic)
{
    gaddr r = tb_NewPtr(8, true, true);
    if (r)
        wr32(r, magic);
    return r;
}

static void free_ref(gaddr *r)
{
    if (*r)
        tb_DisposePtr(*r);
    *r = 0;
}

/* Protocols */
typedef struct {
    bool used, at;
    gaddr ref;
    uint16_t port;
    uint8_t nbp_name[33], nbp_type[33]; /* Pascal strings */
    uint32_t max_rtt, min_thruput;
} protocol;
static protocol protocols[MAX_PROTOCOLS];

static protocol *protocol_of(gaddr ref)
{
    for (int i = 0; ref && i < MAX_PROTOCOLS; i++)
        if (protocols[i].used && protocols[i].ref == ref)
            return &protocols[i];
    return NULL;
}

static protocol *protocol_new(void)
{
    for (int i = 0; i < MAX_PROTOCOLS; i++)
        if (!protocols[i].used) {
            memset(&protocols[i], 0, sizeof protocols[i]);
            protocols[i].ref = new_ref(0x4E537050); /* 'NSpP' */
            if (!protocols[i].ref)
                return NULL;
            protocols[i].used = true;
            return &protocols[i];
        }
    return NULL;
}

/* Protocol lists */
typedef struct {
    bool used;
    gaddr ref;
    int n;
    gaddr items[MAX_PROTOCOLS];
} plist;
static plist lists[MAX_LISTS];

static plist *list_of(gaddr ref)
{
    for (int i = 0; ref && i < MAX_LISTS; i++)
        if (lists[i].used && lists[i].ref == ref)
            return &lists[i];
    return NULL;
}

/* Address references */
typedef struct {
    bool used;
    gaddr ref;
    uint8_t addr[ADDR_MAX];
    uint32_t len;
} address;
static address addrs[MAX_ADDRS];

static address *address_of(gaddr ref)
{
    for (int i = 0; ref && i < MAX_ADDRS; i++)
        if (addrs[i].used && addrs[i].ref == ref)
            return &addrs[i];
    return NULL;
}

static gaddr address_new(const uint8_t *a, uint32_t len)
{
    for (int i = 0; i < MAX_ADDRS; i++)
        if (!addrs[i].used) {
            addrs[i].ref = new_ref(0x4E537041); /* 'NSpA' */
            if (!addrs[i].ref)
                return 0;
            addrs[i].used = true;
            memset(addrs[i].addr, 0, sizeof addrs[i].addr);
            addrs[i].len = len > ADDR_MAX ? ADDR_MAX : len;
            memcpy(addrs[i].addr, a, addrs[i].len);
            return addrs[i].ref;
        }
    return 0;
}

/* ---- The message buffer ---- */

/* One block of guest memory; the messages handed out are ranges in it. */
static gaddr pool;
static uint32_t pool_size;
static struct { uint32_t off, len; } held[MAX_HELD];
static int nheld;
static bool initialized;
static uint32_t std_size, q_elements, game_id, connect_timeout_s = 15;

static bool pool_make(uint32_t want)
{
    if (pool)
        return true;
    for (uint32_t n = want; n >= 16384; n /= 2) {
        pool = tb_NewPtr((int32_t)n, true, false);
        if (!pool)
            pool = tb_NewPtr((int32_t)n, false, false);
        if (pool) {
            pool_size = n;
            return true;
        }
    }
    return false;
}

/* Room for n bytes (first fit, the ranges kept in order): 0 for none. */
static gaddr pool_alloc(uint32_t n)
{
    if (!pool || nheld >= MAX_HELD)
        return 0;
    n = (n + 3) & ~3u;
    uint32_t at = 0;
    int i = 0;
    for (; i < nheld; i++) {
        if (held[i].off - at >= n)
            break;
        at = held[i].off + held[i].len;
    }
    if (i == nheld && pool_size - at < n)
        return 0;
    memmove(held + i + 1, held + i, sizeof held[0] * (size_t)(nheld - i));
    held[i].off = at;
    held[i].len = n;
    nheld++;
    return pool + at;
}

static bool pool_free(gaddr p)
{
    for (int i = 0; p >= pool && i < nheld; i++)
        if (pool + held[i].off == p) {
            memmove(held + i, held + i + 1, sizeof held[0] * (size_t)(nheld - i - 1));
            nheld--;
            return true;
        }
    return false;
}

/* ---- Games ---- */

typedef struct {
    bool used;
    int32_t id;
    uint32_t type;
    uint8_t name[32];
    int link;            /* the host's connection to it; -1 for its own player */
    uint8_t addr[ADDR_MAX];
    uint32_t addrlen;
} player;

typedef struct {
    bool used;
    int32_t id;
    int n;
    int32_t members[MAX_PLAYERS];
} group;

typedef struct {
    uint8_t *bytes;
    uint32_t len;
    gaddr guest; /* a copy in the buffer, once the handler has had it */
} queued;

typedef struct {
    bool used, host, terminated, told_end;
    gaddr ref;
    int32_t my_id;
    uint32_t max_players, topology;
    uint8_t name[32], password[32];
    player players[MAX_PLAYERS];
    group groups[MAX_GROUPS];
    int32_t next_player, next_group;
    uint32_t msg_id;
    uint32_t t0; /* the guest's clock at the host's time stamp 0 */
    /* host: its listeners, one per protocol */
    struct {
        gaddr proto;
        int link;
        bool at;
        uint8_t skt;
        gaddr nte;     /* the NBP name registered for it */
        uint8_t entity[99];
        int entlen;
    } listeners[8];
    int nlisteners;
    /* a joined game: the connection to the host */
    int hostlink;
    uint8_t join_addr[ADDR_MAX];
    uint32_t join_addrlen;
    /* a join's or a request's answer */
    int join_state; /* 0 waiting, 1 approved, -1 denied */
    uint32_t want_token;
    bool have_result;
    int32_t result_status, result_value;
    queued *q;
    int nq, qcap;
} game;

static game games[MAX_GAMES];
static uint32_t next_token;

/* Handlers */
static gaddr msg_handler, msg_context, join_handler, join_context, cb_handler, cb_context;

static game *game_of(gaddr ref)
{
    for (int i = 0; ref && i < MAX_GAMES; i++)
        if (games[i].used && games[i].ref == ref)
            return &games[i];
    return NULL;
}

static uint32_t stamp(game *g) { return guest_ms() - g->t0; }

static player *find_player(game *g, int32_t id)
{
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (g->players[i].used && g->players[i].id == id)
            return &g->players[i];
    return NULL;
}

static group *find_group(game *g, int32_t id)
{
    for (int i = 0; i < MAX_GROUPS; i++)
        if (g->groups[i].used && g->groups[i].id == id)
            return &g->groups[i];
    return NULL;
}

static int count_players(game *g)
{
    int n = 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
        n += g->players[i].used;
    return n;
}

static int count_groups(game *g)
{
    int n = 0;
    for (int i = 0; i < MAX_GROUPS; i++)
        n += g->groups[i].used;
    return n;
}

static player *add_player(game *g, int32_t id, uint32_t type, const uint8_t name[32])
{
    player *p = find_player(g, id);
    for (int i = 0; !p && i < MAX_PLAYERS; i++)
        if (!g->players[i].used) {
            p = &g->players[i];
            memset(p, 0, sizeof *p);
            p->link = -1;
        }
    if (!p)
        return NULL;
    p->used = true;
    p->id = id;
    p->type = type;
    memcpy(p->name, name, 32);
    return p;
}

static bool in_group(group *gr, int32_t id)
{
    for (int i = 0; i < gr->n; i++)
        if (gr->members[i] == id)
            return true;
    return false;
}

static void group_add(group *gr, int32_t id)
{
    if (!in_group(gr, id) && gr->n < MAX_PLAYERS)
        gr->members[gr->n++] = id;
}

static void group_remove(group *gr, int32_t id)
{
    for (int i = 0; i < gr->n; i++)
        if (gr->members[i] == id) {
            memmove(gr->members + i, gr->members + i + 1, sizeof gr->members[0] * (size_t)(gr->n - i - 1));
            gr->n--;
            return;
        }
}

static group *add_group(game *g, int32_t id)
{
    group *gr = find_group(g, id);
    for (int i = 0; !gr && i < MAX_GROUPS; i++)
        if (!g->groups[i].used) {
            gr = &g->groups[i];
            memset(gr, 0, sizeof *gr);
            gr->used = true;
            gr->id = id;
        }
    return gr;
}

static void drop_player(game *g, int32_t id)
{
    player *p = find_player(g, id);
    if (p)
        p->used = false;
    for (int i = 0; i < MAX_GROUPS; i++)
        if (g->groups[i].used)
            group_remove(&g->groups[i], id);
}

static int player_groups(game *g, int32_t id, int32_t *out)
{
    int n = 0;
    for (int i = 0; i < MAX_GROUPS; i++)
        if (g->groups[i].used && in_group(&g->groups[i], id))
            out[n++] = g->groups[i].id;
    return n;
}

/* ---- Messages in ---- */

static void enqueue(game *g, const uint8_t *m, uint32_t len)
{
    if (g->nq >= MAX_QUEUE)
        return;
    if (g->nq == g->qcap) {
        int cap = g->qcap ? g->qcap * 2 : 32;
        queued *q = realloc(g->q, sizeof *q * (size_t)cap);
        if (!q)
            return;
        g->q = q;
        g->qcap = cap;
    }
    uint8_t *b = malloc(len);
    if (!b)
        return;
    memcpy(b, m, len);
    g->q[g->nq++] = (queued){ b, len, 0 };
}

/* Ahead of what came while it waited (a join's approval). */
static void enqueue_front(game *g, const uint8_t *m, uint32_t len)
{
    enqueue(g, m, len);
    if (g->nq > 1) {
        queued last = g->q[g->nq - 1];
        memmove(g->q + 1, g->q, sizeof g->q[0] * (size_t)(g->nq - 1));
        g->q[0] = last;
    }
}

static void dequeue(game *g, int i)
{
    free(g->q[i].bytes);
    memmove(g->q + i, g->q + i + 1, sizeof g->q[0] * (size_t)(g->nq - i - 1));
    g->nq--;
}

/* A message for this machine's program.  The system's update the game's
 * lists first, as the program will see them. */
static void apply_system(game *g, const uint8_t *m, uint32_t len)
{
    uint32_t what = get32(m + 4);
    const uint8_t *b = m + HDR;
    uint32_t n = len - HDR;
    switch (what) {
    case kNSpPlayerJoined:
        if (n >= 4 + 40)
            add_player(g, (int32_t)get32(b + 4), get32(b + 8), b + 12);
        break;
    case kNSpPlayerLeft:
        if (n >= 8)
            drop_player(g, (int32_t)get32(b + 4));
        break;
    case kNSpGroupCreated:
        if (n >= 4)
            add_group(g, (int32_t)get32(b));
        break;
    case kNSpGroupDeleted:
        if (n >= 4) {
            group *gr = find_group(g, (int32_t)get32(b));
            if (gr)
                gr->used = false;
        }
        break;
    case kNSpPlayerAddedToGroup:
    case kNSpPlayerRemovedFromGroup:
        if (n >= 8) {
            group *gr = find_group(g, (int32_t)get32(b));
            if (gr && what == kNSpPlayerAddedToGroup)
                group_add(gr, (int32_t)get32(b + 4));
            else if (gr)
                group_remove(gr, (int32_t)get32(b + 4));
        }
        break;
    case kNSpPlayerTypeChanged:
        if (n >= 8) {
            player *p = find_player(g, (int32_t)get32(b));
            if (p)
                p->type = get32(b + 4);
        }
        break;
    case kNSpGameTerminated:
        g->terminated = g->told_end = true;
        break;
    }
}

static void deliver_local(game *g, const uint8_t *m, uint32_t len)
{
    if (len < HDR)
        return;
    if (!g->host)
        apply_system(g, m, len);
    enqueue(g, m, len);
}

/* A message the host makes. */
static uint32_t build(game *g, uint8_t *out, uint32_t what, int32_t to, const uint8_t *body, uint32_t blen)
{
    put32(out, HDR_VERSION);
    put32(out + 4, what);
    put32(out + 8, (uint32_t)kNSpHostOnly);
    put32(out + 12, (uint32_t)to);
    put32(out + 16, ++g->msg_id);
    put32(out + 20, stamp(g));
    put32(out + 24, HDR + blen);
    if (blen)
        memcpy(out + HDR, body, blen);
    return HDR + blen;
}

static void send_message(int l, uint32_t flags, const uint8_t *m, uint32_t len)
{
    uint8_t f[4];
    put32(f, flags);
    link_send(l, FR_MESSAGE, f, 4, m, len);
}

/* A system message to every player and the host's program. */
static void broadcast(game *g, uint32_t what, const uint8_t *body, uint32_t blen)
{
    uint8_t m[HDR + 512];
    uint32_t len = build(g, m, what, kNSpAllPlayers, body, blen);
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (g->players[i].used && g->players[i].link >= 0)
            send_message(g->players[i].link, 0, m, len);
    enqueue(g, m, len);
}

/* Where the host passes a message on to: each player it is for. */
static void route(game *g, const uint8_t *m, uint32_t len, int32_t from, uint32_t flags)
{
    int32_t to = (int32_t)get32(m + 12);
    bool self = flags & kNSpSendFlag_SelfSend;
    if (to == kNSpHostOnly) {
        enqueue(g, m, len);
        return;
    }
    group *gr = to < kNSpHostOnly ? find_group(g, to) : NULL;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        player *p = &g->players[i];
        if (!p->used)
            continue;
        bool wanted = to == kNSpAllPlayers ? true : gr ? in_group(gr, p->id) : p->id == to;
        if (!wanted || (p->id == from && !self && to != p->id))
            continue;
        if (p->link < 0)
            enqueue(g, m, len);
        else
            send_message(p->link, 0, m, len);
    }
}

/* ---- The host's side ---- */

static void put_name(uint8_t *out, const uint8_t name[32]) { memcpy(out, name, 32); }

static void send_peer(game *g, int l, player *p)
{
    if (!p->addrlen)
        return;
    uint8_t b[8 + ADDR_MAX];
    put32(b, (uint32_t)p->id);
    put32(b + 4, p->addrlen);
    memcpy(b + 8, p->addr, p->addrlen);
    link_send(l, FR_PEER, b, 8 + p->addrlen, NULL, 0);
    (void)g;
}

static void deny(int l, const char *why)
{
    uint8_t r[256] = { 0 };
    size_t n = strlen(why);
    r[0] = (uint8_t)(n > 255 ? 255 : n);
    memcpy(r + 1, why, r[0]);
    link_send(l, FR_DENIED, r, 256, NULL, 0);
    links[l].closing = true;
}

/* The program's join request handler: true to let the player in. */
static bool ask_join_handler(game *g, const uint8_t *req, uint32_t len, uint8_t reason[256])
{
    if (!join_handler)
        return true;
    gaddr m = pool_alloc(len), r = pool_alloc(256);
    if (!m || !r) {
        pool_free(m);
        pool_free(r);
        return true;
    }
    gput(m, req, len);
    gmemset(r, 0, 256);
    uint32_t args[4] = { g->ref, m, join_context, r };
    bool ok = (ot_callproc_as(false, join_handler, 0x3FD1, args, 4) & 0xFF) != 0;
    gget(reason, r, 256);
    pool_free(m);
    pool_free(r);
    return ok;
}

static void host_join(game *g, int l, const uint8_t *b, uint32_t n)
{
    if (n < 72) {
        deny(l, "Bad request");
        return;
    }
    uint8_t name[32], password[32];
    memcpy(name, b, 32);
    memcpy(password, b + 32, 32);
    name[0] = name[0] > 31 ? 31 : name[0];
    password[0] = password[0] > 31 ? 31 : password[0];
    uint32_t type = get32(b + 64), clen = get32(b + 68);
    if (clen > n - 72)
        clen = n - 72;
    if (g->password[0] && !same_str(g->password, password)) {
        deny(l, "Incorrect password");
        return;
    }
    if (g->max_players && (uint32_t)count_players(g) >= g->max_players) {
        deny(l, "Maximum allowed players already in game");
        return;
    }
    /* NSpJoinRequestMessage: name 28, password 60, theType 92,
     * customDataLen 96, customData 100. */
    uint32_t mlen = HDR + 72 + (clen ? clen : 1);
    uint8_t *m = calloc(1, mlen);
    if (!m)
        return;
    int32_t id = g->next_player;
    build(g, m, kNSpJoinRequest, kNSpHostOnly, NULL, 0);
    put32(m + 8, (uint32_t)id);
    put32(m + 24, mlen);
    memcpy(m + HDR, name, 32);
    memcpy(m + HDR + 32, password, 32);
    put32(m + HDR + 64, type);
    put32(m + HDR + 68, clen);
    memcpy(m + HDR + 72, b + 72, clen);
    uint8_t reason[256] = { 0 };
    bool ok = ask_join_handler(g, m, mlen, reason);
    free(m);
    if (!g->used || !links[l].used)
        return;
    if (!ok) {
        char why[256];
        memcpy(why, reason + 1, reason[0]);
        why[reason[0]] = 0;
        deny(l, why);
        return;
    }
    g->next_player++;
    player *p = add_player(g, id, type, name);
    if (!p) {
        deny(l, "Maximum allowed players already in game");
        return;
    }
    p->link = l;
    p->addrlen = links[l].peerlen;
    memcpy(p->addr, links[l].peer, p->addrlen);

    /* what the new player needs to know: the game, its players and groups */
    uint32_t cap = 52 + 4 + (uint32_t)MAX_PLAYERS * 40 + 4 + (uint32_t)MAX_GROUPS * (8 + 4 * MAX_PLAYERS);
    uint8_t *a = malloc(cap), *q;
    if (!a)
        return;
    put32(a, (uint32_t)id);
    put32(a + 4, stamp(g));
    put32(a + 8, g->max_players);
    put32(a + 12, g->topology);
    put_name(a + 16, g->name);
    q = a + 52;
    put32(q, (uint32_t)count_players(g));
    q += 4;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (g->players[i].used) {
            put32(q, (uint32_t)g->players[i].id);
            put32(q + 4, g->players[i].type);
            put_name(q + 8, g->players[i].name);
            q += 40;
        }
    put32(q, (uint32_t)count_groups(g));
    q += 4;
    for (int i = 0; i < MAX_GROUPS; i++)
        if (g->groups[i].used) {
            put32(q, (uint32_t)g->groups[i].id);
            put32(q + 4, (uint32_t)g->groups[i].n);
            q += 8;
            for (int j = 0; j < g->groups[i].n; j++, q += 4)
                put32(q, (uint32_t)g->groups[i].members[j]);
        }
    link_send(l, FR_APPROVED, a, (uint32_t)(q - a), NULL, 0);
    free(a);
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (g->players[i].used && g->players[i].link >= 0) {
            if (g->players[i].link != l)
                send_peer(g, g->players[i].link, p);
            send_peer(g, l, &g->players[i]);
        }

    /* kNSpPlayerJoined: playerCount, then the NSpPlayerInfo */
    uint8_t body[4 + INFO + 4] = { 0 };
    put32(body, (uint32_t)count_players(g));
    put32(body + 4, (uint32_t)id);
    put32(body + 8, type);
    put_name(body + 12, name);
    put32(body + 44, 0);
    LOGF(LOG_TRAPS, "NetSprocket: player %d \"%.*s\" joined\n", id, name[0], name + 1);
    broadcast(g, kNSpPlayerJoined, body, sizeof body);
}

/* A player gone: from the lists, and every program told. */
static void host_player_left(game *g, int32_t id)
{
    player *p = find_player(g, id);
    if (!p)
        return;
    uint8_t name[32];
    memcpy(name, p->name, 32);
    int l = p->link;
    drop_player(g, id);
    if (l >= 0)
        link_close(l);
    uint8_t body[40];
    put32(body, (uint32_t)count_players(g));
    put32(body + 4, (uint32_t)id);
    put_name(body + 8, name);
    LOGF(LOG_TRAPS, "NetSprocket: player %d left\n", id);
    broadcast(g, kNSpPlayerLeft, body, sizeof body);
}

static int32_t host_new_group(game *g, int32_t by, int32_t *out)
{
    group *gr = NULL;
    int32_t id = g->next_group;
    if (count_groups(g) < MAX_GROUPS)
        gr = add_group(g, id);
    if (!gr)
        return kNSpCreateGroupFailedErr;
    g->next_group--;
    uint8_t body[8];
    put32(body, (uint32_t)id);
    put32(body + 4, (uint32_t)by);
    broadcast(g, kNSpGroupCreated, body, 8);
    *out = id;
    return noErr;
}

static int32_t host_request(game *g, int32_t by, uint32_t op, int32_t a, int32_t b, int32_t *value)
{
    *value = 0;
    uint8_t body[8];
    put32(body, (uint32_t)a);
    put32(body + 4, (uint32_t)b);
    switch (op) {
    case OP_GROUP_NEW:
        return host_new_group(g, by, value);
    case OP_GROUP_DISPOSE: {
        group *gr = find_group(g, a);
        if (!gr)
            return kNSpInvalidGroupIDErr;
        gr->used = false;
        put32(body + 4, (uint32_t)by);
        broadcast(g, kNSpGroupDeleted, body, 8);
        return noErr;
    }
    case OP_GROUP_ADD:
    case OP_GROUP_REMOVE: {
        group *gr = find_group(g, a);
        if (!gr)
            return kNSpInvalidGroupIDErr;
        if (!find_player(g, b))
            return kNSpInvalidPlayerIDErr;
        if (op == OP_GROUP_ADD)
            group_add(gr, b);
        else
            group_remove(gr, b);
        broadcast(g, op == OP_GROUP_ADD ? kNSpPlayerAddedToGroup : kNSpPlayerRemovedFromGroup, body, 8);
        return noErr;
    }
    case OP_CHANGE_TYPE: {
        player *p = find_player(g, a);
        if (!p)
            return kNSpInvalidPlayerIDErr;
        p->type = (uint32_t)b;
        broadcast(g, kNSpPlayerTypeChanged, body, 8);
        return noErr;
    }
    case OP_REMOVE_PLAYER: {
        player *p = find_player(g, a);
        if (!p)
            return kNSpInvalidPlayerIDErr;
        if (p->link < 0)
            return kNSpRemovePlayerFailedErr; /* the host's own player stays */
        uint8_t m[HDR];
        uint32_t len = build(g, m, kNSpGameTerminated, a, NULL, 0);
        send_message(p->link, 0, m, len);
        link_flush(p->link);
        host_player_left(g, a);
        return noErr;
    }
    }
    return kNSpInvalidParameterErr;
}

/* ---- Frames ---- */

static void pong(int l, const uint8_t *b, uint32_t n)
{
    if (n >= 4)
        link_send(l, FR_PONG, b, 4, NULL, 0);
}

static void got_pong(int l, const uint8_t *b, uint32_t n)
{
    link *k = &links[l];
    if (n >= 4 && k->pinging && get32(b) == k->ping_seq) {
        k->rtt = (uint32_t)(mono_ms() - k->ping_ms);
        k->pinging = false;
    }
}

static int32_t player_of_link(game *g, int l)
{
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (g->players[i].used && g->players[i].link == l)
            return g->players[i].id;
    return 0;
}

static void host_frame(game *g, int l, uint32_t kind, const uint8_t *b, uint32_t n)
{
    int32_t from = player_of_link(g, l);
    switch (kind) {
    case FR_JOIN:
        if (!from)
            host_join(g, l, b, n);
        break;
    case FR_MESSAGE:
        if (from && n >= 4 + HDR) {
            uint8_t *m = malloc(n - 4);
            if (!m)
                break;
            memcpy(m, b + 4, n - 4);
            put32(m + 8, (uint32_t)from); /* it is from whoever sent it */
            put32(m + 24, n - 4);
            route(g, m, n - 4, from, get32(b));
            free(m);
        }
        break;
    case FR_REQUEST:
        if (from && n >= 16) {
            int32_t value;
            int32_t st = host_request(g, from, get32(b + 4), (int32_t)get32(b + 8), (int32_t)get32(b + 12), &value);
            uint8_t r[12];
            put32(r, get32(b));
            put32(r + 4, (uint32_t)st);
            put32(r + 8, (uint32_t)value);
            if (links[l].used)
                link_send(l, FR_RESULT, r, 12, NULL, 0);
        }
        break;
    case FR_PING:
        pong(l, b, n);
        break;
    case FR_PONG:
        got_pong(l, b, n);
        break;
    }
}

static void client_approved(game *g, const uint8_t *b, uint32_t n)
{
    if (n < 56)
        return;
    g->my_id = (int32_t)get32(b);
    g->t0 = guest_ms() - get32(b + 4);
    g->max_players = get32(b + 8);
    g->topology = get32(b + 12);
    memcpy(g->name, b + 16, 32);
    const uint8_t *q = b + 52, *end = b + n;
    uint32_t np = get32(q);
    q += 4;
    for (uint32_t i = 0; i < np && q + 40 <= end; i++, q += 40)
        add_player(g, (int32_t)get32(q), get32(q + 4), q + 8);
    if (q + 4 > end)
        return;
    uint32_t ng = get32(q);
    q += 4;
    for (uint32_t i = 0; i < ng && q + 8 <= end; i++) {
        group *gr = add_group(g, (int32_t)get32(q));
        uint32_t m = get32(q + 4);
        q += 8;
        for (uint32_t j = 0; j < m && q + 4 <= end; j++, q += 4)
            if (gr)
                group_add(gr, (int32_t)get32(q));
    }
    g->join_state = 1;
}

static void client_frame(game *g, int l, uint32_t kind, const uint8_t *b, uint32_t n)
{
    switch (kind) {
    case FR_APPROVED:
        client_approved(g, b, n);
        break;
    case FR_DENIED:
        g->join_state = -1;
        break;
    case FR_MESSAGE:
        if (n >= 4 + HDR)
            deliver_local(g, b + 4, n - 4);
        break;
    case FR_RESULT:
        if (n >= 12 && get32(b) == g->want_token) {
            g->have_result = true;
            g->result_status = (int32_t)get32(b + 4);
            g->result_value = (int32_t)get32(b + 8);
        }
        break;
    case FR_PEER:
        if (n >= 8) {
            player *p = find_player(g, (int32_t)get32(b));
            uint32_t len = get32(b + 4);
            if (!p) { /* known before its kNSpPlayerJoined comes */
                uint8_t none[32] = { 0 };
                p = add_player(g, (int32_t)get32(b), 0, none);
            }
            if (p && len <= n - 8 && len <= ADDR_MAX) {
                memcpy(p->addr, b + 8, len);
                p->addrlen = len;
            }
        }
        break;
    case FR_PING:
        pong(l, b, n);
        break;
    case FR_PONG:
        got_pong(l, b, n);
        break;
    }
}

/* The frames read on a connection, each handled once it is all there. */
static void take_frames(game *g, int l)
{
    while (g->used && links[l].used && links[l].inlen >= 8) {
        link *k = &links[l];
        uint32_t n = get32(k->in), kind = get32(k->in + 4);
        if (n > MAX_FRAME) {
            k->dead = true;
            return;
        }
        if (k->inlen < 8 + n)
            return;
        uint8_t *b = malloc(n ? n : 1);
        if (!b)
            return;
        memcpy(b, k->in + 8, n);
        memmove(k->in, k->in + 8 + n, k->inlen - 8 - n);
        k->inlen -= 8 + n;
        if (g->host)
            host_frame(g, l, kind, b, n);
        else
            client_frame(g, l, kind, b, n);
        free(b);
    }
}

static void ping(int l)
{
    link *k = &links[l];
    uint64_t now = mono_ms();
    if (k->connecting || k->dead || (k->pinging && now - k->ping_ms < 5000) || now - k->ping_ms < 1000)
        return;
    k->ping_seq++;
    k->ping_ms = now;
    k->pinging = true;
    uint8_t b[4];
    put32(b, k->ping_seq);
    link_send(l, FR_PING, b, 4, NULL, 0);
}

/* Connections not yet a player's, waiting for their join request: the
 * link, and the game's index above it. */
static int pending[MAX_LINKS];
static int npending;

/* A listener's waiting connections, each a link of its own. */
static void accept_all(game *g, int li)
{
    int l = g->listeners[li].link;
    bool at = g->listeners[li].at;
    for (int tries = 0; tries < 4; tries++) {
        gaddr call = scratch + S_CALL;
        gmemset(call, 0, 40);
        wr32(call, ADDR_MAX);
        wr32(call + 8, scratch + S_ADDR2);
        if ((int32_t)otc("OTListen", 2, links[l].ep, call) != kOTNoError)
            return;
        uint8_t peer[ADDR_MAX];
        uint32_t plen = rd32(call + 4) < ADDR_MAX ? rd32(call + 4) : ADDR_MAX;
        gget(peer, scratch + S_ADDR2, plen);
        uint32_t w = ep_open(at);
        if (!w)
            return;
        if ((int32_t)otc("OTAccept", 3, links[l].ep, w, call) != kOTNoError) {
            otc("OTCloseProvider", 1, w);
            continue;
        }
        int k = link_new(w, at);
        if (k < 0)
            return;
        memcpy(links[k].peer, peer, plen);
        links[k].peerlen = plen;
        if (npending < MAX_LINKS)
            pending[npending++] = k | (int)(g - games) << 8;
        LOGF(LOG_TRAPS, "NetSprocket: a connection to the host (%s)\n", at ? "AppleTalk" : "TCP/IP");
    }
}

static void game_poll(game *g)
{
    if (g->host) {
        for (int i = 0; i < g->nlisteners; i++)
            if (g->listeners[i].link >= 0)
                accept_all(g, i);
        for (int i = 0; i < npending; i++) {
            if ((pending[i] >> 8) != (int)(g - games))
                continue;
            int l = pending[i] & 0xFF;
            if (links[l].used) {
                link_read(l);
                take_frames(g, l);
                if (!g->used)
                    return;
            }
            if (!links[l].used || player_of_link(g, l)) { /* gone, or a player's now */
                pending[i--] = pending[--npending];
                continue;
            }
            link_flush(l);
            /* denied (once the reason has gone), dead, or never asking */
            uint64_t age = mono_ms() - links[l].opened_ms;
            if (links[l].dead || (links[l].closing && (!links[l].outlen || age > 5000)) || age > 30000) {
                link_close(l);
                pending[i--] = pending[--npending];
            }
        }
        for (int i = 0; g->used && i < MAX_PLAYERS; i++) {
            player *p = &g->players[i];
            if (!p->used || p->link < 0)
                continue;
            int l = p->link;
            link_read(l);
            take_frames(g, l);
            if (!g->used)
                return;
            if (p->used && p->link == l && links[l].used) {
                link_flush(l);
                ping(l);
                if (links[l].dead)
                    host_player_left(g, p->id);
            }
        }
    } else if (g->hostlink >= 0) {
        int l = g->hostlink;
        link_read(l);
        take_frames(g, l);
        if (!g->used || g->hostlink != l)
            return;
        link_flush(l);
        if (g->join_state == 1)
            ping(l);
        if (links[l].dead) {
            link_close(l);
            g->hostlink = -1;
            if (!g->told_end && g->join_state == 1) {
                uint8_t m[HDR];
                uint32_t len = build(g, m, kNSpGameTerminated, g->my_id, NULL, 0);
                enqueue(g, m, len);
            }
            g->terminated = g->told_end = true;
        }
    }
}

/* The program's message handler sees each message as it comes in; false
 * from it and the message is gone. */
static void hand_to_handler(game *g)
{
    if (!msg_handler)
        return;
    for (int i = 0; g->used && i < g->nq; i++) {
        queued *q = &g->q[i];
        if (q->guest)
            continue;
        q->guest = pool_alloc(q->len);
        if (!q->guest)
            return; /* the buffer is full: later */
        gput(q->guest, q->bytes, q->len);
        gaddr m = q->guest;
        uint32_t args[3] = { g->ref, m, msg_context };
        bool keep = (ot_callproc_as(false, msg_handler, 0xFD1, args, 3) & 0xFF) != 0;
        if (!g->used)
            return;
        /* the queue may have moved under the handler's calls */
        for (int j = 0; j < g->nq; j++)
            if (g->q[j].guest == m) {
                if (!keep) {
                    pool_free(m);
                    dequeue(g, j);
                    i = j - 1;
                }
                break;
            }
    }
}

static int polling;

/* Every game's connections, read and written; called by NetSprocket's
 * calls and with Open Transport's (as the program waits for events and at
 * deferred task time). */
void netsprocket_poll(void)
{
    if (polling || g_cpu->halted)
        return;
    bool any = false;
    for (int i = 0; i < MAX_GAMES; i++)
        any |= games[i].used;
    if (!any || !have_scratch())
        return;
    polling++;
    if (nat_links > 0)
        appletalk_poll();
    for (int i = 0; i < MAX_GAMES; i++)
        if (games[i].used)
            game_poll(&games[i]);
    for (int i = 0; i < MAX_GAMES; i++)
        if (games[i].used)
            hand_to_handler(&games[i]);
    polling--;
}

/* A slice of waiting in a call that waits: false once the time is up.
 * Calls that wait can't be made from the message handler (interrupt time,
 * as NetSprocket said: kNSpCantBlockErr). */
static bool wait_slice(uint64_t until)
{
    struct timespec ts = { 0, 1000000 };
    nanosleep(&ts, NULL);
    if (nat_links > 0)
        appletalk_poll();
    netsprocket_poll(); /* a host's side answers, in this run too */
    return mono_ms() < until && !g_cpu->halted;
}

/* ---- Initialization ---- */

/* NSpInitialize(standardMessageSize, bufferSize, qElements, gameID, timeout) */
static void nsp_Initialize(ppc_cpu *c)
{
    if (initialized) {
        RETURN(kNSpAlreadyInitializedErr);
        return;
    }
    std_size = R(0);
    q_elements = R(2);
    game_id = R(3);
    uint32_t want = R(1) > 65536 ? R(1) : 65536;
    if (want > (4u << 20))
        want = 4u << 20;
    if (!pool_make(want) || !have_scratch()) {
        RETURN(kNSpMemAllocationErr);
        return;
    }
    initialized = true;
    LOGF(LOG_TRAPS, "NetSprocket: initialized (messages %u, buffer %u, queue %u, game '%c%c%c%c')\n", std_size, R(1),
         q_elements, (char)(game_id >> 24), (char)(game_id >> 16), (char)(game_id >> 8), (char)game_id);
    RETURN(noErr);
}

/* Without NSpInitialize, as a program may: the defaults. */
static bool ready(void)
{
    if (!pool && !pool_make(65536))
        return false;
    return have_scratch();
}

/* ---- Protocols ---- */

static void definition(protocol *p, char *out, size_t size)
{
    if (p->at)
        snprintf(out, size, "ot:atlk$\r%u\r%u\r%.*s\r%.*s", p->max_rtt, p->min_thruput, p->nbp_name[0],
                 (const char *)p->nbp_name + 1, p->nbp_type[0], (const char *)p->nbp_type + 1);
    else
        snprintf(out, size, "ot:inet$\r%u\r%u\r%u", p->max_rtt, p->min_thruput, p->port);
}

/* Apple's form, "ot:atlk$" or "ot:inet$" then the fields after returns:
 * the round trip time and throughput wanted, then the NBP name and type,
 * or the port. */
static bool parse_definition(const char *s, protocol *p)
{
    char buf[256], *fields[8];
    snprintf(buf, sizeof buf, "%s", s);
    int n = 0;
    for (char *f = buf; n < 8;) {
        fields[n++] = f;
        f = strpbrk(f, "\r\t\n");
        if (!f)
            break;
        *f++ = 0;
    }
    if (!strncasecmp(fields[0], "ot:atlk", 7) || !strncasecmp(fields[0], "atlk", 4))
        p->at = true;
    else if (strncasecmp(fields[0], "ot:inet", 7) && strncasecmp(fields[0], "inet", 4))
        return false;
    p->max_rtt = n > 1 ? (uint32_t)strtoul(fields[1], NULL, 10) : 0;
    p->min_thruput = n > 2 ? (uint32_t)strtoul(fields[2], NULL, 10) : 0;
    if (p->at) {
        const char *nm = n > 3 ? fields[3] : "", *ty = n > 4 ? fields[4] : "";
        p->nbp_name[0] = (uint8_t)snprintf((char *)p->nbp_name + 1, 32, "%.31s", nm);
        p->nbp_type[0] = (uint8_t)snprintf((char *)p->nbp_type + 1, 32, "%.31s", ty);
    } else {
        p->port = n > 3 ? (uint16_t)strtoul(fields[3], NULL, 10) : DEFAULT_PORT;
        if (!p->port)
            p->port = DEFAULT_PORT;
    }
    return true;
}

/* NSpProtocol_New(definition, &ref) */
static void nsp_ProtocolNew(ppc_cpu *c)
{
    char def[256];
    if (!R(0) || !R(1)) {
        RETURN(kNSpInvalidParameterErr);
        return;
    }
    uint32_t i = 0;
    for (; i < sizeof def - 1 && rd8(R(0) + i); i++)
        def[i] = (char)rd8(R(0) + i);
    def[i] = 0;
    protocol tmp;
    memset(&tmp, 0, sizeof tmp);
    if (!parse_definition(def, &tmp)) {
        wr32(R(1), 0);
        RETURN(kNSpInvalidDefinitionErr);
        return;
    }
    protocol *p = protocol_new();
    if (!p) {
        RETURN(kNSpMemAllocationErr);
        return;
    }
    gaddr ref = p->ref;
    *p = tmp;
    p->used = true;
    p->ref = ref;
    wr32(R(1), ref);
    RETURN(noErr);
}

static void nsp_ProtocolDispose(ppc_cpu *c)
{
    protocol *p = protocol_of(R(0));
    if (p) {
        for (int i = 0; i < MAX_LISTS; i++)
            for (int j = 0; lists[i].used && j < lists[i].n; j++)
                if (lists[i].items[j] == p->ref) {
                    memmove(lists[i].items + j, lists[i].items + j + 1, sizeof(gaddr) * (size_t)(lists[i].n - j - 1));
                    lists[i].n--;
                    j--;
                }
        free_ref(&p->ref);
        p->used = false;
    }
    c->pc = c->lr;
}

/* NSpProtocol_ExtractDefinitionString(ref, out char[256]) */
static void nsp_ProtocolExtractDefinitionString(ppc_cpu *c)
{
    protocol *p = protocol_of(R(0));
    if (!p) {
        RETURN(kNSpInvalidProtocolRefErr);
        return;
    }
    if (!R(1)) {
        RETURN(kNSpInvalidParameterErr);
        return;
    }
    char def[256];
    definition(p, def, sizeof def);
    put_cstr(R(1), def);
    RETURN(noErr);
}

/* NSpProtocol_CreateAppleTalk(nbpName, nbpType, maxRTT, minThruput) */
static void nsp_ProtocolCreateAppleTalk(ppc_cpu *c)
{
    protocol *p = protocol_new();
    if (!p) {
        RETURN(0);
        return;
    }
    p->at = true;
    get_str31(p->nbp_name, R(0));
    get_str31(p->nbp_type, R(1));
    p->max_rtt = R(2);
    p->min_thruput = R(3);
    RETURN(p->ref);
}

/* NSpProtocol_CreateIP(port, maxRTT, minThruput) */
static void nsp_ProtocolCreateIP(ppc_cpu *c)
{
    protocol *p = protocol_new();
    if (!p) {
        RETURN(0);
        return;
    }
    p->port = (uint16_t)R(0) ? (uint16_t)R(0) : DEFAULT_PORT;
    p->max_rtt = R(1);
    p->min_thruput = R(2);
    RETURN(p->ref);
}

/* NSpProtocolList_New(protocol or nil, &list) */
static void nsp_ProtocolListNew(ppc_cpu *c)
{
    if (!R(1)) {
        RETURN(kNSpInvalidParameterErr);
        return;
    }
    if (R(0) && !protocol_of(R(0))) {
        RETURN(kNSpInvalidProtocolRefErr);
        return;
    }
    for (int i = 0; i < MAX_LISTS; i++)
        if (!lists[i].used) {
            memset(&lists[i], 0, sizeof lists[i]);
            lists[i].ref = new_ref(0x4E53704C); /* 'NSpL' */
            if (!lists[i].ref)
                break;
            lists[i].used = true;
            if (R(0))
                lists[i].items[lists[i].n++] = R(0);
            wr32(R(1), lists[i].ref);
            RETURN(noErr);
            return;
        }
    wr32(R(1), 0);
    RETURN(kNSpMemAllocationErr);
}

/* Disposing of a list disposes of the protocols in it. */
static void nsp_ProtocolListDispose(ppc_cpu *c)
{
    plist *l = list_of(R(0));
    if (l) {
        for (int i = 0; i < l->n; i++) {
            protocol *p = protocol_of(l->items[i]);
            if (p) {
                free_ref(&p->ref);
                p->used = false;
            }
        }
        free_ref(&l->ref);
        l->used = false;
    }
    c->pc = c->lr;
}

static void nsp_ProtocolListAppend(ppc_cpu *c)
{
    plist *l = list_of(R(0));
    if (!l) {
        RETURN(kNSpInvalidProtocolListErr);
        return;
    }
    if (!protocol_of(R(1))) {
        RETURN(kNSpInvalidProtocolRefErr);
        return;
    }
    if (l->n >= MAX_PROTOCOLS) {
        RETURN(kNSpMemAllocationErr);
        return;
    }
    l->items[l->n++] = R(1);
    RETURN(noErr);
}

static void remove_at(plist *l, int i)
{
    memmove(l->items + i, l->items + i + 1, sizeof(gaddr) * (size_t)(l->n - i - 1));
    l->n--;
}

static void nsp_ProtocolListRemove(ppc_cpu *c)
{
    plist *l = list_of(R(0));
    if (!l) {
        RETURN(kNSpInvalidProtocolListErr);
        return;
    }
    for (int i = 0; i < l->n; i++)
        if (l->items[i] == R(1)) {
            remove_at(l, i);
            RETURN(noErr);
            return;
        }
    RETURN(kNSpInvalidProtocolRefErr);
}

/* Indices count from 0. */
static void nsp_ProtocolListRemoveIndexed(ppc_cpu *c)
{
    plist *l = list_of(R(0));
    if (!l) {
        RETURN(kNSpInvalidProtocolListErr);
        return;
    }
    if (R(1) >= (uint32_t)l->n) {
        RETURN(kNSpInvalidParameterErr);
        return;
    }
    remove_at(l, (int)R(1));
    RETURN(noErr);
}

static void nsp_ProtocolListGetCount(ppc_cpu *c)
{
    plist *l = list_of(R(0));
    RETURN(l ? (uint32_t)l->n : 0);
}

static void nsp_ProtocolListGetIndexedRef(ppc_cpu *c)
{
    plist *l = list_of(R(0));
    RETURN(l && R(1) < (uint32_t)l->n ? l->items[R(1)] : 0);
}

/* ---- Hosting and joining ---- */

static game *game_new(bool host)
{
    for (int i = 0; i < MAX_GAMES; i++)
        if (!games[i].used) {
            game *g = &games[i];
            memset(g, 0, sizeof *g);
            g->ref = new_ref(0x4E537047); /* 'NSpG' */
            if (!g->ref)
                return NULL;
            g->used = true;
            g->host = host;
            g->hostlink = -1;
            g->next_player = 1;
            g->next_group = -2;
            g->t0 = guest_ms();
            for (int k = 0; k < 8; k++)
                g->listeners[k].link = -1;
            return g;
        }
    return NULL;
}

/* NBP's packed entity: object, type, zone ("*"). */
static int pack_entity(uint8_t *out, const uint8_t *obj, const uint8_t *type)
{
    int n = 0;
    out[n++] = obj[0];
    memcpy(out + n, obj + 1, obj[0]);
    n += obj[0];
    out[n++] = type[0];
    memcpy(out + n, type + 1, type[0]);
    n += type[0];
    out[n++] = 1;
    out[n++] = '*';
    return n;
}

static void advertise(game *g, int i, bool on)
{
    if (!g->listeners[i].at)
        return;
    if (on && !g->listeners[i].nte && !at_name_taken(g->listeners[i].entity, g->listeners[i].entlen))
        g->listeners[i].nte = at_register_name(g->listeners[i].entity, g->listeners[i].entlen, g->listeners[i].skt);
    else if (!on && g->listeners[i].nte) {
        at_remove_name(g->listeners[i].nte);
        g->listeners[i].nte = 0;
    }
}

/* A listener for a protocol: 0, or why not. */
static int32_t listen_on(game *g, protocol *p)
{
    if (g->nlisteners >= 8)
        return kNSpMemAllocationErr;
    if (p->at && (!appletalk_enabled() || at_open() != noErr))
        return kNSpProtocolNotAvailableErr;
    uint32_t ep = ep_open(p->at);
    if (!ep)
        return kNSpProtocolNotAvailableErr;
    gaddr req = scratch + S_BIND, ret = scratch + S_BIND2, a = scratch + S_ADDR;
    gmemset(req, 0, 16);
    gmemset(ret, 0, 16);
    gmemset(a, 0, ADDR_MAX);
    if (!p->at) {
        wr16(a, AF_INET_OT);
        wr16(a + 2, p->port);
        wr32(req, 16);
        wr32(req + 4, 16);
    }
    wr32(req + 8, a);
    wr32(req + 12, p->at ? 1 : 8);
    wr32(ret, ADDR_MAX);
    wr32(ret + 8, scratch + S_ADDR2);
    int32_t r = (int32_t)otc("OTBind", 3, ep, req, ret);
    if (r != kOTNoError) {
        otc("OTCloseProvider", 1, ep);
        return r == kOTAddressBusyErr || r == -3247 /* kEADDRINUSEErr */ ? kNSpAddressInUseErr : kNSpHostFailedErr;
    }
    int l = link_new(ep, p->at);
    if (l < 0)
        return kNSpMemAllocationErr;
    int i = g->nlisteners++;
    g->listeners[i].proto = p->ref;
    g->listeners[i].link = l;
    g->listeners[i].at = p->at;
    if (p->at) {
        g->listeners[i].skt = rd8(scratch + S_ADDR2 + 5);
        uint8_t type[33];
        if (p->nbp_type[0])
            memcpy(type, p->nbp_type, 33);
        else { /* the game's ID, as its type */
            type[0] = 4;
            put32(type + 1, game_id);
        }
        g->listeners[i].entlen = pack_entity(g->listeners[i].entity, p->nbp_name[0] ? p->nbp_name : g->name, type);
    }
    LOGF(LOG_TRAPS, "NetSprocket: hosting on %s %u\n", p->at ? "AppleTalk socket" : "TCP port",
         p->at ? g->listeners[i].skt : p->port);
    return noErr;
}

static void game_free(game *g)
{
    for (int i = 0; i < g->nlisteners; i++) {
        advertise(g, i, false);
        link_close(g->listeners[i].link);
    }
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (g->players[i].used && g->players[i].link >= 0)
            link_close(g->players[i].link);
    for (int i = 0; i < npending; i++)
        if ((pending[i] >> 8) == (int)(g - games)) {
            link_close(pending[i] & 0xFF);
            pending[i--] = pending[--npending];
        }
    if (g->hostlink >= 0)
        link_close(g->hostlink);
    for (int i = 0; i < g->nq; i++) {
        if (g->q[i].guest)
            pool_free(g->q[i].guest);
        free(g->q[i].bytes);
    }
    free(g->q);
    free_ref(&g->ref);
    memset(g, 0, sizeof *g);
}

/* NSpGame_Host(&game, protocolList, maxPlayers, gameName, password,
 * playerName, playerType, topology, flags) */
static void nsp_GameHost(ppc_cpu *c)
{
    gaddr out = R(0);
    plist *l = list_of(R(1));
    if (!out) {
        RETURN(kNSpInvalidParameterErr);
        return;
    }
    wr32(out, 0);
    if (!l) {
        RETURN(kNSpInvalidProtocolListErr);
        return;
    }
    uint32_t topology = R(7), flags = rd32(c->gpr[1] + 56);
    if (topology != kNSpClientServer) {
        RETURN(kNSpTopologyNotSupportedErr);
        return;
    }
    if (!ready()) {
        RETURN(kNSpMemAllocationErr);
        return;
    }
    game *g = game_new(true);
    if (!g) {
        RETURN(kNSpMemAllocationErr);
        return;
    }
    g->max_players = R(2);
    g->topology = topology;
    get_str31(g->name, R(3));
    get_str31(g->password, R(4));
    int32_t err = kNSpProtocolNotAvailableErr;
    int ok = 0;
    for (int i = 0; i < l->n; i++) {
        protocol *p = protocol_of(l->items[i]);
        if (!p)
            continue;
        int32_t e = listen_on(g, p);
        if (e == noErr)
            ok++;
        else
            err = e;
    }
    if (!ok) {
        game_free(g);
        RETURN(l->n ? err : kNSpInvalidProtocolListErr);
        return;
    }
    if (!(flags & kNSpGameFlag_DontAdvertise))
        for (int i = 0; i < g->nlisteners; i++)
            advertise(g, i, true);
    if (R(5)) { /* the host's own player; none for a server */
        uint8_t name[32];
        get_str31(name, R(5));
        add_player(g, g->next_player, R(6), name);
        g->my_id = g->next_player++;
    } else {
        g->my_id = kNSpHostOnly;
    }
    LOGF(LOG_TRAPS, "NetSprocket: hosting \"%.*s\" for %u players\n", g->name[0], g->name + 1, g->max_players);
    wr32(out, g->ref);
    RETURN(noErr);
}

/* NSpGame_Join(&game, address, name, password, type, customData,
 * customDataLen, flags): connected, and the host's answer. */
static void nsp_GameJoin(ppc_cpu *c)
{
    gaddr out = R(0);
    address *a = address_of(R(1));
    if (!out) {
        RETURN(kNSpInvalidParameterErr);
        return;
    }
    wr32(out, 0);
    if (!a) {
        RETURN(kNSpInvalidAddressErr);
        return;
    }
    if (!ready()) {
        RETURN(kNSpMemAllocationErr);
        return;
    }
    if (polling) {
        RETURN(kNSpCantBlockErr);
        return;
    }
    bool at = addr_is_atalk(a->addr);
    if (at && (!appletalk_enabled() || at_open() != noErr)) {
        RETURN(kNSpProtocolNotAvailableErr);
        return;
    }
    uint32_t ep = ep_open(at);
    if (!ep) {
        RETURN(kNSpProtocolNotAvailableErr);
        return;
    }
    otc("OTBind", 3, ep, 0, 0);
    int l = link_new(ep, at);
    game *g = l >= 0 ? game_new(false) : NULL;
    if (!g) {
        link_close(l);
        RETURN(kNSpMemAllocationErr);
        return;
    }
    g->hostlink = l;
    memcpy(g->join_addr, a->addr, a->len);
    g->join_addrlen = a->len;
    gaddr call = scratch + S_CALL;
    gmemset(call, 0, 40);
    gput(scratch + S_ADDR, a->addr, a->len);
    wr32(call, a->len);
    wr32(call + 4, a->len);
    wr32(call + 8, scratch + S_ADDR);
    int32_t r = (int32_t)otc("OTConnect", 3, ep, call, 0);
    if (r != kOTNoError && r != kOTNoDataErr) {
        game_free(g);
        RETURN(kNSpConnectFailedErr);
        return;
    }
    links[l].connecting = r == kOTNoDataErr;
    links[l].peerlen = a->len;
    memcpy(links[l].peer, a->addr, a->len);

    /* the request goes as soon as the connection is made */
    uint8_t req[72 + 256];
    uint32_t clen = R(6) > 256 ? 256 : R(6);
    memset(req, 0, sizeof req);
    get_str31(req, R(2));
    get_str31(req + 32, R(3));
    put32(req + 64, R(4));
    put32(req + 68, R(5) ? clen : 0);
    if (R(5) && clen)
        gget(req + 72, R(5), clen);
    link_send(l, FR_JOIN, req, 72 + (R(5) ? clen : 0), NULL, 0);
    uint64_t until = mono_ms() + (uint64_t)connect_timeout_s * 1000;
    while (g->used && g->join_state == 0 && g->hostlink >= 0 && wait_slice(until))
        ;
    if (!g->used || g->join_state != 1) {
        int32_t e = g->join_state == -1 ? kNSpJoinFailedErr
                    : g->hostlink < 0 || links[l].dead ? kNSpConnectFailedErr : kNSpTimeoutErr;
        LOGF(LOG_TRAPS, "NetSprocket: join failed (%d)\n", e);
        if (g->used)
            game_free(g);
        RETURN(e);
        return;
    }
    /* the program hears it was let in */
    uint8_t m[HDR];
    uint32_t len = build(g, m, kNSpJoinApproved, g->my_id, NULL, 0);
    enqueue_front(g, m, len);
    LOGF(LOG_TRAPS, "NetSprocket: joined \"%.*s\" as player %d\n", g->name[0], g->name + 1, g->my_id);
    wr32(out, g->ref);
    RETURN(noErr);
}

/* NSpGame_EnableAdvertising(game, protocol or nil for all, enable) */
static void nsp_GameEnableAdvertising(ppc_cpu *c)
{
    game *g = game_of(R(0));
    if (!g) {
        RETURN(kNSpInvalidGameRefErr);
        return;
    }
    if (!g->host) {
        RETURN(kNSpNotAdvertisingErr);
        return;
    }
    bool found = false;
    for (int i = 0; i < g->nlisteners; i++)
        if (!R(1) || g->listeners[i].proto == R(1)) {
            advertise(g, i, (R(2) & 0xFF) != 0);
            found = true;
        }
    RETURN(found ? noErr : kNSpInvalidProtocolRefErr);
}

/* NSpGame_Dispose(game, flags): the host ends the game for everyone. */
static void nsp_GameDispose(ppc_cpu *c)
{
    game *g = game_of(R(0));
    if (!g) {
        RETURN(kNSpInvalidGameRefErr);
        return;
    }
    if (g->host) {
        uint8_t m[HDR];
        uint32_t len = build(g, m, kNSpGameTerminated, kNSpAllPlayers, NULL, 0);
        bool waiting = false;
        for (int i = 0; i < MAX_PLAYERS; i++)
            if (g->players[i].used && g->players[i].link >= 0) {
                send_message(g->players[i].link, 0, m, len);
                waiting = true;
            }
        /* a moment for the news to leave */
        uint64_t until = mono_ms() + 500;
        while (waiting) {
            waiting = false;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                int l = g->players[i].used ? g->players[i].link : -1;
                if (l >= 0 && links[l].used && !links[l].dead) {
                    link_flush(l);
                    waiting |= links[l].outlen > 0;
                }
            }
            if (waiting && !wait_slice(until))
                break;
        }
        if (nat_links > 0) { /* ADSP's queues go before its ends close */
            uint64_t t = mono_ms() + 100;
            while (mono_ms() < t) {
                struct timespec ts = { 0, 1000000 };
                nanosleep(&ts, NULL);
                appletalk_poll();
            }
        }
    } else if (g->hostlink >= 0) {
        link_flush(g->hostlink);
    }
    LOGF(LOG_TRAPS, "NetSprocket: game \"%.*s\" disposed\n", g->name[0], g->name + 1);
    game_free(g);
    RETURN(noErr);
}

/* NSpGame_GetInfo(game, NSpGameInfo *) */
static void nsp_GameGetInfo(ppc_cpu *c)
{
    game *g = game_of(R(0));
    gaddr info = R(1);
    if (!g) {
        RETURN(kNSpInvalidGameRefErr);
        return;
    }
    if (!info) {
        RETURN(kNSpInvalidParameterErr);
        return;
    }
    netsprocket_poll();
    gmemset(info, 0, GAME_INFO);
    wr32(info, g->max_players);
    wr32(info + 4, (uint32_t)count_players(g));
    wr32(info + 8, (uint32_t)count_groups(g));
    wr32(info + 12, g->topology);
    gput(info + 20, g->name, 32);
    if (g->host)
        gput(info + 52, g->password, 32);
    RETURN(noErr);
}

/* ---- Messages ---- */

/* Whether a message to `to` is for this machine's player. */
static bool for_me(game *g, int32_t to, uint32_t flags)
{
    bool self = flags & kNSpSendFlag_SelfSend;
    if (to == g->my_id)
        return true;
    if (!self)
        return false;
    if (to == kNSpAllPlayers)
        return true;
    group *gr = to < kNSpHostOnly ? find_group(g, to) : NULL;
    return gr && in_group(gr, g->my_id);
}

static int32_t send_bytes(game *g, uint8_t *m, uint32_t len, uint32_t flags)
{
    if (g->terminated)
        return kNSpGameTerminatedErr;
    int32_t to = (int32_t)get32(m + 12);
    if (to != kNSpAllPlayers && to != kNSpHostOnly && !find_player(g, to) && !find_group(g, to))
        return to < kNSpHostOnly ? kNSpInvalidGroupIDErr : kNSpInvalidPlayerIDErr;
    put32(m, HDR_VERSION);
    put32(m + 8, (uint32_t)g->my_id);
    put32(m + 16, ++g->msg_id);
    put32(m + 20, stamp(g));
    if (g->host) {
        route(g, m, len, g->my_id, flags);
        return noErr;
    }
    int l = g->hostlink;
    if (l < 0 || !links[l].used || links[l].dead)
        return kNSpSendFailedErr;
    if ((flags & kNSpSendFlag_FailIfPipeFull) && links[l].outlen > 65536)
        return kNSpPipeFullErr;
    if (for_me(g, to, flags))
        deliver_local(g, m, len);
    if (to != g->my_id)
        send_message(l, flags & ~(uint32_t)kNSpSendFlag_SelfSend, m, len);
    if (flags & kNSpSendFlag_Blocking) { /* until it has gone */
        uint64_t until = mono_ms() + 5000;
        while (links[l].used && links[l].outlen && !links[l].dead && mono_ms() < until && !g_cpu->halted) {
            struct timespec ts = { 0, 1000000 };
            nanosleep(&ts, NULL);
            if (links[l].at)
                appletalk_poll();
            link_flush(l);
        }
    }
    return links[l].used && links[l].dead ? kNSpSendFailedErr : noErr;
}

static uint32_t max_message(void) { return pool_size ? pool_size : 65536; }

/* NSpMessage_Send(game, NSpMessageHeader *, flags) */
static void nsp_MessageSend(ppc_cpu *c)
{
    game *g = game_of(R(0));
    gaddr h = R(1);
    if (!g) {
        RETURN(kNSpInvalidGameRefErr);
        return;
    }
    if (!h) {
        RETURN(kNSpInvalidParameterErr);
        return;
    }
    uint32_t len = rd32(h + 24);
    if (len < HDR) {
        RETURN(kNSpInvalidParameterErr);
        return;
    }
    if (len > max_message()) {
        RETURN(kNSpMessageTooBigErr);
        return;
    }
    uint8_t *m = malloc(len);
    if (!m) {
        RETURN(kNSpMemAllocationErr);
        return;
    }
    gget(m, h, len);
    int32_t e = send_bytes(g, m, len, R(2));
    if (e == noErr) { /* what it was sent as */
        wr32(h, HDR_VERSION);
        wr32(h + 8, get32(m + 8));
        wr32(h + 16, get32(m + 16));
        wr32(h + 20, get32(m + 20));
    }
    free(m);
    netsprocket_poll();
    RETURN((uint32_t)e);
}

/* NSpMessage_SendTo(game, to, what, data, dataLen, flags) */
static void nsp_MessageSendTo(ppc_cpu *c)
{
    game *g = game_of(R(0));
    if (!g) {
        RETURN(kNSpInvalidGameRefErr);
        return;
    }
    uint32_t dlen = R(3) ? R(4) : 0;
    if (HDR + (uint64_t)dlen > max_message()) {
        RETURN(kNSpMessageTooBigErr);
        return;
    }
    uint8_t *m = calloc(1, HDR + dlen);
    if (!m) {
        RETURN(kNSpMemAllocationErr);
        return;
    }
    put32(m + 4, R(2));
    put32(m + 12, R(1));
    put32(m + 24, HDR + dlen);
    if (dlen)
        gget(m + HDR, R(3), dlen);
    int32_t e = send_bytes(g, m, HDR + dlen, R(5));
    free(m);
    netsprocket_poll();
    RETURN((uint32_t)e);
}

/* NSpMessage_Get(game): the next message, or nil. */
static void nsp_MessageGet(ppc_cpu *c)
{
    game *g = game_of(R(0));
    if (!g) {
        RETURN(0);
        return;
    }
    netsprocket_poll();
    if (!g->used || !g->nq) {
        RETURN(0);
        return;
    }
    queued *q = &g->q[0];
    gaddr m = q->guest ? q->guest : pool_alloc(q->len);
    if (!m) {
        RETURN(0); /* the buffer is full until messages are released */
        return;
    }
    if (!q->guest)
        gput(m, q->bytes, q->len);
    dequeue(g, 0);
    RETURN(m);
}

static void nsp_MessageRelease(ppc_cpu *c)
{
    pool_free(R(1));
    c->pc = c->lr;
}

/* ---- Players ---- */

static void nsp_PlayerGetMyID(ppc_cpu *c)
{
    game *g = game_of(R(0));
    RETURN(g ? (uint32_t)g->my_id : 0);
}

/* A player's NSpPlayerInfo in guest memory at `at` (room for its groups). */
static uint32_t info_size(game *g, int32_t id)
{
    int32_t gs[MAX_GROUPS];
    int n = player_groups(g, id, gs);
    return INFO + 4 * (uint32_t)(n ? n : 1);
}

static void put_info(game *g, player *p, gaddr at)
{
    int32_t gs[MAX_GROUPS];
    int n = player_groups(g, p->id, gs);
    gmemset(at, 0, info_size(g, p->id));
    wr32(at, (uint32_t)p->id);
    wr32(at + 4, p->type);
    gput(at + 8, p->name, 32);
    wr32(at + 40, (uint32_t)n);
    for (int i = 0; i < n; i++)
        wr32(at + 44 + 4 * (uint32_t)i, (uint32_t)gs[i]);
}

/* NSpPlayer_GetInfo(game, id, &info): released with NSpPlayer_ReleaseInfo. */
static void nsp_PlayerGetInfo(ppc_cpu *c)
{
    game *g = game_of(R(0));
    if (!g) {
        RETURN(kNSpInvalidGameRefErr);
        return;
    }
    if (!R(2)) {
        RETURN(kNSpInvalidParameterErr);
        return;
    }
    netsprocket_poll();
    player *p = find_player(g, (int32_t)R(1));
    if (!p) {
        wr32(R(2), 0);
        RETURN(kNSpInvalidPlayerIDErr);
        return;
    }
    gaddr info = tb_NewPtr((int32_t)info_size(g, p->id), false, true);
    if (!info) {
        RETURN(kNSpMemAllocationErr);
        return;
    }
    put_info(g, p, info);
    wr32(R(2), info);
    RETURN(noErr);
}

static void dispose_arg(ppc_cpu *c, int n)
{
    if (R(n))
        tb_DisposePtr(R(n));
    c->pc = c->lr;
}

static void nsp_ReleaseInfo(ppc_cpu *c) { dispose_arg(c, 1); }

/* NSpPlayer_GetEnumeration(game, &enumeration): count, then a pointer to
 * each player's info (all in one block). */
static void nsp_PlayerGetEnumeration(ppc_cpu *c)
{
    game *g = game_of(R(0));
    if (!g) {
        RETURN(kNSpInvalidGameRefErr);
        return;
    }
    if (!R(1)) {
        RETURN(kNSpInvalidParameterErr);
        return;
    }
    netsprocket_poll();
    int n = count_players(g);
    uint32_t size = 4 + 4 * (uint32_t)(n ? n : 1);
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (g->players[i].used)
            size += info_size(g, g->players[i].id);
    gaddr e = tb_NewPtr((int32_t)size, false, true);
    if (!e) {
        RETURN(kNSpMemAllocationErr);
        return;
    }
    wr32(e, (uint32_t)n);
    gaddr at = e + 4 + 4 * (uint32_t)(n ? n : 1);
    int k = 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (g->players[i].used) {
            put_info(g, &g->players[i], at);
            wr32(e + 4 + 4 * (uint32_t)k++, at);
            at += info_size(g, g->players[i].id);
        }
    wr32(R(1), e);
    RETURN(noErr);
}

static void nsp_ReleaseEnumeration(ppc_cpu *c) { dispose_arg(c, 1); }

/* Round-trip time in milliseconds: to the host, or the host's to the
 * player. */
static void nsp_PlayerGetRoundTripTime(ppc_cpu *c)
{
    game *g = game_of(R(0));
    if (!g) {
        RETURN(0);
        return;
    }
    netsprocket_poll();
    player *p = find_player(g, (int32_t)R(1));
    if (!p || p->id == g->my_id) {
        RETURN(0);
        return;
    }
    int l = g->host ? p->link : g->hostlink;
    RETURN(l >= 0 && links[l].used ? (links[l].rtt ? links[l].rtt : 1) : 0);
}

/* Bytes a second the connection takes: a 10 Mb Ethernet's for TCP/IP, a
 * LocalTalk's for AppleTalk. */
static void nsp_PlayerGetThruput(ppc_cpu *c)
{
    game *g = game_of(R(0));
    player *p = g ? find_player(g, (int32_t)R(1)) : NULL;
    if (!p) {
        RETURN(0);
        return;
    }
    int l = g->host ? p->link : g->hostlink;
    bool at = l >= 0 && links[l].used ? links[l].at : g->host && g->nlisteners && g->listeners[0].at;
    RETURN(at ? 28750u : 1250000u);
}

/* NSpPlayer_GetAddress(game, id, &OTAddress *): a copy, in a pointer of
 * its own. */
static void nsp_PlayerGetAddress(ppc_cpu *c)
{
    game *g = game_of(R(0));
    if (!g) {
        RETURN(kNSpInvalidGameRefErr);
        return;
    }
    if (!R(2)) {
        RETURN(kNSpInvalidParameterErr);
        return;
    }
    netsprocket_poll();
    player *p = find_player(g, (int32_t)R(1));
    if (!p) {
        RETURN(kNSpInvalidPlayerIDErr);
        return;
    }
    const uint8_t *a = p->addr;
    uint32_t n = p->addrlen;
    if (!n && !g->host && p->id == 1 && p->id != g->my_id) { /* the host's own player */
        a = g->join_addr;
        n = g->join_addrlen;
    }
    if (!n) {
        RETURN(kNSpInvalidPlayerIDErr);
        return;
    }
    gaddr out = tb_NewPtr((int32_t)n, false, true);
    if (!out) {
        RETURN(kNSpMemAllocationErr);
        return;
    }
    gput(out, a, n);
    wr32(R(2), out);
    RETURN(noErr);
}

/* A change the host makes: done here, or asked of it. */
static int32_t request(game *g, uint32_t op, int32_t a, int32_t b, int32_t *value)
{
    *value = 0;
    if (g->host)
        return host_request(g, g->my_id, op, a, b, value);
    if (g->terminated || g->hostlink < 0)
        return kNSpGameTerminatedErr;
    if (polling)
        return kNSpCantBlockErr;
    uint8_t r[16];
    g->want_token = ++next_token;
    g->have_result = false;
    put32(r, g->want_token);
    put32(r + 4, op);
    put32(r + 8, (uint32_t)a);
    put32(r + 12, (uint32_t)b);
    link_send(g->hostlink, FR_REQUEST, r, 16, NULL, 0);
    uint64_t until = mono_ms() + (uint64_t)connect_timeout_s * 1000;
    while (g->used && !g->have_result && g->hostlink >= 0 && wait_slice(until))
        ;
    if (!g->used || !g->have_result)
        return g->used && g->hostlink >= 0 ? kNSpTimeoutErr : kNSpGameTerminatedErr;
    *value = g->result_value;
    return g->result_status;
}

static void nsp_PlayerChangeType(ppc_cpu *c)
{
    game *g = game_of(R(0));
    int32_t v;
    RETURN(g ? (uint32_t)request(g, OP_CHANGE_TYPE, (int32_t)R(1), (int32_t)R(2), &v) : (uint32_t)kNSpInvalidGameRefErr);
}

static void nsp_PlayerRemove(ppc_cpu *c)
{
    game *g = game_of(R(0));
    int32_t v;
    RETURN(g ? (uint32_t)request(g, OP_REMOVE_PLAYER, (int32_t)R(1), 0, &v) : (uint32_t)kNSpInvalidGameRefErr);
}

/* ---- Groups ---- */

static void nsp_GroupNew(ppc_cpu *c)
{
    game *g = game_of(R(0));
    if (!g) {
        RETURN(kNSpInvalidGameRefErr);
        return;
    }
    if (!R(1)) {
        RETURN(kNSpInvalidParameterErr);
        return;
    }
    int32_t id = 0;
    int32_t e = request(g, OP_GROUP_NEW, 0, 0, &id);
    wr32(R(1), e == noErr ? (uint32_t)id : 0);
    RETURN((uint32_t)e);
}

static void nsp_GroupDispose(ppc_cpu *c)
{
    game *g = game_of(R(0));
    int32_t v;
    RETURN(g ? (uint32_t)request(g, OP_GROUP_DISPOSE, (int32_t)R(1), 0, &v) : (uint32_t)kNSpInvalidGameRefErr);
}

static void nsp_GroupAddPlayer(ppc_cpu *c)
{
    game *g = game_of(R(0));
    int32_t v;
    RETURN(g ? (uint32_t)request(g, OP_GROUP_ADD, (int32_t)R(1), (int32_t)R(2), &v) : (uint32_t)kNSpInvalidGameRefErr);
}

static void nsp_GroupRemovePlayer(ppc_cpu *c)
{
    game *g = game_of(R(0));
    int32_t v;
    RETURN(g ? (uint32_t)request(g, OP_GROUP_REMOVE, (int32_t)R(1), (int32_t)R(2), &v)
             : (uint32_t)kNSpInvalidGameRefErr);
}

static uint32_t group_size(group *gr) { return 8 + 4 * (uint32_t)(gr->n ? gr->n : 1); }

static void put_group(group *gr, gaddr at)
{
    gmemset(at, 0, group_size(gr));
    wr32(at, (uint32_t)gr->id);
    wr32(at + 4, (uint32_t)gr->n);
    for (int i = 0; i < gr->n; i++)
        wr32(at + 8 + 4 * (uint32_t)i, (uint32_t)gr->members[i]);
}

/* NSpGroup_GetInfo(game, id, &info) */
static void nsp_GroupGetInfo(ppc_cpu *c)
{
    game *g = game_of(R(0));
    if (!g) {
        RETURN(kNSpInvalidGameRefErr);
        return;
    }
    if (!R(2)) {
        RETURN(kNSpInvalidParameterErr);
        return;
    }
    netsprocket_poll();
    group *gr = find_group(g, (int32_t)R(1));
    if (!gr) {
        wr32(R(2), 0);
        RETURN(kNSpInvalidGroupIDErr);
        return;
    }
    gaddr info = tb_NewPtr((int32_t)group_size(gr), false, true);
    if (!info) {
        RETURN(kNSpMemAllocationErr);
        return;
    }
    put_group(gr, info);
    wr32(R(2), info);
    RETURN(noErr);
}

/* NSpGroup_GetEnumeration(game, &enumeration) */
static void nsp_GroupGetEnumeration(ppc_cpu *c)
{
    game *g = game_of(R(0));
    if (!g) {
        RETURN(kNSpInvalidGameRefErr);
        return;
    }
    if (!R(1)) {
        RETURN(kNSpInvalidParameterErr);
        return;
    }
    netsprocket_poll();
    int n = count_groups(g);
    uint32_t size = 4 + 4 * (uint32_t)(n ? n : 1);
    for (int i = 0; i < MAX_GROUPS; i++)
        if (g->groups[i].used)
            size += group_size(&g->groups[i]);
    gaddr e = tb_NewPtr((int32_t)size, false, true);
    if (!e) {
        RETURN(kNSpMemAllocationErr);
        return;
    }
    wr32(e, (uint32_t)n);
    gaddr at = e + 4 + 4 * (uint32_t)(n ? n : 1);
    int k = 0;
    for (int i = 0; i < MAX_GROUPS; i++)
        if (g->groups[i].used) {
            put_group(&g->groups[i], at);
            wr32(e + 4 + 4 * (uint32_t)k++, at);
            at += group_size(&g->groups[i]);
        }
    wr32(R(1), e);
    RETURN(noErr);
}

/* ---- Utilities ---- */

/* 1.7, a NumVersion */
static void nsp_GetVersion(ppc_cpu *c)
{
    ppc_struct_result(c, R(0), 0x01708000, 4);
    c->pc = c->lr;
}

static void nsp_SetConnectTimeout(ppc_cpu *c)
{
    connect_timeout_s = R(0) ? R(0) : 15;
    c->pc = c->lr;
}

static void nsp_ClearMessageHeader(ppc_cpu *c)
{
    if (R(0)) {
        gmemset(R(0), 0, HDR);
        wr32(R(0) + 24, HDR);
    }
    c->pc = c->lr;
}

/* Milliseconds since the host started the game, on every player's machine. */
static void nsp_GetCurrentTimeStamp(ppc_cpu *c)
{
    game *g = game_of(R(0));
    RETURN(g ? stamp(g) : guest_ms());
}

static void nsp_ConvertOTAddrToAddressReference(ppc_cpu *c)
{
    if (!R(0)) {
        RETURN(0);
        return;
    }
    uint8_t a[ADDR_MAX];
    gget(a, R(0), ADDR_MAX);
    RETURN(address_new(a, addr_len(a, ADDR_MAX)));
}

/* The OTAddress, a copy in a pointer of its own. */
static void nsp_ConvertAddressReferenceToOTAddr(ppc_cpu *c)
{
    address *a = address_of(R(0));
    gaddr out = a ? tb_NewPtr((int32_t)(a->len < 16 ? 16 : a->len), false, true) : 0;
    if (out)
        gput(out, a->addr, a->len);
    RETURN(out);
}

static void nsp_ReleaseAddressReference(ppc_cpu *c)
{
    address *a = address_of(R(0));
    if (a) {
        free_ref(&a->ref);
        a->used = false;
    }
    c->pc = c->lr;
}

/* Kept, never called: NetSprocket 1.x defined no events for it. */
static void nsp_InstallCallbackHandler(ppc_cpu *c)
{
    cb_handler = R(0);
    cb_context = R(1);
    RETURN(noErr);
}

static void nsp_InstallJoinRequestHandler(ppc_cpu *c)
{
    join_handler = R(0);
    join_context = R(1);
    RETURN(noErr);
}

static void nsp_InstallAsyncMessageHandler(ppc_cpu *c)
{
    msg_handler = R(0);
    msg_context = R(1);
    RETURN(noErr);
}

/* GetQState: an early NetSprocketLib's debugging export (no interface). */
static void nsp_GetQState(ppc_cpu *c) { RETURN(0); }

/* ---- The human interface: the host's dialogs ---- */

static void pas_to_c(const uint8_t *p, char *out, size_t size)
{
    size_t n = p[0] < size - 1 ? p[0] : size - 1;
    memcpy(out, p + 1, n);
    out[n] = 0;
}

static void c_to_str31(gaddr at, const char *s)
{
    size_t n = strlen(s);
    if (n > 31)
        n = 31;
    wr8(at, (uint8_t)n);
    gput(at + 1, s, (uint32_t)n);
}

/* A question with a text answer: false if cancelled.  Headless, the
 * answers come from BLUEBOX_SCRIPT_ANSWERS, and without them the dialogs
 * are cancelled (or for a text field with no default, left empty). */
static bool ask(const char *message, const char *ok, const char *def, char *answer, size_t size, bool cancel_default)
{
    machine_script_dialog d;
    memset(&d, 0, sizeof d);
    d.kind = MACHINE_SCRIPT_DIALOG;
    snprintf(d.message, sizeof d.message, "%s", message);
    d.icon = MACHINE_ALERT_NOTE;
    d.nbuttons = 2;
    snprintf(d.buttons[0], sizeof d.buttons[0], "Cancel");
    snprintf(d.buttons[1], sizeof d.buttons[1], "%s", ok);
    d.cancel_button = 1;
    d.default_button = cancel_default && !as_dialogs_native() && !getenv("BLUEBOX_SCRIPT_ANSWERS") ? 1 : 2;
    d.has_answer = true;
    snprintf(d.answer, sizeof d.answer, "%s", def);
    int b = as_ask(&d);
    snprintf(answer, size, "%s", d.answer);
    return b == 2;
}

typedef struct {
    int n;
    char names[16][33];
    uint8_t ddp[16][8];
} found_games;

static void nbp_found(void *ctx, const uint8_t *tuple, int len)
{
    found_games *f = ctx;
    if (f->n >= 16 || len < 6)
        return;
    const uint8_t *e = tuple + 5;
    int n = e[0] < 32 ? e[0] : 32;
    for (int i = 0; i < f->n; i++)
        if ((int)strlen(f->names[i]) == n && !memcmp(f->names[i], e + 1, (size_t)n))
            return;
    memcpy(f->names[f->n], e + 1, (size_t)n);
    f->names[f->n][n] = 0;
    uint8_t *a = f->ddp[f->n];
    a[0] = AF_DDP >> 8;
    a[1] = AF_DDP & 0xFF;
    a[2] = tuple[0];
    a[3] = tuple[1];
    a[4] = tuple[2];
    a[5] = tuple[3];
    a[6] = a[7] = 0;
    f->n++;
}

/* NSpDoModalJoinDialog(gameType, entityListLabel, ioName, ioPassword,
 * eventProc): the address of the game chosen, or nil. */
static void nsp_DoModalJoinDialog(ppc_cpu *c)
{
    found_games f;
    memset(&f, 0, sizeof f);
    uint8_t type[32];
    get_str31(type, R(0));
    if (type[0] && appletalk_enabled() && at_open() == noErr) { /* the games on AppleTalk */
        uint8_t packed[40];
        uint8_t any[2] = { 1, '=' };
        int n = pack_entity(packed, any, type);
        int h = at_nbp_start(packed, n, 3, 250, nbp_found, &f);
        uint64_t until = mono_ms() + 1000;
        while (h && at_nbp_busy(h) && mono_ms() < until) {
            struct timespec ts = { 0, 2000000 };
            nanosleep(&ts, NULL);
            appletalk_poll();
        }
        at_nbp_stop(h);
    }
    char msg[1024], where[256], label[256] = "";
    if (R(1)) {
        uint8_t l[256];
        gget(l, R(1), 1);
        gget(l + 1, R(1) + 1, l[0]);
        pas_to_c(l, label, sizeof label);
    }
    int at = snprintf(msg, sizeof msg, "Join a network game.\rThe host's TCP/IP address (with :port if not %d)",
                      DEFAULT_PORT);
    if (f.n) {
        at += snprintf(msg + at, sizeof msg - (size_t)at, ", or one of the %s on AppleTalk:", label[0] ? label : "games");
        for (int i = 0; i < f.n && at < (int)sizeof msg - 40; i++)
            at += snprintf(msg + at, sizeof msg - (size_t)at, "%s %s", i ? "," : "", f.names[i]);
    } else {
        snprintf(msg + at, sizeof msg - (size_t)at, ":");
    }
    if (!ask(msg, "Join", "", where, sizeof where, false) || !where[0]) {
        RETURN(0);
        return;
    }
    char name[64] = "", password[64] = "";
    uint8_t s[32];
    if (R(2)) {
        get_str31(s, R(2));
        pas_to_c(s, name, sizeof name);
        if (!ask("Your name in the game:", "Join", name, name, sizeof name, false)) {
            RETURN(0);
            return;
        }
        c_to_str31(R(2), name);
    }
    if (R(3)) {
        get_str31(s, R(3));
        pas_to_c(s, password, sizeof password);
        if (!ask("The game's password (if it has one):", "Join", password, password, sizeof password, false)) {
            RETURN(0);
            return;
        }
        c_to_str31(R(3), password);
    }
    for (int i = 0; i < f.n; i++)
        if (!strcasecmp(f.names[i], where)) {
            RETURN(address_new(f.ddp[i], 8));
            return;
        }
    /* host[:port] as a DNS address, which Open Transport looks up */
    uint8_t a[ADDR_MAX];
    memset(a, 0, sizeof a);
    a[1] = AF_DNS_OT;
    if (strchr(where, ':'))
        snprintf((char *)a + 2, sizeof a - 2, "%s", where);
    else
        snprintf((char *)a + 2, sizeof a - 2, "%s:%d", where, DEFAULT_PORT);
    RETURN(address_new(a, 2 + (uint32_t)strlen((char *)a + 2)));
}

/* NSpDoModalHostDialog(protocolList, ioGameName, ioPlayerName,
 * ioPassword, eventProc): true to host.  An empty list gets TCP/IP on the
 * usual port. */
static void nsp_DoModalHostDialog(ppc_cpu *c)
{
    char game_name[64] = "", name[64] = "", password[64] = "";
    uint8_t s[32];
    if (R(1)) {
        get_str31(s, R(1));
        pas_to_c(s, game_name, sizeof game_name);
    }
    if (!ask("Host a network game.\rThe game's name:", "Host", game_name, game_name, sizeof game_name, true)) {
        RETURN(0);
        return;
    }
    if (R(2)) {
        get_str31(s, R(2));
        pas_to_c(s, name, sizeof name);
        if (!ask("Your name in the game:", "Host", name, name, sizeof name, false)) {
            RETURN(0);
            return;
        }
        c_to_str31(R(2), name);
    }
    if (R(3)) {
        get_str31(s, R(3));
        pas_to_c(s, password, sizeof password);
        if (!ask("A password for the game (none: anyone may join):", "Host", password, password, sizeof password,
                 false)) {
            RETURN(0);
            return;
        }
        c_to_str31(R(3), password);
    }
    if (R(1))
        c_to_str31(R(1), game_name);
    plist *l = list_of(R(0));
    if (l && !l->n) {
        protocol *p = protocol_new();
        if (p) {
            p->port = DEFAULT_PORT;
            l->items[l->n++] = p->ref;
        }
    }
    RETURN(1);
}

/* ---- The run ---- */

/* A new program, or the machine starting again: the games end, their
 * endpoints closed. */
void netsprocket_reset(void)
{
    for (int i = 0; i < MAX_LINKS; i++)
        if (links[i].used) {
            provider *p = ot_provider_of(links[i].ep);
            if (p)
                ot_close_provider(p);
            free(links[i].in);
            free(links[i].out);
        }
    for (int i = 0; i < MAX_GAMES; i++)
        if (games[i].used) {
            for (int k = 0; k < games[i].nq; k++)
                free(games[i].q[k].bytes);
            free(games[i].q);
        }
    memset(links, 0, sizeof links);
    memset(games, 0, sizeof games);
    memset(protocols, 0, sizeof protocols);
    memset(lists, 0, sizeof lists);
    memset(addrs, 0, sizeof addrs);
    nat_links = npending = nheld = polling = 0;
    scratch = pool = 0;
    pool_size = 0;
    initialized = false;
    std_size = q_elements = game_id = 0;
    connect_timeout_s = 15;
    next_token = 0;
    msg_handler = msg_context = join_handler = join_context = cb_handler = cb_context = 0;
}

const ppc_export nsp_exports[] = {
    { "GetQState", nsp_GetQState },
    { "NSpClearMessageHeader", nsp_ClearMessageHeader },
    { "NSpConvertAddressReferenceToOTAddr", nsp_ConvertAddressReferenceToOTAddr },
    { "NSpConvertOTAddrToAddressReference", nsp_ConvertOTAddrToAddressReference },
    { "NSpDoModalHostDialog", nsp_DoModalHostDialog },
    { "NSpDoModalJoinDialog", nsp_DoModalJoinDialog },
    { "NSpGame_Dispose", nsp_GameDispose },
    { "NSpGame_EnableAdvertising", nsp_GameEnableAdvertising },
    { "NSpGame_GetInfo", nsp_GameGetInfo },
    { "NSpGame_Host", nsp_GameHost },
    { "NSpGame_Join", nsp_GameJoin },
    { "NSpGetCurrentTimeStamp", nsp_GetCurrentTimeStamp },
    { "NSpGetVersion", nsp_GetVersion },
    { "NSpGroup_AddPlayer", nsp_GroupAddPlayer },
    { "NSpGroup_Dispose", nsp_GroupDispose },
    { "NSpGroup_GetEnumeration", nsp_GroupGetEnumeration },
    { "NSpGroup_GetInfo", nsp_GroupGetInfo },
    { "NSpGroup_New", nsp_GroupNew },
    { "NSpGroup_ReleaseEnumeration", nsp_ReleaseEnumeration },
    { "NSpGroup_ReleaseInfo", nsp_ReleaseInfo },
    { "NSpGroup_RemovePlayer", nsp_GroupRemovePlayer },
    { "NSpInitialize", nsp_Initialize },
    { "NSpInstallAsyncMessageHandler", nsp_InstallAsyncMessageHandler },
    { "NSpInstallCallbackHandler", nsp_InstallCallbackHandler },
    { "NSpInstallJoinRequestHandler", nsp_InstallJoinRequestHandler },
    { "NSpMessage_Get", nsp_MessageGet },
    { "NSpMessage_Release", nsp_MessageRelease },
    { "NSpMessage_Send", nsp_MessageSend },
    { "NSpMessage_SendTo", nsp_MessageSendTo },
    { "NSpPlayer_ChangeType", nsp_PlayerChangeType },
    { "NSpPlayer_GetAddress", nsp_PlayerGetAddress },
    { "NSpPlayer_GetEnumeration", nsp_PlayerGetEnumeration },
    { "NSpPlayer_GetInfo", nsp_PlayerGetInfo },
    { "NSpPlayer_GetMyID", nsp_PlayerGetMyID },
    { "NSpPlayer_GetRoundTripTime", nsp_PlayerGetRoundTripTime },
    { "NSpPlayer_GetThruput", nsp_PlayerGetThruput },
    { "NSpPlayer_ReleaseEnumeration", nsp_ReleaseEnumeration },
    { "NSpPlayer_ReleaseInfo", nsp_ReleaseInfo },
    { "NSpPlayer_Remove", nsp_PlayerRemove },
    { "NSpProtocolList_Append", nsp_ProtocolListAppend },
    { "NSpProtocolList_Dispose", nsp_ProtocolListDispose },
    { "NSpProtocolList_GetCount", nsp_ProtocolListGetCount },
    { "NSpProtocolList_GetIndexedRef", nsp_ProtocolListGetIndexedRef },
    { "NSpProtocolList_New", nsp_ProtocolListNew },
    { "NSpProtocolList_Remove", nsp_ProtocolListRemove },
    { "NSpProtocolList_RemoveIndexed", nsp_ProtocolListRemoveIndexed },
    { "NSpProtocol_CreateAppleTalk", nsp_ProtocolCreateAppleTalk },
    { "NSpProtocol_CreateIP", nsp_ProtocolCreateIP },
    { "NSpProtocol_Dispose", nsp_ProtocolDispose },
    { "NSpProtocol_ExtractDefinitionString", nsp_ProtocolExtractDefinitionString },
    { "NSpProtocol_New", nsp_ProtocolNew },
    { "NSpReleaseAddressReference", nsp_ReleaseAddressReference },
    { "NSpSetConnectTimeout", nsp_SetConnectTimeout },
};
const unsigned nsp_export_count = sizeof nsp_exports / sizeof nsp_exports[0];
