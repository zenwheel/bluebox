/*
 * ATP, the AppleTalk Transaction Protocol (the .ATP driver), and ZIP's
 * calls through .XPP (its ASP and AFP calls in xpp.c), on appletalk.c's DDP.
 *
 * A transaction is a request (DDP type 3, function TReq) and up to eight
 * numbered responses (TResp), the requester asking again for those still
 * missing until its retries run out.  Exactly-once transactions (XO) keep
 * the response at the responder, for repeated requests, until the
 * requester's release (TRel) or the release timer.
 *
 * The network has no router, so ZIP's answers are those of a single
 * LocalTalk: this zone is "*", and there is no zone list.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "legacy.h"
#include "tb_internal.h"
#include "trap.h"

enum { DDP_ATP = 3, ATP_DATA = 578, MAX_REQS = 16, MAX_GETS = 32, MAX_RSPS = 16, MAX_SEEN = 64 };
enum { TREQ = 0x40, TRESP = 0x80, TREL = 0xC0, XO = 0x20, EOM = 0x10, STS = 0x08 };
/* atpFlags */
enum { atpXOvalue = 32, atpEOMvalue = 16, atpSTSvalue = 8, atpTIDValidvalue = 2 };
/* .ATP control codes */
enum {
    nSendRequest = 248, relRspCB = 249, closeATPSkt = 250, addResponse = 251, sendResponse = 252,
    getRequest = 253, openATPSkt = 254, sendRequest = 255, relTCB = 256, killGetReq = 257, killSendReq = 258,
    killAllGetReq = 259,
};
enum {
    reqFailed = -1096, tooManyReqs = -1097, tooManySkts = -1098, badATPSkt = -1099, badBuffNum = -1100,
    noRelErr = -1101, cbNotFound = -1102, noSendResp = -1103, reqAborted = -1105, noBridgeErr = -93,
};
/* ATPParamBlock, after csCode */
enum {
    P_SOCKET = 28, P_FLAGS = 29, P_ADDR = 30, P_REQLEN = 34, P_REQPTR = 36, P_BDS = 40,
    P_NUMBUFFS = 44, P_TIMEOUT = 45, P_NUMRESPS = 46, P_RETRY = 47, P_TRELTIME = 50,
    P_BITMAP = 44, P_BDSSIZE = 45, P_TRANSID = 46, P_RSPNUM = 44, P_KILLQEL = 44,
    P_USERDATA = 18, P_REQTID = 22,
};
/* BDSElement: buffSize, buffPtr, dataSize, userBytes */
enum { BDS_SIZE = 12 };

typedef struct {
    bool used;
    gaddr pb;
    uint8_t skt;    /* the socket the request goes from */
    bool own_skt;   /* opened for it (sendRequest): closed after */
    uint8_t node, dskt;
    uint16_t tid;
    uint8_t bitmap, xo;
    int nbufs, got;
    uint64_t next, timeout;
    int retries;    /* -1: for ever */
    uint8_t req[8 + ATP_DATA];
    int reqlen;
    atp_reply *host; /* host code's (atp_host_request): the responses go here, not to a BDS */
} request;

typedef struct {
    bool used;
    gaddr pb;
    uint8_t skt;
} getter;

/* An XO response kept for repeated requests. */
typedef struct {
    bool used;
    uint8_t skt, node, rskt;
    uint16_t tid;
    int n;
    uint8_t pkt[8][8 + ATP_DATA];
    int len[8];
    uint64_t until;
} response;

/* Requests delivered lately, so a repeat isn't delivered again. */
typedef struct {
    uint8_t skt, node, rskt;
    uint16_t tid;
    bool xo;
    uint64_t when;
} seen;

static request reqs[MAX_REQS];
static getter getters[MAX_GETS];
static response rsps[MAX_RSPS];
static seen seens[MAX_SEEN];
static int nseen;
static bool atp_sockets[256];
static uint32_t filters[256]; /* OpenATPSkt's address filter: net, node, socket (0 any) */
static atp_request_fn host_fns[256]; /* host code's sockets: requests to a function */
static void *host_ctx[256];
static uint16_t next_tid;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void put_header(uint8_t *p, uint8_t control, uint8_t bitmap, uint16_t tid, uint32_t user)
{
    p[0] = control;
    p[1] = bitmap;
    p[2] = (uint8_t)(tid >> 8);
    p[3] = (uint8_t)tid;
    p[4] = (uint8_t)(user >> 24);
    p[5] = (uint8_t)(user >> 16);
    p[6] = (uint8_t)(user >> 8);
    p[7] = (uint8_t)user;
}

static uint32_t user_of(const uint8_t *p) { return (uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 | (uint32_t)p[6] << 8 | p[7]; }

static void finish(gaddr pb, int16_t r)
{
    LOGF(LOG_TRAPS, "  ATP call completes -> %d\n", r);
    at_complete(pb, r);
}

static void end_request(request *q, int16_t r)
{
    q->used = false;
    if (q->own_skt) {
        atp_sockets[q->skt] = false;
        at_close_socket(q->skt);
    }
    if (q->host) {
        q->host->count = q->got;
        q->host->result = r;
        return;
    }
    wr8(q->pb + P_NUMRESPS, (uint8_t)q->got);
    finish(q->pb, r);
}

static void send_request(request *q)
{
    q->req[1] = q->bitmap;
    at_send_ddp(q->node, q->dskt, q->skt, DDP_ATP, q->req, q->reqlen);
}

/* ---------------------------------------------------------------------- */
/* Datagrams to ATP sockets */

static bool filter_ok(uint8_t skt, uint8_t node, uint8_t rskt)
{
    uint32_t f = filters[skt];
    uint8_t fnode = (uint8_t)(f >> 8), fskt = (uint8_t)f;
    return (!fnode || fnode == node) && (!fskt || fskt == rskt);
}

static seen *find_seen(uint8_t skt, uint8_t node, uint8_t rskt, uint16_t tid)
{
    for (int i = 0; i < nseen; i++)
        if (seens[i].skt == skt && seens[i].node == node && seens[i].rskt == rskt && seens[i].tid == tid)
            return &seens[i];
    return NULL;
}

static void got_request(uint8_t skt, uint8_t node, uint8_t rskt, const uint8_t *p, int len)
{
    uint16_t tid = (uint16_t)(p[2] << 8 | p[3]);
    /* A repeat of an XO request answered already: the missing responses
     * again, from the kept copy. */
    for (int i = 0; i < MAX_RSPS; i++) {
        response *r = &rsps[i];
        if (r->used && r->skt == skt && r->node == node && r->rskt == rskt && r->tid == tid) {
            for (int k = 0; k < r->n; k++)
                if (p[1] & (1 << k))
                    at_send_ddp(node, rskt, skt, DDP_ATP, r->pkt[k], r->len[k]);
            r->until = now_ms() + 30000;
            return;
        }
    }
    if (find_seen(skt, node, rskt, tid) || !filter_ok(skt, node, rskt))
        return; /* delivered, its answer to come */
    if (host_fns[skt]) {
        if (nseen == MAX_SEEN)
            memmove(seens, seens + 1, sizeof seens[0] * (size_t)--nseen);
        seens[nseen++] = (seen){ skt, node, rskt, tid, (p[0] & XO) != 0, now_ms() };
        host_fns[skt](host_ctx[skt], skt, node, rskt, tid, user_of(p), p + 8, len - 8);
        return;
    }
    for (int i = 0; i < MAX_GETS; i++) {
        getter *g = &getters[i];
        if (!g->used || g->skt != skt)
            continue;
        gaddr pb = g->pb;
        g->used = false;
        int n = len - 8, room = (int16_t)rd16(pb + P_REQLEN);
        if (n > room)
            n = room;
        if (n > 0)
            gput(rd32(pb + P_REQPTR), p + 8, (uint32_t)n);
        wr16(pb + P_REQLEN, (uint16_t)n);
        wr8(pb + P_FLAGS, (uint8_t)(p[0] & XO ? atpXOvalue : 0));
        wr16(pb + P_ADDR, 0);
        wr8(pb + P_ADDR + 2, node);
        wr8(pb + P_ADDR + 3, rskt);
        wr8(pb + P_BITMAP, p[1]);
        wr16(pb + P_TRANSID, tid);
        wr32(pb + P_USERDATA, user_of(p));
        if (nseen == MAX_SEEN)
            memmove(seens, seens + 1, sizeof seens[0] * (size_t)--nseen);
        seens[nseen++] = (seen){ skt, node, rskt, tid, (p[0] & XO) != 0, now_ms() };
        finish(pb, noErr);
        return;
    }
    /* Nobody listening yet: the requester asks again. */
}

static void got_response(uint8_t skt, uint8_t node, uint8_t rskt, const uint8_t *p, int len)
{
    uint16_t tid = (uint16_t)(p[2] << 8 | p[3]);
    for (int i = 0; i < MAX_REQS; i++) {
        request *q = &reqs[i];
        if (!q->used || q->skt != skt || q->tid != tid || q->node != node)
            continue;
        int seq = p[1];
        if (seq >= q->nbufs || !(q->bitmap & (1 << seq)))
            return;
        if (q->host) {
            memcpy(q->host->data[seq], p + 8, (size_t)(len - 8));
            q->host->len[seq] = len - 8;
            q->host->user[seq] = user_of(p);
        } else {
            gaddr e = rd32(q->pb + P_BDS) + (uint32_t)seq * BDS_SIZE;
            int n = len - 8, room = (int16_t)rd16(e);
            if (n > room)
                n = room;
            if (n > 0)
                gput(rd32(e + 2), p + 8, (uint32_t)n);
            wr16(e + 6, (uint16_t)n);
            wr32(e + 8, user_of(p));
        }
        q->bitmap &= (uint8_t)~(1 << seq);
        if (seq + 1 > q->got)
            q->got = seq + 1;
        if (p[0] & EOM)
            q->bitmap &= (uint8_t)((1 << (seq + 1)) - 1);
        if (p[0] & STS) /* the responder asks for the request again */
            q->next = 0;
        if (!q->bitmap) {
            if (q->xo) {
                uint8_t rel[8];
                put_header(rel, TREL, 0, tid, 0);
                at_send_ddp(q->node, q->dskt, q->skt, DDP_ATP, rel, 8);
            }
            end_request(q, noErr);
        }
        (void)rskt;
        return;
    }
}

static void got_release(uint8_t skt, uint8_t node, uint8_t rskt, const uint8_t *p)
{
    uint16_t tid = (uint16_t)(p[2] << 8 | p[3]);
    for (int i = 0; i < MAX_RSPS; i++)
        if (rsps[i].used && rsps[i].skt == skt && rsps[i].node == node && rsps[i].rskt == rskt && rsps[i].tid == tid)
            rsps[i].used = false;
}

static void atp_in(uint8_t skt, uint8_t snode, uint8_t sskt, uint8_t type, const uint8_t *data, int len)
{
    if (type != DDP_ATP || len < 8)
        return;
    switch (data[0] & 0xC0) {
    case TREQ: got_request(skt, snode, sskt, data, len); break;
    case TRESP: got_response(skt, snode, sskt, data, len); break;
    case TREL: got_release(skt, snode, sskt, data); break;
    }
}

/* ---------------------------------------------------------------------- */
/* Time: requests asked again, kept responses and remembered requests
 * dropped. */

void atp_poll(void)
{
    uint64_t t = now_ms();
    for (int i = 0; i < MAX_REQS; i++) {
        request *q = &reqs[i];
        if (!q->used || t < q->next)
            continue;
        if (q->next && q->retries == 0) {
            end_request(q, reqFailed);
            continue;
        }
        if (q->next && q->retries > 0)
            q->retries--;
        send_request(q);
        q->next = t + q->timeout;
    }
    for (int i = 0; i < MAX_RSPS; i++)
        if (rsps[i].used && t > rsps[i].until)
            rsps[i].used = false;
    int k = 0;
    for (int i = 0; i < nseen; i++)
        if (t - seens[i].when < 30000)
            seens[k++] = seens[i];
    nseen = k;
}

void atp_reset(void)
{
    memset(reqs, 0, sizeof reqs);
    memset(getters, 0, sizeof getters);
    memset(rsps, 0, sizeof rsps);
    memset(atp_sockets, 0, sizeof atp_sockets);
    memset(host_fns, 0, sizeof host_fns);
    nseen = 0;
}

/* ---------------------------------------------------------------------- */
/* The driver */

static int16_t wait_for(gaddr pb, bool async)
{
    wr16(pb + IOParam_ioResult_off, ioInProgress);
    if (async)
        return ioInProgress;
    /* Synchronous: the poll answers it (it writes ioResult). */
    while ((int16_t)rd16(pb + IOParam_ioResult_off) == ioInProgress && !g_cpu->halted) {
        struct timespec ts = { 0, 2000000 };
        nanosleep(&ts, NULL);
        appletalk_poll();
    }
    return (int16_t)rd16(pb + IOParam_ioResult_off);
}

static int16_t start_request(gaddr pb, bool async, bool own)
{
    request *q = NULL;
    for (int i = 0; i < MAX_REQS && !q; i++)
        if (!reqs[i].used)
            q = &reqs[i];
    if (!q)
        return tooManyReqs;
    int nbufs = rd8(pb + P_NUMBUFFS);
    if (nbufs < 1 || nbufs > 8)
        return badBuffNum;
    int skt = rd8(pb + P_SOCKET);
    if (own) {
        skt = at_open_socket(0, atp_in);
        if (!skt)
            return tooManySkts;
        atp_sockets[skt] = true;
    } else if (!atp_sockets[skt]) {
        return badATPSkt;
    }
    memset(q, 0, sizeof *q);
    q->used = true;
    q->pb = pb;
    q->skt = (uint8_t)skt;
    q->own_skt = own;
    q->node = rd8(pb + P_ADDR + 2);
    q->dskt = rd8(pb + P_ADDR + 3);
    q->tid = ++next_tid;
    q->nbufs = nbufs;
    q->bitmap = (uint8_t)((1 << nbufs) - 1);
    uint8_t flags = rd8(pb + P_FLAGS);
    q->xo = (flags & atpXOvalue) != 0;
    int t = rd8(pb + P_TIMEOUT);
    q->timeout = (uint64_t)(t ? t : 1) * 1000;
    int retries = rd8(pb + P_RETRY);
    q->retries = retries == 255 ? -1 : retries;
    int n = (int16_t)rd16(pb + P_REQLEN);
    if (n < 0 || n > ATP_DATA)
        n = n < 0 ? 0 : ATP_DATA;
    uint8_t trel = q->xo && !own ? (uint8_t)(rd8(pb + P_TRELTIME) & 7) : 0;
    put_header(q->req, (uint8_t)(TREQ | (q->xo ? XO : 0) | trel), q->bitmap, q->tid, rd32(pb + P_USERDATA));
    if (n)
        gget(q->req + 8, rd32(pb + P_REQPTR), (uint32_t)n);
    q->reqlen = 8 + n;
    wr16(pb + P_REQTID, q->tid);
    wr8(pb + P_NUMRESPS, 0);
    LOGF(LOG_TRAPS, "  ATP request to node %u socket %u, TID %u, %d responses%s\n", q->node, q->dskt, q->tid, nbufs,
         q->xo ? ", exactly once" : "");
    send_request(q);
    q->next = now_ms() + q->timeout;
    return wait_for(pb, async);
}

/* A transaction's responses (count of them, the last with EOM), kept for
 * repeats of the request when it was exactly-once. */
static void respond(uint8_t skt, uint8_t node, uint8_t rskt, uint16_t tid, uint8_t eom, int count,
                    const uint8_t (*data)[ATP_DATA], const int *lens, const uint32_t *users)
{
    seen *s = find_seen(skt, node, rskt, tid);
    response *keep = NULL;
    if (s && s->xo)
        for (int i = 0; i < MAX_RSPS && !keep; i++)
            if (!rsps[i].used)
                keep = &rsps[i];
    if (keep) {
        memset(keep, 0, sizeof *keep);
        *keep = (response){ true, skt, node, rskt, tid, count, { { 0 } }, { 0 }, now_ms() + 30000 };
    }
    for (int i = 0; i < count; i++) {
        uint8_t pkt[8 + ATP_DATA];
        int n = lens[i] < 0 ? 0 : lens[i] > ATP_DATA ? ATP_DATA : lens[i];
        put_header(pkt, (uint8_t)(TRESP | (i == count - 1 ? eom : 0)), (uint8_t)i, tid, users ? users[i] : 0);
        memcpy(pkt + 8, data[i], (size_t)n);
        at_send_ddp(node, rskt, skt, DDP_ATP, pkt, 8 + n);
        if (keep) {
            memcpy(keep->pkt[i], pkt, (size_t)(8 + n));
            keep->len[i] = 8 + n;
        }
    }
}

static int16_t send_response(gaddr pb, bool add)
{
    int skt = rd8(pb + P_SOCKET);
    if (!atp_sockets[skt])
        return badATPSkt;
    uint8_t node = rd8(pb + P_ADDR + 2), rskt = rd8(pb + P_ADDR + 3);
    uint16_t tid = rd16(pb + P_TRANSID);
    uint8_t eom = rd8(pb + P_FLAGS) & atpEOMvalue ? EOM : 0;
    if (add) { /* one response, from reqPointer */
        uint8_t pkt[8 + ATP_DATA];
        int n = (int16_t)rd16(pb + P_REQLEN);
        n = n < 0 ? 0 : n > ATP_DATA ? ATP_DATA : n;
        put_header(pkt, (uint8_t)(TRESP | eom), rd8(pb + P_RSPNUM), tid, rd32(pb + P_USERDATA));
        gget(pkt + 8, rd32(pb + P_REQPTR), (uint32_t)n);
        at_send_ddp(node, rskt, (uint8_t)skt, DDP_ATP, pkt, 8 + n);
        return noErr;
    }
    int count = rd8(pb + P_BDSSIZE);
    if (count < 1 || count > 8)
        return badBuffNum;
    static uint8_t data[8][ATP_DATA];
    int lens[8];
    uint32_t users[8];
    gaddr bds = rd32(pb + P_BDS);
    for (int i = 0; i < count; i++) {
        gaddr e = bds + (uint32_t)i * BDS_SIZE;
        int n = (int16_t)rd16(e + 6);
        lens[i] = n = n < 0 ? 0 : n > ATP_DATA ? ATP_DATA : n;
        users[i] = rd32(e + 8);
        gget(data[i], rd32(e + 2), (uint32_t)n);
    }
    respond((uint8_t)skt, node, rskt, tid, eom, count, (const uint8_t(*)[ATP_DATA])data, lens, users);
    return noErr;
}

static int16_t kill_get(gaddr q, int skt)
{
    bool found = false;
    for (int i = 0; i < MAX_GETS; i++)
        if (getters[i].used && (q ? getters[i].pb == q : getters[i].skt == skt)) {
            getters[i].used = false;
            finish(getters[i].pb, reqAborted);
            found = true;
        }
    return found || skt ? noErr : cbNotFound;
}

static int16_t atp_control(gaddr pb, bool async)
{
    int code = (int16_t)rd16(pb + CntrlParam_csCode_off);
    switch (code) {
    case openATPSkt: {
        int s = at_open_socket(rd8(pb + P_SOCKET), atp_in);
        if (!s)
            return tooManySkts;
        atp_sockets[s] = true;
        filters[s] = rd32(pb + P_ADDR);
        wr8(pb + P_SOCKET, (uint8_t)s);
        LOGF(LOG_TRAPS, "  ATP socket %d opened\n", s);
        return noErr;
    }
    case closeATPSkt: {
        int s = rd8(pb + P_SOCKET);
        if (!atp_sockets[s])
            return badATPSkt;
        kill_get(0, s);
        for (int i = 0; i < MAX_REQS; i++)
            if (reqs[i].used && reqs[i].skt == s)
                end_request(&reqs[i], reqAborted);
        for (int i = 0; i < MAX_RSPS; i++)
            if (rsps[i].skt == s)
                rsps[i].used = false;
        atp_sockets[s] = false;
        at_close_socket(s);
        return noErr;
    }
    case sendRequest: return start_request(pb, async, true);
    case nSendRequest: return start_request(pb, async, false);
    case getRequest: {
        int s = rd8(pb + P_SOCKET);
        if (!atp_sockets[s])
            return badATPSkt;
        for (int i = 0; i < MAX_GETS; i++)
            if (!getters[i].used) {
                getters[i] = (getter){ true, pb, (uint8_t)s };
                return wait_for(pb, async);
            }
        return tooManyReqs;
    }
    case sendResponse: return send_response(pb, false);
    case addResponse: return send_response(pb, true);
    case relRspCB: {
        int s = rd8(pb + P_SOCKET);
        uint8_t node = rd8(pb + P_ADDR + 2), rskt = rd8(pb + P_ADDR + 3);
        uint16_t tid = rd16(pb + P_TRANSID);
        for (int i = 0; i < MAX_RSPS; i++)
            if (rsps[i].used && rsps[i].skt == s && rsps[i].node == node && rsps[i].rskt == rskt && rsps[i].tid == tid) {
                rsps[i].used = false;
                return noErr;
            }
        return cbNotFound;
    }
    case relTCB: {
        uint8_t node = rd8(pb + P_ADDR + 2);
        uint16_t tid = rd16(pb + P_TRANSID);
        for (int i = 0; i < MAX_REQS; i++)
            if (reqs[i].used && reqs[i].node == node && reqs[i].tid == tid) {
                end_request(&reqs[i], reqAborted);
                return noErr;
            }
        return cbNotFound;
    }
    case killSendReq: {
        gaddr q = rd32(pb + P_KILLQEL);
        for (int i = 0; i < MAX_REQS; i++)
            if (reqs[i].used && reqs[i].pb == q) {
                end_request(&reqs[i], reqAborted);
                return noErr;
            }
        return cbNotFound;
    }
    case killGetReq: return kill_get(rd32(pb + P_KILLQEL), 0);
    case killAllGetReq: return kill_get(0, rd8(pb + P_SOCKET));
    default: return -17; /* controlErr */
    }
}

static int16_t atp_open(gaddr pb) { return at_open(); }

const native_driver atp_driver = { ".ATP", 10, atp_open, NULL, atp_control, NULL, NULL, NULL, NULL };

/* ---------------------------------------------------------------------- */
/* .XPP: ZIP's calls (xCall) on a network without a router.  XCallParam:
 * xppSubCode, xppTimeout, xppRetry, filler, zipBuffPtr, zipNumZones,
 * zipLastFlag. */

enum { xCall = 246, zipGetLocalZones = 5, zipGetZoneList = 6, zipGetMyZone = 7 };
enum { X_SUBCODE = 28, X_BUFF = 34, X_NUMZONES = 38, X_LASTFLAG = 40 };

static int16_t xpp_control(gaddr pb, bool async)
{
    if ((int16_t)rd16(pb + CntrlParam_csCode_off) != xCall)
        return xpp_asp_control(pb, async); /* ASP and AFP (xpp.c) */
    switch ((int16_t)rd16(pb + X_SUBCODE)) {
    case zipGetMyZone: {
        static const uint8_t star[2] = { 1, '*' };
        gput(rd32(pb + X_BUFF), star, 2);
        wr16(pb + X_NUMZONES, 1);
        wr8(pb + X_LASTFLAG, 1);
        return noErr;
    }
    case zipGetLocalZones:
    case zipGetZoneList:
        wr16(pb + X_NUMZONES, 0);
        wr8(pb + X_LASTFLAG, 1);
        return noBridgeErr;
    default: return -17;
    }
}

static int16_t xpp_open(gaddr pb) { return at_open(); }

const native_driver xpp_driver = { ".XPP", 40, xpp_open, NULL, xpp_control, NULL, NULL, NULL, NULL };

/* ---------------------------------------------------------------------- */
/* ATP for host code (ASP in asp.c): a socket whose requests go to a C
 * function, transactions waited for while the stack runs (as the
 * driver's synchronous calls wait), and responses sent from C. */

int atp_host_open(atp_request_fn fn, void *ctx)
{
    if (at_open() != noErr)
        return 0;
    int s = at_open_socket(0, atp_in);
    if (!s)
        return 0;
    atp_sockets[s] = true;
    filters[s] = 0;
    host_fns[s] = fn;
    host_ctx[s] = ctx;
    return s;
}

void atp_host_close(int s)
{
    if (s <= 0 || s > 254 || !atp_sockets[s])
        return;
    for (int i = 0; i < MAX_REQS; i++)
        if (reqs[i].used && reqs[i].skt == s)
            end_request(&reqs[i], reqAborted);
    for (int i = 0; i < MAX_RSPS; i++)
        if (rsps[i].skt == s)
            rsps[i].used = false;
    atp_sockets[s] = false;
    host_fns[s] = NULL;
    at_close_socket(s);
}

int16_t atp_host_start(int skt, uint8_t node, uint8_t dskt, uint32_t user, const uint8_t *data, int len, bool xo,
                       int nresp, int timeout_ms, int retries, atp_reply *out)
{
    if (skt <= 0 || skt > 254 || !atp_sockets[skt])
        return badATPSkt;
    if (nresp < 1 || nresp > 8 || len < 0 || len > ATP_DATA)
        return badBuffNum;
    request *q = NULL;
    for (int i = 0; i < MAX_REQS && !q; i++)
        if (!reqs[i].used)
            q = &reqs[i];
    if (!q)
        return tooManyReqs;
    memset(q, 0, sizeof *q);
    memset(out, 0, sizeof *out);
    out->result = ioInProgress;
    q->used = true;
    q->host = out;
    q->skt = (uint8_t)skt;
    q->node = node;
    q->dskt = dskt;
    q->tid = ++next_tid;
    q->nbufs = nresp;
    q->bitmap = (uint8_t)((1 << nresp) - 1);
    q->xo = xo;
    q->timeout = (uint64_t)(timeout_ms > 0 ? timeout_ms : 1000);
    q->retries = retries;
    put_header(q->req, (uint8_t)(TREQ | (xo ? XO : 0)), q->bitmap, q->tid, user);
    if (len)
        memcpy(q->req + 8, data, (size_t)len);
    q->reqlen = 8 + len;
    send_request(q);
    q->next = now_ms() + q->timeout;
    return noErr;
}

int16_t atp_host_request(int skt, uint8_t node, uint8_t dskt, uint32_t user, const uint8_t *data, int len, bool xo,
                         int nresp, int timeout_ms, int retries, atp_reply *out)
{
    int16_t e = atp_host_start(skt, node, dskt, user, data, len, xo, nresp, timeout_ms, retries, out);
    if (e != noErr)
        return e;
    request *q = NULL;
    for (int i = 0; i < MAX_REQS && !q; i++)
        if (reqs[i].used && reqs[i].host == out)
            q = &reqs[i];
    /* A deadline of its own too, should the poll not be running. */
    uint64_t timeout = (uint64_t)(timeout_ms > 0 ? timeout_ms : 1000);
    uint64_t give_up = now_ms() + timeout * (uint64_t)(retries < 0 ? 1000 : retries + 2);
    while (out->result == ioInProgress) {
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
        appletalk_poll();
        if (out->result == ioInProgress && now_ms() > give_up && q && q->used && q->host == out)
            end_request(q, reqFailed);
    }
    return out->result;
}

void atp_host_respond(int skt, uint8_t node, uint8_t rskt, uint16_t tid, int count, const uint8_t (*data)[578],
                      const int *lens, const uint32_t *users)
{
    if (skt > 0 && skt < 255 && atp_sockets[skt] && count >= 1 && count <= 8)
        respond((uint8_t)skt, node, rskt, tid, EOM, count, data, lens, users);
}

/* A request of host code's given up: its responses go nowhere now. */
void atp_host_cancel(atp_reply *out)
{
    for (int i = 0; i < MAX_REQS; i++)
        if (reqs[i].used && reqs[i].host == out) {
            reqs[i].host = NULL;
            reqs[i].used = false;
            if (reqs[i].own_skt) {
                atp_sockets[reqs[i].skt] = false;
                at_close_socket(reqs[i].skt);
            }
        }
}

bool atp_host_xo(int skt, uint8_t node, uint8_t rskt, uint16_t tid)
{
    seen *s = skt > 0 && skt < 255 ? find_seen((uint8_t)skt, node, rskt, tid) : NULL;
    return s && s->xo;
}

void atp_host_send_once(int skt, uint8_t node, uint8_t dskt, uint32_t user, const uint8_t *data, int len)
{
    uint8_t pkt[8 + ATP_DATA];
    len = len < 0 ? 0 : len > ATP_DATA ? ATP_DATA : len;
    put_header(pkt, TREQ, 1, ++next_tid, user);
    if (len)
        memcpy(pkt + 8, data, (size_t)len);
    at_send_ddp(node, dskt, (uint8_t)skt, DDP_ATP, pkt, 8 + len);
}
