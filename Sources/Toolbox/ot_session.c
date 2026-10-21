/*
 * Open Transport's pap and asp endpoints: the Printer Access Protocol and
 * the AppleTalk Session Protocol, on ATP transactions from host code
 * (appletalk_atp.c), both ends of each: a workstation connects, and an
 * endpoint bound with a queue listens on its session listening socket
 * (SLS) and accepts connections on other endpoints.
 *
 * PAP (Inside AppleTalk, chapter 10): ATP's user bytes carry the
 * connection ID, the function and two bytes more.  OpenConn goes to the
 * SLS (our responding socket, flow quantum and wait time) and is answered
 * with the other end's responding socket, flow quantum, a result and a
 * status string.  Each end reads from the other with SendData, a request
 * the other holds until it has data to write: its answer is up to a flow
 * quantum of 512-byte responses, the last with PAP's EOF bit when the
 * writer has finished.  Here a write is OTSnd, the EOF is OTSnd-
 * OrderlyDisconnect and comes to the other end as T_ORDREL after the
 * data; the end whose release completes last sends CloseConn.  Tickles
 * every 60 seconds, and a connection not heard from for two minutes ends.
 * SendStatus at the SLS is answered with an empty status.
 *
 * ASP (as asp.c, the AppleShare client's): OpenSess to the SLS names the
 * workstation's session socket (WSS); the reply, the server's session
 * socket (SSS) and the session ID.  A command is OTSndRequest, an XO
 * request to the SSS, and its reply OTRcvReply; the server hears it with
 * OTRcvRequest and answers with OTSndReply.  As for atp endpoints, the
 * first four bytes of a reply's data are the ATP user bytes of its first
 * response, here ASP's command result.  An attention (OTSnd with
 * T_EXPEDITED, a two-byte code) goes from server to workstation and is
 * read there as T_EXDATA.  GetStatus is OTSndURequest to a server's
 * address and OTRcvUReply.  Tickles every 30 seconds; two minutes of
 * silence ends a session.  ASP's Write is not done.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "machine.h"
#include "ot_internal.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

enum { ATP_PKT = 578, PAP_PKT = 512, QUANTUM = 8, PAP_QUEUE = 2 * QUANTUM * PAP_PKT, MAX_OUT = 4, MAX_REQS = 8 };
enum { CONN_TIMEOUT = 120000 };
/* PAP's functions */
enum { OPEN_CONN = 1, OPEN_CONN_REPLY = 2, SEND_DATA = 3, PAP_DATA = 4, PAP_TICKLE = 5, CLOSE_CONN = 6,
       CLOSE_CONN_REPLY = 7, SEND_STATUS = 8, STATUS = 9 };
/* ASP's */
enum { CLOSE_SESS = 1, COMMAND = 2, GET_STATUS = 3, OPEN_SESS = 4, ASP_TICKLE = 5, ASP_WRITE = 6, ATTENTION = 8 };
enum { ASP_VERSION = 0x0100, aspServerBusy = -1071 };

/* A command of ours, its reply coming (ASP). */
typedef struct {
    bool used, told;
    uint32_t seq;
    atp_reply reply;
} asked;

/* A command heard, waiting for OTRcvRequest and then its reply (ASP). */
typedef struct {
    bool used, taken;
    uint32_t seq;
    uint16_t tid;
    uint8_t rskt;
    int len;
    uint8_t data[ATP_PKT];
} heard;

typedef struct ot_session {
    bool pap;
    bool live;            /* a connection: ours to answer, tickle and close */
    bool server;          /* accepted from a listener */
    uint8_t node, rskt;   /* the other end: PAP its responding socket; ASP the SSS, or the WSS */
    uint8_t sls;          /* ASP workstation: the server's SLS, for tickles */
    uint8_t id;           /* PAP's connection ID, ASP's session ID */
    uint8_t quantum;      /* PAP: the other end's flow quantum */
    uint64_t heard_at, tickle_at;
    /* a listener: the open request heard */
    bool have_call;
    uint8_t cnode, crskt, cfrom, cid, cquantum;
    uint16_t ctid;
    /* opening */
    bool opening;
    atp_reply open;
    /* PAP's data: what we write, waiting for the other end's SendData */
    uint8_t sendq[PAP_QUEUE];
    int sendlen;
    bool eof_out, eof_sent;
    bool held;            /* its SendData, held */
    uint16_t htid, hseq, last_seq;
    uint8_t hrskt;
    /* and what we read with ours */
    bool reading;
    uint16_t rseq;
    atp_reply rd;
    uint8_t recvq[PAP_QUEUE];
    int recvlen;
    bool eof_in;
    /* ASP's commands, both ways */
    uint16_t cmdseq;
    uint32_t seq;
    asked out[MAX_OUT];
    heard reqs[MAX_REQS];
    bool told_request;
    bool have_attn, told_attn;
    uint16_t attn;
    /* GetStatus */
    bool status_asked, told_status;
    uint32_t status_seq;
    atp_reply status;
    /* CloseConn, CloseSess and attentions sent: their replies go here */
    atp_reply scratch;
} ot_session;

static uint8_t next_sid = 1;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint32_t user_bytes(int b0, int b1, int word)
{
    return (uint32_t)(b0 & 0xFF) << 24 | (uint32_t)(b1 & 0xFF) << 16 | (uint16_t)word;
}

static bool blocking(provider *p) { return p->blocking && !p->async; }

static bool wait_slice(provider *p)
{
    struct timespec ts = { 0, 1000000 };
    nanosleep(&ts, NULL);
    appletalk_poll();
    return ot_wait_slice(p);
}

static bool is_session(provider *p) { return p && p->used && (p->kind == P_PAP || p->kind == P_ASP) && p->ss; }

static uint8_t skt_of(provider *p) { return p->at ? ota_socket(p) : 0; }

/* ---------------------------------------------------------------------- */
/* Connections */

static void begin(provider *p, uint8_t node, uint8_t rskt, uint8_t id)
{
    ot_session *s = p->ss;
    s->live = true;
    s->server = false;
    s->node = node;
    s->rskt = rskt;
    s->id = id;
    s->heard_at = s->tickle_at = now_ms();
    s->sendlen = s->recvlen = 0;
    s->eof_out = s->eof_sent = s->eof_in = s->reading = false; /* (a SendData held stays) */
    s->rseq = 0;
    s->last_seq = 0;
    s->have_attn = s->told_attn = false;
    memset(s->out, 0, sizeof s->out);
    memset(s->reqs, 0, sizeof s->reqs);
}

/* The connection's state let go of (its socket stays bound). */
static void end(provider *p)
{
    ot_session *s = p->ss;
    if (s->reading)
        atp_host_cancel(&s->rd);
    for (int i = 0; i < MAX_OUT; i++)
        if (s->out[i].used)
            atp_host_cancel(&s->out[i].reply);
    s->live = s->reading = s->held = false;
    memset(s->out, 0, sizeof s->out);
    memset(s->reqs, 0, sizeof s->reqs);
}

/* The other end gone: T_DISCONNECT until OTRcvDisconnect. */
static void lost(provider *p, int32_t reason)
{
    end(p);
    if (p->state == T_IDLE || p->state == T_UNBND)
        return;
    p->disconnected = true;
    p->disc_reason = reason;
}

/* Tell the other end the connection is over. */
static void send_close(provider *p)
{
    ot_session *s = p->ss;
    if (!s->live)
        return;
    uint32_t user = s->pap ? user_bytes(s->id, CLOSE_CONN, 0) : user_bytes(CLOSE_SESS, s->id, 0);
    atp_host_cancel(&s->scratch);
    atp_host_start(skt_of(p), s->node, s->rskt, user, NULL, 0, true, 1, 1000, 2, &s->scratch);
}

static provider *session_by_id(bool pap, uint8_t node, uint8_t id)
{
    for (int i = 0; i < MAX_PROVIDERS; i++) {
        provider *p = &ot_providers[i];
        if (is_session(p) && p->ss->pap == pap && p->ss->live && p->ss->server && p->ss->node == node &&
            p->ss->id == id)
            return p;
    }
    return NULL;
}

static void opened(provider *p);
static void pap_write(provider *p);

/* ---------------------------------------------------------------------- */
/* PAP: requests heard */

static void pap_heard(provider *p, uint8_t skt, uint8_t node, uint8_t rskt, uint16_t tid, uint32_t user,
                      const uint8_t *data, int len)
{
    ot_session *s = p->ss;
    int id = (int)(user >> 24), fn = (int)(user >> 16 & 0xFF);
    static uint8_t pkt[1][ATP_PKT];
    int lens[1] = { 0 };
    if (fn == SEND_STATUS) { /* (at the SLS) an empty status */
        uint32_t u = user_bytes(0, STATUS, 0);
        memset(pkt[0], 0, 5);
        lens[0] = 5;
        atp_host_respond(skt, node, rskt, tid, 1, (const uint8_t(*)[ATP_PKT])pkt, lens, &u);
        return;
    }
    if (fn == OPEN_CONN) {
        if (p->qlen <= 0 || s->have_call || len < 4)
            return; /* (it asks again) */
        s->have_call = true;
        s->cnode = node;
        s->crskt = data[0];
        s->cquantum = data[1] ? data[1] : 1;
        s->cid = (uint8_t)id;
        s->cfrom = rskt;
        s->ctid = tid;
        return;
    }
    if (s->opening)
        opened(p);
    if (s->opening && fn == SEND_DATA && node == s->node && id == s->id) {
        s->held = true; /* before the reply to our OpenConn has been seen */
        s->htid = tid;
        s->hrskt = rskt;
        s->hseq = (uint16_t)user;
        return;
    }
    if (!s->live || node != s->node || id != s->id)
        return;
    s->heard_at = now_ms();
    switch (fn) {
    case SEND_DATA: {
        uint16_t seq = (uint16_t)user;
        if (seq && seq == s->last_seq)
            return; /* answered already; ATP sends its responses again */
        s->held = true;
        s->htid = tid;
        s->hrskt = rskt;
        s->hseq = seq;
        pap_write(p); /* at once, if there is something to write */
        break;
    }
    case CLOSE_CONN: {
        uint32_t u = user_bytes(s->id, CLOSE_CONN_REPLY, 0);
        atp_host_respond(skt, node, rskt, tid, 1, (const uint8_t(*)[ATP_PKT])pkt, lens, &u);
        if (p->state == T_IDLE)
            end(p); /* (after the release: over) */
        else
            lost(p, kECONNRESETErr);
        break;
    }
    default: break; /* tickles: heard */
    }
}

/* Answer the other end's SendData with what there is to write. */
static void pap_write(provider *p)
{
    ot_session *s = p->ss;
    if (!s->live || !s->held || (!s->sendlen && !(s->eof_out && !s->eof_sent)))
        return;
    int max = s->quantum * PAP_PKT, n = s->sendlen < max ? s->sendlen : max;
    bool eof = s->eof_out && n == s->sendlen;
    static uint8_t pkts[QUANTUM][ATP_PKT];
    int lens[QUANTUM] = { 0 }, count = 0;
    uint32_t users[QUANTUM];
    do {
        int k = n - count * PAP_PKT;
        k = k > PAP_PKT ? PAP_PKT : k;
        if (k > 0)
            memcpy(pkts[count], s->sendq + count * PAP_PKT, (size_t)k);
        lens[count] = k > 0 ? k : 0;
        users[count] = user_bytes(s->id, PAP_DATA, 0);
        count++;
    } while (count * PAP_PKT < n);
    if (eof)
        users[count - 1] = user_bytes(s->id, PAP_DATA, 0x0100);
    atp_host_respond(skt_of(p), s->node, s->hrskt, s->htid, count, (const uint8_t(*)[ATP_PKT])pkts, lens, users);
    memmove(s->sendq, s->sendq + n, (size_t)(s->sendlen - n));
    s->sendlen -= n;
    s->last_seq = s->hseq;
    s->held = false;
    if (eof)
        s->eof_sent = true;
}

/* Ask the other end for data, while there is room for a quantum of it. */
static void pap_read(provider *p)
{
    ot_session *s = p->ss;
    if (!s->live || s->reading || s->eof_in || PAP_QUEUE - s->recvlen < QUANTUM * PAP_PKT)
        return;
    s->rseq = s->rseq == 0xFFFF ? 1 : s->rseq + 1;
    if (atp_host_start(skt_of(p), s->node, s->rskt, user_bytes(s->id, SEND_DATA, s->rseq), NULL, 0, true, QUANTUM,
                       15000, -1, &s->rd) == noErr)
        s->reading = true;
}

static void pap_poll(provider *p)
{
    ot_session *s = p->ss;
    if (s->reading && s->rd.result != ioInProgress) {
        s->reading = false;
        if (s->rd.result == noErr) {
            s->heard_at = now_ms();
            for (int i = 0; i < s->rd.count; i++) {
                int k = s->rd.len[i];
                k = k > PAP_QUEUE - s->recvlen ? PAP_QUEUE - s->recvlen : k;
                memcpy(s->recvq + s->recvlen, s->rd.data[i], (size_t)k);
                s->recvlen += k;
            }
            if (s->rd.count && (s->rd.user[s->rd.count - 1] >> 8 & 0xFF))
                s->eof_in = true;
        }
    }
    pap_write(p);
    pap_read(p);
}

/* ---------------------------------------------------------------------- */
/* ASP: requests heard */

static void asp_heard(provider *p, uint8_t skt, uint8_t node, uint8_t rskt, uint16_t tid, uint32_t user,
                      const uint8_t *data, int len)
{
    ot_session *s = p->ss;
    int fn = (int)(user >> 24), sid = (int)(user >> 16 & 0xFF);
    static uint8_t pkt[1][ATP_PKT];
    int lens[1] = { 0 };
    if (p->qlen > 0) { /* the SLS */
        if (fn == GET_STATUS) {
            uint32_t u = 0;
            atp_host_respond(skt, node, rskt, tid, 1, (const uint8_t(*)[ATP_PKT])pkt, lens, &u);
        } else if (fn == OPEN_SESS && !s->have_call) {
            s->have_call = true;
            s->cnode = node;
            s->crskt = (uint8_t)sid; /* the WSS */
            s->cfrom = rskt;
            s->ctid = tid;
        } else if (fn == ASP_TICKLE) {
            provider *w = session_by_id(false, node, (uint8_t)sid);
            if (w)
                w->ss->heard_at = now_ms();
        }
        return;
    }
    if (s->opening)
        opened(p);
    if (!s->live || node != s->node || sid != s->id)
        return;
    s->heard_at = now_ms();
    switch (fn) {
    case COMMAND:
        for (int i = 0; i < MAX_REQS; i++) {
            heard *h = &s->reqs[i];
            if (h->used)
                continue;
            memset(h, 0, sizeof *h);
            h->used = true;
            h->tid = tid;
            h->rskt = rskt;
            h->len = len > ATP_PKT ? ATP_PKT : len;
            memcpy(h->data, data, (size_t)h->len);
            return;
        }
        break;
    case ATTENTION:
        atp_host_respond(skt, node, rskt, tid, 1, (const uint8_t(*)[ATP_PKT])pkt, lens, NULL);
        s->have_attn = true;
        s->told_attn = false;
        s->attn = (uint16_t)user;
        break;
    case CLOSE_SESS:
        atp_host_respond(skt, node, rskt, tid, 1, (const uint8_t(*)[ATP_PKT])pkt, lens, NULL);
        lost(p, kECONNRESETErr);
        break;
    default: break; /* tickles: heard */
    }
}

static void heard_fn(void *ctx, uint8_t skt, uint8_t node, uint8_t rskt, uint16_t tid, uint32_t user,
                     const uint8_t *data, int len)
{
    provider *p = ctx;
    if (!is_session(p))
        return;
    if (p->ss->pap)
        pap_heard(p, skt, node, rskt, tid, user, data, len);
    else
        asp_heard(p, skt, node, rskt, tid, user, data, len);
}

/* ---------------------------------------------------------------------- */
/* Providers */

int32_t ots_open(provider *p)
{
    p->ss = calloc(1, sizeof *p->ss);
    if (!p->ss)
        return kOTOutOfMemoryErr;
    p->ss->pap = p->kind == P_PAP;
    return kOTNoError;
}

void ots_close(provider *p)
{
    if (!p->ss)
        return;
    send_close(p);
    end(p);
    atp_host_cancel(&p->ss->scratch);
    atp_host_cancel(&p->ss->status);
    free(p->ss);
    p->ss = NULL;
}

/* A socket for the endpoint (its SLS when it listens): 0 if none. */
uint8_t ots_bind(provider *p, int qlen)
{
    p->qlen = qlen > 0 ? 1 : 0;
    p->ss->have_call = false;
    return (uint8_t)atp_host_open(heard_fn, p);
}

void ots_unbind(provider *p, uint8_t skt)
{
    send_close(p);
    end(p);
    atp_host_cancel(&p->ss->scratch);
    atp_host_cancel(&p->ss->status);
    p->ss->status_asked = false;
    if (skt)
        atp_host_close(skt);
}

/* TEndpointInfo's transport data unit sizes and service type. */
void ots_info(gaddr info, provider_kind kind)
{
    wr32(info + 8, kind == P_ASP ? 8 * ATP_PKT : 0);
    wr32(info + 12, kind == P_ASP ? 2 : (uint32_t)-2);
    wr32(info + 24, kind == P_ASP ? 5 : 2); /* T_TRANS, T_COTS_ORD */
}

/* OTConnect(ep, sndCall, rcvCall): OpenConn or OpenSess to the SLS. */
void ots_Connect(ppc_cpu *c, provider *p)
{
    ot_session *s = p->ss;
    gaddr snd = R(1);
    uint8_t node, sls;
    if (p->state != T_IDLE || p->qlen > 0) {
        RETURN((uint32_t)kOTOutStateErr);
        return;
    }
    if (!snd || !ota_address(p, rd32(snd + 8), rd32(snd + 4), &node, &sls)) {
        RETURN((uint32_t)kOTBadAddressErr);
        return;
    }
    end(p);
    p->connected = p->told_connect = p->disconnected = p->told_disc = false;
    p->eof = p->told_ordrel = p->told_data = p->flow = false;
    s->node = node;
    s->sls = sls;
    int16_t e;
    if (s->pap) {
        s->id = (uint8_t)(1 + rand() % 255);
        uint8_t d[4] = { skt_of(p), QUANTUM, 0, 0 };
        e = atp_host_start(skt_of(p), node, sls, user_bytes(s->id, OPEN_CONN, 0), d, 4, true, 1, 2000, 7, &s->open);
    } else {
        e = atp_host_start(skt_of(p), node, sls, user_bytes(OPEN_SESS, skt_of(p), ASP_VERSION), NULL, 0, true, 1,
                           1000, 5, &s->open);
    }
    if (e != noErr) {
        RETURN((uint32_t)kOTOutOfMemoryErr);
        return;
    }
    s->opening = true;
    p->state = T_OUTCON;
    appletalk_poll();
    LOGF(LOG_TRAPS, "OT: %s %d connects to node %u socket %u\n", s->pap ? "pap" : "asp", (int)(p - ot_providers),
         node, sls);
    if (!blocking(p)) {
        RETURN((uint32_t)kOTNoDataErr); /* T_CONNECT comes */
        return;
    }
    while (s->opening && wait_slice(p))
        opened(p);
    if (!p->connected) {
        p->state = T_IDLE;
        p->told_disc = true;
        RETURN((uint32_t)(p->disconnected ? p->disc_reason : kETIMEDOUTErr));
        return;
    }
    p->state = T_DATAXFER;
    p->told_connect = true;
    if (R(2))
        ota_netbuf_ddp(R(2), s->node, s->sls);
    RETURN(kOTNoError);
}

/* The open request's reply, when it has come: the connection is up, and
 * the endpoint in T_DATAXFER once OTRcvConnect says so. */
static void opened(provider *p)
{
    ot_session *s = p->ss;
    if (!s->opening || s->open.result == ioInProgress)
        return;
    s->opening = false;
    int32_t err = s->open.result == noErr ? kOTNoError : kETIMEDOUTErr;
    uint8_t rskt = 0, quantum = QUANTUM;
    if (err == kOTNoError && s->pap) {
        const uint8_t *d = s->open.data[0];
        if (s->open.len[0] < 4 || (d[2] << 8 | d[3]))
            err = kECONNREFUSEDErr; /* the printer is busy */
        rskt = d[0];
        quantum = d[1] ? d[1] : 1;
    } else if (err == kOTNoError) {
        uint32_t u = s->open.user[0];
        if ((int16_t)(u & 0xFFFF))
            err = kECONNREFUSEDErr;
        rskt = (uint8_t)(u >> 24);
        s->id = (uint8_t)(u >> 16);
    }
    if (err != kOTNoError) {
        p->disconnected = true;
        p->disc_reason = err;
        return;
    }
    uint8_t sls = s->sls;
    begin(p, s->node, rskt, s->id);
    s->sls = sls;
    s->quantum = quantum > QUANTUM ? QUANTUM : quantum;
    p->connected = true;
    if (s->pap)
        pap_read(p);
}

void ots_RcvConnect(ppc_cpu *c, provider *p)
{
    ot_session *s = p->ss;
    if (p->state != T_OUTCON) {
        RETURN((uint32_t)kOTOutStateErr);
        return;
    }
    while (s->opening && blocking(p) && wait_slice(p))
        opened(p);
    opened(p);
    if (p->disconnected) {
        RETURN((uint32_t)kOTLookErr);
        return;
    }
    if (!p->connected) {
        RETURN((uint32_t)kOTNoDataErr);
        return;
    }
    p->state = T_DATAXFER;
    p->told_connect = true;
    if (R(1))
        ota_netbuf_ddp(R(1), s->node, s->sls);
    RETURN(kOTNoError);
}

/* OTListen(ep, call): the open request the SLS heard. */
void ots_Listen(ppc_cpu *c, provider *p)
{
    ot_session *s = p->ss;
    if (p->qlen <= 0) {
        RETURN((uint32_t)kOTNotSupportedErr);
        return;
    }
    while (!s->have_call) {
        if (!blocking(p) || !wait_slice(p)) {
            p->told_listen = false;
            RETURN((uint32_t)kOTNoDataErr);
            return;
        }
    }
    gaddr call = R(1);
    if (call) {
        ota_netbuf_ddp(call, s->cnode, s->crskt);
        wr32(call + 16, 0);
        wr32(call + 28, 0);
        wr32(call + 36, 1); /* sequence */
    }
    RETURN(kOTNoError);
}

/* OTAccept(listener, worker, call): the worker (bound now if need be)
 * answers the open request from the listener's SLS. */
void ots_Accept(ppc_cpu *c, provider *p, provider *w)
{
    ot_session *s = p->ss;
    if (!is_session(w) || w->kind != p->kind || w == p) { /* (the SLS stays a listener) */
        RETURN((uint32_t)kOTBadReferenceErr);
        return;
    }
    if (!s->have_call) {
        RETURN((uint32_t)kOTNoDataErr);
        return;
    }
    if (w->state == T_UNBND && ota_bind_any(w) != kOTNoError) {
        RETURN((uint32_t)kOTOutOfMemoryErr);
        return;
    }
    ot_session *ws = w->ss;
    static uint8_t pkt[1][ATP_PKT];
    int lens[1] = { 0 };
    uint32_t u;
    uint8_t id;
    if (s->pap) {
        id = s->cid;
        u = user_bytes(id, OPEN_CONN_REPLY, 0);
        pkt[0][0] = skt_of(w);
        pkt[0][1] = QUANTUM;
        pkt[0][2] = pkt[0][3] = 0; /* result: open */
        pkt[0][4] = 0;             /* status: "" */
        lens[0] = 5;
    } else {
        id = next_sid++;
        if (!next_sid)
            next_sid = 1;
        u = user_bytes(skt_of(w), id, 0);
    }
    atp_host_respond(skt_of(p), s->cnode, s->cfrom, s->ctid, 1, (const uint8_t(*)[ATP_PKT])pkt, lens, &u);
    s->have_call = false;
    p->told_listen = false;
    w->connected = w->told_connect = w->disconnected = w->told_disc = false;
    w->eof = w->told_ordrel = w->told_data = w->flow = false;
    begin(w, s->cnode, s->crskt, id);
    ws->server = true;
    ws->quantum = s->pap ? (s->cquantum > QUANTUM ? QUANTUM : s->cquantum) : QUANTUM;
    w->state = T_DATAXFER;
    if (ws->pap)
        pap_read(w);
    if (w != p)
        ot_post(w, T_PASSCON, kOTNoError, 0);
    appletalk_poll();
    ot_complete(c, p, T_ACCEPTCOMPLETE, kOTNoError, R(2));
}

/* OTSnd: PAP's data, or an ASP server's attention (T_EXPEDITED). */
void ots_Snd(ppc_cpu *c, provider *p)
{
    ot_session *s = p->ss;
    gaddr buf = R(1);
    uint32_t n = R(2);
    if (p->state != T_DATAXFER && p->state != T_INREL) {
        RETURN((uint32_t)(p->disconnected || p->eof ? kOTLookErr : kOTOutStateErr));
        return;
    }
    if (!s->pap) {
        if (!(R(3) & T_EXPEDITED) || ot_data_len(buf, n) < 2) {
            RETURN((uint32_t)kOTNotSupportedErr);
            return;
        }
        uint8_t code[2];
        ot_data_get(code, buf, n, 2);
        atp_host_cancel(&s->scratch);
        atp_host_start(skt_of(p), s->node, s->rskt, user_bytes(ATTENTION, s->id, code[0] << 8 | code[1]), NULL, 0,
                       true, 1, 2000, 5, &s->scratch);
        appletalk_poll();
        RETURN(2);
        return;
    }
    uint32_t want = ot_data_len(buf, n), done = 0;
    uint8_t *data = malloc(want ? want : 1);
    if (!data) {
        RETURN((uint32_t)kOTOutOfMemoryErr);
        return;
    }
    ot_data_get(data, buf, n, want);
    while (done < want) {
        int room = PAP_QUEUE - s->sendlen;
        if (room <= 0) {
            pap_poll(p);
            if (blocking(p) && s->live && wait_slice(p))
                continue;
            if (!done) {
                p->flow = true;
                free(data);
                RETURN((uint32_t)kOTFlowErr);
                return;
            }
            break;
        }
        uint32_t k = want - done < (uint32_t)room ? want - done : (uint32_t)room;
        memcpy(s->sendq + s->sendlen, data + done, k);
        s->sendlen += (int)k;
        done += k;
        pap_write(p);
    }
    free(data);
    appletalk_poll();
    RETURN(done);
}

/* OTRcv: PAP's data, or an ASP workstation's attention. */
void ots_Rcv(ppc_cpu *c, provider *p)
{
    ot_session *s = p->ss;
    gaddr buf = R(1), flags = R(3);
    uint32_t n = R(2);
    if (flags)
        wr32(flags, 0);
    if (p->state != T_DATAXFER && p->state != T_OUTREL && p->state != T_INREL) {
        RETURN((uint32_t)(p->disconnected ? kOTLookErr : kOTOutStateErr));
        return;
    }
    if (!s->pap) {
        if (!s->have_attn) {
            RETURN((uint32_t)(p->disconnected ? kOTLookErr : kOTNoDataErr));
            return;
        }
        uint8_t code[2] = { (uint8_t)(s->attn >> 8), (uint8_t)s->attn };
        uint32_t k = n < 2 ? n : 2;
        gput(buf, code, k);
        s->have_attn = s->told_attn = false;
        if (flags)
            wr32(flags, T_EXPEDITED);
        RETURN(k);
        return;
    }
    for (;;) {
        pap_poll(p);
        if (s->recvlen > 0)
            break;
        if (p->disconnected || (s->eof_in && !s->reading)) {
            RETURN((uint32_t)kOTLookErr); /* T_ORDREL (or T_DISCONNECT) */
            return;
        }
        if (!blocking(p) || !wait_slice(p)) {
            p->told_data = false;
            RETURN((uint32_t)kOTNoDataErr);
            return;
        }
    }
    uint32_t k = n < (uint32_t)s->recvlen ? n : (uint32_t)s->recvlen;
    gput(buf, s->recvq, k);
    memmove(s->recvq, s->recvq + k, (size_t)(s->recvlen - (int)k));
    s->recvlen -= (int)k;
    if (!s->recvlen)
        p->told_data = false;
    pap_read(p);
    RETURN(k);
}

static bool pap_released(provider *p) { return p->ss->pap && p->ss->live && p->ss->eof_in && !p->ss->recvlen; }

static asked *answered(ot_session *s)
{
    for (int i = 0; i < MAX_OUT; i++)
        if (s->out[i].used && s->out[i].reply.result != ioInProgress)
            return &s->out[i];
    return NULL;
}

static heard *waiting_request(ot_session *s)
{
    for (int i = 0; i < MAX_REQS; i++)
        if (s->reqs[i].used && !s->reqs[i].taken)
            return &s->reqs[i];
    return NULL;
}

void ots_Look(ppc_cpu *c, provider *p)
{
    ot_session *s = p->ss;
    ots_poll(p);
    if (p->disconnected)
        RETURN(T_DISCONNECT);
    else if (p->state == T_OUTCON && p->connected)
        RETURN(T_CONNECT);
    else if (s->have_call)
        RETURN(T_LISTEN);
    else if (s->have_attn)
        RETURN(T_EXDATA);
    else if (s->pap && s->recvlen)
        RETURN(T_DATA);
    else if (pap_released(p) && (p->state == T_DATAXFER || p->state == T_OUTREL))
        RETURN(T_ORDREL);
    else if (!s->pap && answered(s))
        RETURN(T_REPLY);
    else if (!s->pap && waiting_request(s))
        RETURN(T_REQUEST);
    else
        RETURN(0);
}

void ots_CountDataBytes(ppc_cpu *c, provider *p)
{
    int n = p->ss->pap ? p->ss->recvlen : 0;
    if (R(1))
        wr32(R(1), (uint32_t)n);
    RETURN(n > 0 ? kOTNoError : kOTNoDataErr);
}

/* PAP's EOF: the end of what we write; the other end's T_ORDREL. */
void ots_SndOrderlyDisconnect(ppc_cpu *c, provider *p)
{
    ot_session *s = p->ss;
    if (!s->pap) {
        RETURN((uint32_t)kOTNotSupportedErr);
        return;
    }
    if (p->state != T_DATAXFER && p->state != T_INREL) {
        RETURN((uint32_t)kOTOutStateErr);
        return;
    }
    s->eof_out = true;
    pap_write(p);
    appletalk_poll();
    p->state = p->state == T_INREL ? T_IDLE : T_OUTREL;
    if (p->state == T_IDLE)
        p->eof = p->told_ordrel = false;
    RETURN(kOTNoError);
}

void ots_RcvOrderlyDisconnect(ppc_cpu *c, provider *p)
{
    if (!pap_released(p) || (p->state != T_DATAXFER && p->state != T_OUTREL)) {
        RETURN((uint32_t)(p->ss->pap ? kOTNoReleaseErr : kOTNotSupportedErr));
        return;
    }
    if (p->state == T_OUTREL) { /* both done: this end closes */
        p->state = T_IDLE;
        send_close(p);
        end(p);
        p->eof = p->told_ordrel = false;
    } else {
        p->state = T_INREL;
    }
    RETURN(kOTNoError);
}

/* OTSndDisconnect: end the connection, or refuse an open request. */
void ots_SndDisconnect(ppc_cpu *c, provider *p)
{
    ot_session *s = p->ss;
    if (s->have_call) {
        static uint8_t pkt[1][ATP_PKT];
        int lens[1] = { 0 };
        uint32_t u;
        if (s->pap) {
            u = user_bytes(s->cid, OPEN_CONN_REPLY, 0);
            memset(pkt[0], 0, 5);
            pkt[0][2] = pkt[0][3] = 0xFF; /* busy */
            lens[0] = 5;
        } else {
            u = (uint16_t)aspServerBusy;
        }
        atp_host_respond(ota_socket(p), s->cnode, s->cfrom, s->ctid, 1, (const uint8_t(*)[ATP_PKT])pkt, lens, &u);
        s->have_call = false;
        p->told_listen = false;
    } else {
        if (p->state == T_OUTCON)
            atp_host_cancel(&s->open);
        s->opening = false;
        send_close(p);
        end(p);
        p->state = T_IDLE;
        p->connected = p->told_connect = p->disconnected = p->told_disc = false;
        p->eof = p->told_ordrel = p->told_data = p->flow = false;
    }
    appletalk_poll();
    ot_complete(c, p, T_DISCONNECTCOMPLETE, kOTNoError, R(1));
}

void ots_RcvDisconnect(ppc_cpu *c, provider *p)
{
    if (!p->disconnected) {
        RETURN((uint32_t)kOTNoDisconnectErr);
        return;
    }
    if (R(1)) {
        wr32(R(1) + 4, 0);
        wr32(R(1) + 12, (uint32_t)p->disc_reason);
    }
    end(p);
    p->state = T_IDLE;
    p->connected = p->told_connect = p->disconnected = p->told_disc = false;
    p->eof = p->told_ordrel = p->told_data = p->flow = false;
    RETURN(kOTNoError);
}

/* ---------------------------------------------------------------------- */
/* ASP's transactions: TRequest and TReply are data, opt, sequence */

static provider *asp_endpoint(ppc_cpu *c)
{
    provider *p = ot_provider_of(R(0));
    if (!is_session(p) || p->ss->pap) {
        RETURN((uint32_t)(p ? kOTNotSupportedErr : kOTBadReferenceErr));
        return NULL;
    }
    return p;
}

/* OTSndRequest(ep, req, reqFlags): a command to the server. */
static void ots_SndRequest(ppc_cpu *c)
{
    provider *p = asp_endpoint(c);
    if (!p)
        return;
    ot_session *s = p->ss;
    gaddr req = R(1);
    if (p->state != T_DATAXFER || !s->live || p->qlen > 0) {
        RETURN((uint32_t)kOTOutStateErr);
        return;
    }
    uint32_t n = req ? ot_data_len(rd32(req + 8), rd32(req + 4)) : 0;
    if (n > ATP_PKT) {
        RETURN((uint32_t)kOTBadDataErr);
        return;
    }
    uint8_t data[ATP_PKT];
    if (req)
        ot_data_get(data, rd32(req + 8), rd32(req + 4), n);
    asked *q = NULL;
    for (int i = 0; i < MAX_OUT && !q; i++)
        if (!s->out[i].used)
            q = &s->out[i];
    if (!q) {
        RETURN((uint32_t)kOTFlowErr);
        return;
    }
    if (atp_host_start(skt_of(p), s->node, s->rskt, user_bytes(COMMAND, s->id, s->cmdseq++), data, (int)n, true,
                       8, 2000, 15, &q->reply) != noErr) {
        RETURN((uint32_t)kOTFlowErr);
        return;
    }
    q->used = true;
    q->told = false;
    q->seq = ++s->seq;
    if (req)
        wr32(req + 24, q->seq);
    appletalk_poll();
    RETURN(kOTNoError);
}

/* The result's four bytes, then the responses' data, into a TNetbuf. */
static void put_reply(gaddr nb, const atp_reply *r)
{
    static uint8_t data[4 + 8 * ATP_PKT];
    uint32_t u = r->user[0];
    data[0] = (uint8_t)(u >> 24);
    data[1] = (uint8_t)(u >> 16);
    data[2] = (uint8_t)(u >> 8);
    data[3] = (uint8_t)u;
    int n = 4;
    for (int i = 0; i < r->count; i++) {
        memcpy(data + n, r->data[i], (size_t)r->len[i]);
        n += r->len[i];
    }
    uint32_t room = rd32(nb), k = (uint32_t)n < room ? (uint32_t)n : room;
    gput(rd32(nb + 8), data, k);
    wr32(nb + 4, k);
}

/* OTRcvReply(ep, reply, &replyFlags) */
static void ots_RcvReply(ppc_cpu *c)
{
    provider *p = asp_endpoint(c);
    if (!p)
        return;
    ot_session *s = p->ss;
    gaddr reply = R(1);
    if (R(2))
        wr32(R(2), 0);
    asked *q;
    while (!(q = answered(s))) {
        bool asking = false;
        for (int i = 0; i < MAX_OUT; i++)
            asking |= s->out[i].used;
        if (!asking || !blocking(p) || !wait_slice(p)) {
            RETURN((uint32_t)kOTNoDataErr);
            return;
        }
    }
    q->used = false;
    if (reply)
        wr32(reply + 24, q->seq);
    if (q->reply.result != noErr) {
        if (reply)
            wr32(reply + 4, 0);
        lost(p, kETIMEDOUTErr);
        RETURN((uint32_t)kETIMEDOUTErr);
        return;
    }
    if (reply)
        put_reply(reply, &q->reply);
    RETURN(kOTNoError);
}

/* OTRcvRequest(ep, req, &reqFlags): a command heard. */
static void ots_RcvRequest(ppc_cpu *c)
{
    provider *p = asp_endpoint(c);
    if (!p)
        return;
    ot_session *s = p->ss;
    gaddr req = R(1);
    if (R(2))
        wr32(R(2), 0);
    heard *h;
    while (!(h = waiting_request(s))) {
        if (!blocking(p) || !wait_slice(p)) {
            s->told_request = false;
            RETURN((uint32_t)kOTNoDataErr);
            return;
        }
    }
    h->taken = true;
    h->seq = ++s->seq;
    if (req) {
        uint32_t room = rd32(req), k = (uint32_t)h->len < room ? (uint32_t)h->len : room;
        gput(rd32(req + 8), h->data, k);
        wr32(req + 4, k);
        wr32(req + 16, 0);
        wr32(req + 24, h->seq);
    }
    if (R(2))
        wr32(R(2), T_ACKNOWLEDGED);
    RETURN(kOTNoError);
}

/* OTSndReply(ep, reply, replyFlags): the result, then up to eight
 * responses of data. */
static void ots_SndReply(ppc_cpu *c)
{
    provider *p = asp_endpoint(c);
    if (!p)
        return;
    ot_session *s = p->ss;
    gaddr reply = R(1);
    heard *h = NULL;
    for (int i = 0; i < MAX_REQS && !h && reply; i++)
        if (s->reqs[i].used && s->reqs[i].taken && s->reqs[i].seq == rd32(reply + 24))
            h = &s->reqs[i];
    if (!h) {
        RETURN((uint32_t)kOTBadSequenceErr);
        return;
    }
    uint32_t n = ot_data_len(rd32(reply + 8), rd32(reply + 4));
    if (n > 4 + 8 * ATP_PKT) {
        RETURN((uint32_t)kOTBadDataErr);
        return;
    }
    static uint8_t data[4 + 8 * ATP_PKT];
    memset(data, 0, 4);
    ot_data_get(data, rd32(reply + 8), rd32(reply + 4), n);
    static uint8_t pkts[8][ATP_PKT];
    int lens[8] = { 0 }, count = 0;
    uint32_t users[8] = { (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 | (uint32_t)data[2] << 8 | data[3] };
    for (uint32_t off = 4; count < 8 && (off < n || count == 0); off += ATP_PKT) {
        int k = n > off ? (int)(n - off > ATP_PKT ? ATP_PKT : n - off) : 0;
        memcpy(pkts[count], data + off, (size_t)k);
        lens[count++] = k;
    }
    atp_host_respond(skt_of(p), s->node, h->rskt, h->tid, count, (const uint8_t(*)[ATP_PKT])pkts, lens, users);
    h->used = false;
    appletalk_poll();
    RETURN(kOTNoError);
}

/* GetStatus: OTSndURequest(ep, req TUnitRequest *, flags) to a server's
 * SLS and OTRcvUReply(ep, reply TUnitReply *, &flags) for the status. */
void ots_SndURequest(ppc_cpu *c, provider *p)
{
    ot_session *s = p->ss;
    gaddr req = R(1);
    uint8_t node, sls;
    if (s->pap || p->state == T_UNBND || s->status_asked) {
        RETURN((uint32_t)(s->pap ? kOTNotSupportedErr : kOTOutStateErr));
        return;
    }
    if (!req || !ota_address(p, rd32(req + 8), rd32(req + 4), &node, &sls)) {
        RETURN((uint32_t)kOTBadAddressErr);
        return;
    }
    if (atp_host_start(skt_of(p), node, sls, user_bytes(GET_STATUS, 0, 0), NULL, 0, false, 8, 1000, 3, &s->status) !=
        noErr) {
        RETURN((uint32_t)kOTFlowErr);
        return;
    }
    s->status_asked = true;
    s->told_status = false;
    s->status_seq = ++s->seq;
    wr32(req + 36, s->status_seq);
    appletalk_poll();
    RETURN(kOTNoError);
}

void ots_RcvUReply(ppc_cpu *c, provider *p)
{
    ot_session *s = p->ss;
    gaddr reply = R(1);
    if (R(2))
        wr32(R(2), 0);
    while (!s->status_asked || s->status.result == ioInProgress) {
        if (!s->status_asked || !blocking(p) || !wait_slice(p)) {
            RETURN((uint32_t)kOTNoDataErr);
            return;
        }
    }
    s->status_asked = false;
    if (reply)
        wr32(reply + 24, s->status_seq);
    if (s->status.result != noErr) {
        if (reply)
            wr32(reply + 16, 0);
        RETURN((uint32_t)kETIMEDOUTErr);
        return;
    }
    if (reply) {
        static uint8_t data[8 * ATP_PKT];
        int n = 0;
        for (int i = 0; i < s->status.count; i++) {
            memcpy(data + n, s->status.data[i], (size_t)s->status.len[i]);
            n += s->status.len[i];
        }
        uint32_t room = rd32(reply + 12), k = (uint32_t)n < room ? (uint32_t)n : room;
        gput(rd32(reply + 20), data, k);
        wr32(reply + 16, k);
    }
    RETURN(kOTNoError);
}

/* ---------------------------------------------------------------------- */
/* Events and time */

void ots_poll(provider *p)
{
    ot_session *s = p->ss;
    if (!s)
        return;
    if (s->opening)
        opened(p);
    if (p->state == T_OUTCON) {
        if (p->connected && !p->told_connect) {
            p->told_connect = true;
            ot_post(p, T_CONNECT, 0, 0);
        }
    }
    if (s->have_call && !p->told_listen) {
        p->told_listen = true;
        ot_post(p, T_LISTEN, 0, 0);
    }
    if (s->live) {
        uint64_t t = now_ms();
        if (t - s->heard_at > CONN_TIMEOUT) {
            lost(p, kETIMEDOUTErr);
        } else if (t - s->tickle_at > (s->pap ? 60000u : 30000u)) {
            s->tickle_at = t;
            uint8_t to = !s->pap && s->sls ? s->sls : s->rskt; /* a workstation tickles the SLS */
            uint32_t u = s->pap ? user_bytes(s->id, PAP_TICKLE, 0) : user_bytes(ASP_TICKLE, s->id, 0);
            atp_host_send_once(skt_of(p), s->node, to, u, NULL, 0);
        }
    }
    if (s->pap && s->live) {
        pap_poll(p);
        if (s->recvlen && !p->told_data) {
            p->told_data = true;
            ot_post(p, T_DATA, 0, 0);
        }
        if (pap_released(p) && !p->eof && (p->state == T_DATAXFER || p->state == T_OUTREL)) {
            p->eof = p->told_ordrel = true;
            ot_post(p, T_ORDREL, 0, 0);
        }
        if (p->flow && s->sendlen < PAP_QUEUE) {
            p->flow = false;
            ot_post(p, T_GODATA, 0, 0);
        }
    }
    if (!s->pap) {
        if (s->have_attn && !s->told_attn) {
            s->told_attn = true;
            ot_post(p, T_EXDATA, 0, 0);
        }
        for (int i = 0; i < MAX_OUT; i++)
            if (s->out[i].used && !s->out[i].told && s->out[i].reply.result != ioInProgress) {
                s->out[i].told = true;
                ot_post(p, T_REPLY, 0, 0);
            }
        if (waiting_request(s) && !s->told_request) {
            s->told_request = true;
            ot_post(p, T_REQUEST, 0, 0);
        }
        if (s->status_asked && !s->told_status && s->status.result != ioInProgress) {
            s->told_status = true;
            ot_post(p, T_REPLY, 0, 0);
        }
    }
    if (p->disconnected && !p->told_disc) {
        p->told_disc = true;
        ot_post(p, T_DISCONNECT, 0, 0);
    }
}

const ppc_export ots_exports[] = {
    { "OTSndRequest", ots_SndRequest }, { "OTRcvReply", ots_RcvReply },
    { "OTRcvRequest", ots_RcvRequest }, { "OTSndReply", ots_SndReply },
};
const unsigned ots_export_count = sizeof ots_exports / sizeof ots_exports[0];


