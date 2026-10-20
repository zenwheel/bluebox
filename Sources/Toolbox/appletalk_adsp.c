/*
 * ADSP, the AppleTalk Data Stream Protocol (the .DSP driver), on
 * appletalk.c's DDP: connection ends and connection listeners, each on a
 * DDP socket of its own, exchanging DDP type 7 packets.
 *
 * Each packet starts with a 13-byte header: source connection ID, the
 * sequence number of its first data byte, the next byte the sender
 * expects, its receive window, and a descriptor (control, ack request,
 * EOM and attention bits, and for control packets a code: probe or ack,
 * open request, open ack, open request and ack, open deny, close advice,
 * forward reset and its ack, retransmit advice).  Open packets add the
 * ADSP version, the destination connection ID and the attention receive
 * sequence.
 *
 * The client's CCB is kept up to date (state, user flags, the remote
 * address, attention fields), and its user routine is called, A1 the CCB,
 * on close, tear-down, attention and forward reset.  Data waits in our own
 * queues, of the sizes the client gave.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tb_internal.h"
#include "trap.h"

enum { DDP_ADSP = 7, ADSP_DATA = 572, HDR = 13, MAX_ENDS = 16, MAX_WAIT = 8 };
enum { D_CONTROL = 0x80, D_ACKREQ = 0x40, D_EOM = 0x20, D_ATTN = 0x10 };
enum { C_PROBE = 0, C_OPENREQ = 1, C_OPENACK = 2, C_OPENREQACK = 3, C_DENY = 4, C_CLOSE = 5, C_RESET = 6, C_RESETACK = 7,
       C_RETRANSMIT = 8 };
enum {
    dspInit = 255, dspRemove = 254, dspOpen = 253, dspClose = 252, dspCLInit = 251, dspCLRemove = 250,
    dspCLListen = 249, dspCLDeny = 248, dspStatus = 247, dspRead = 246, dspWrite = 245, dspAttention = 244,
    dspOptions = 243, dspReset = 242, dspNewCID = 241,
};
enum { ocRequest = 1, ocPassive = 2, ocAccept = 3, ocEstablish = 4 };
enum { sListening = 1, sPassive = 2, sOpening = 3, sOpen = 4, sClosing = 5, sClosed = 6 };
enum { eClosed = 0x80, eTearDown = 0x40, eAttention = 0x20, eFwdReset = 0x10 };
enum {
    errRefNum = -1280, errAborted = -1279, errState = -1278, errOpening = -1277, errAttention = -1276,
    errFwdReset = -1275, errDSPQueueSize = -1274, errOpenDenied = -1273,
};
/* DSPParamBlock, after csCode */
enum {
    P_REFNUM = 32,
    P_CCB = 34, P_ROUTINE = 38, P_SENDQSIZE = 42, P_RECVQSIZE = 48, P_ATTNPTR = 54, P_LOCALSKT = 58,
    P_LOCALCID = 34, P_REMOTECID = 36, P_REMOTEADDR = 38, P_FILTER = 42, P_SENDSEQ = 46, P_SENDWINDOW = 50,
    P_RECVSEQ = 52, P_ATTNSENDSEQ = 56, P_ATTNRECVSEQ = 60, P_OCMODE = 64, P_OCINTERVAL = 65, P_OCMAX = 66,
    P_ABORT = 34,
    P_STATUSCCB = 34, P_SENDPEND = 38, P_SENDFREE = 40, P_RECVPEND = 42, P_RECVFREE = 44,
    P_REQCOUNT = 34, P_ACTCOUNT = 36, P_DATAPTR = 38, P_EOM = 42, P_FLUSH = 43,
    P_ATTNCODE = 34, P_ATTNSIZE = 36, P_ATTNDATA = 38,
    P_NEWCID = 34,
};
/* TRCCB */
enum { CCB_STATE = 6, CCB_FLAGS = 8, CCB_SOCKET = 9, CCB_REMOTE = 10, CCB_ATTNCODE = 14, CCB_ATTNSIZE = 16, CCB_ATTNPTR = 18 };

typedef struct {
    bool used, listener;
    gaddr ccb, routine, attnbuf;
    uint8_t skt;
    int state;
    uint16_t cid, rcid;
    uint8_t rnode, rskt;
    uint32_t filter;
    /* opening */
    gaddr open_pb, listen_pb;
    int oc_tries;
    uint64_t oc_next, oc_interval;
    /* data */
    uint32_t send_seq; /* the next byte we send (the first unsent) */
    uint32_t acked;    /* the remote has everything before this */
    uint32_t recv_seq; /* the next byte we expect */
    uint32_t attn_send, attn_recv;
    uint8_t *sendq, *recvq;
    int sendcap, recvcap, sendlen, recvlen;
    int sent;          /* of sendq, bytes sent and awaiting acknowledgement */
    int eoms[64];      /* receive queue offsets just after an end of message */
    int neoms;
    bool send_eom_at_end;
    uint64_t rexmit;
    gaddr reads[MAX_WAIT];
    int nreads;
} dsp_end;

static dsp_end ends[MAX_ENDS];
static uint16_t next_cid = 0x1000;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static dsp_end *end_of(int refnum) { return refnum >= 1 && refnum <= MAX_ENDS && ends[refnum - 1].used ? &ends[refnum - 1] : NULL; }
static int refnum_of(dsp_end *e) { return (int)(e - ends) + 1; }

static void set_state(dsp_end *e, int s)
{
    e->state = s;
    if (e->ccb)
        wr16(e->ccb + CCB_STATE, (uint16_t)s);
}

static void send_packet(dsp_end *e, uint8_t desc, uint32_t first, const uint8_t *data, int n)
{
    uint8_t p[HDR + 8 + ADSP_DATA];
    p[0] = (uint8_t)(e->cid >> 8);
    p[1] = (uint8_t)e->cid;
    put32(p + 2, first);
    put32(p + 6, e->recv_seq);
    int window = e->recvcap - e->recvlen;
    p[10] = (uint8_t)((window > 0xFFFF ? 0xFFFF : window) >> 8);
    p[11] = (uint8_t)(window > 0xFFFF ? 0xFFFF : window);
    p[12] = desc;
    if (n > 0)
        memcpy(p + HDR, data, (size_t)n);
    at_send_ddp(e->rnode, e->rskt, e->skt, DDP_ADSP, p, HDR + (n > 0 ? n : 0));
}

static void send_control(dsp_end *e, int code, bool ackreq)
{
    send_packet(e, (uint8_t)(D_CONTROL | (ackreq ? D_ACKREQ : 0) | code), e->send_seq, NULL, 0);
}

/* Open packets: version 0x0100, the destination's connection ID, our
 * attention receive sequence. */
static void send_open(dsp_end *e, int code)
{
    uint8_t x[8];
    x[0] = 1;
    x[1] = 0;
    x[2] = (uint8_t)(e->rcid >> 8);
    x[3] = (uint8_t)e->rcid;
    put32(x + 4, e->attn_recv);
    send_packet(e, (uint8_t)(D_CONTROL | code), e->send_seq, x, 8);
}

static void finish(gaddr pb, int16_t r)
{
    LOGF(LOG_TRAPS, "  ADSP call completes -> %d\n", r);
    at_complete(pb, r);
}

/* The client's user routine, A1 the CCB, with its user flags set. */
static void event(dsp_end *e, uint8_t flags)
{
    if (!e->ccb)
        return;
    wr8(e->ccb + CCB_FLAGS, (uint8_t)(rd8(e->ccb + CCB_FLAGS) | flags));
    if (!e->routine || !g_cpu)
        return;
    cpu68k *c = g_cpu;
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d);
    memcpy(a, c->a, sizeof a);
    uint16_t sr = c->sr;
    c->a[1] = e->ccb;
    guest_call_regs(e->routine);
    memcpy(c->d, d, sizeof d);
    memcpy(c->a, a, sizeof a);
    c->sr = sr;
}

/* ---------------------------------------------------------------------- */
/* Data */

static bool read_done(dsp_end *e, gaddr pb, bool closing)
{
    int want = (int16_t)rd16(pb + P_REQCOUNT);
    int have = (int16_t)rd16(pb + P_ACTCOUNT);
    int upto = e->recvlen, eom = 0;
    if (e->neoms && e->eoms[0] <= e->recvlen) {
        upto = e->eoms[0];
        eom = 1;
    }
    int n = upto < want - have ? upto : want - have;
    if (n > 0) {
        gput(rd32(pb + P_DATAPTR) + (uint32_t)have, e->recvq, (uint32_t)n);
        memmove(e->recvq, e->recvq + n, (size_t)(e->recvlen - n));
        e->recvlen -= n;
        for (int i = 0; i < e->neoms; i++)
            e->eoms[i] -= n;
        have += n;
        wr16(pb + P_ACTCOUNT, (uint16_t)have);
    }
    bool at_eom = eom && n == upto;
    if (at_eom) {
        memmove(e->eoms, e->eoms + 1, sizeof e->eoms[0] * (size_t)--e->neoms);
        wr8(pb + P_EOM, 1);
    } else {
        wr8(pb + P_EOM, 0);
    }
    return have >= want || at_eom || closing;
}

static void serve_reads(dsp_end *e)
{
    bool closing = e->state == sClosed && !e->recvlen;
    while (e->nreads) {
        gaddr pb = e->reads[0];
        if (!read_done(e, pb, closing))
            break;
        memmove(e->reads, e->reads + 1, sizeof e->reads[0] * (size_t)--e->nreads);
        finish(pb, noErr);
    }
}

/* Send what the remote has room for, from the first unsent byte. */
static void push(dsp_end *e)
{
    if (e->state != sOpen)
        return;
    while (e->sent < e->sendlen) {
        int n = e->sendlen - e->sent;
        n = n > ADSP_DATA ? ADSP_DATA : n;
        bool last = e->sent + n == e->sendlen;
        uint8_t desc = (uint8_t)(D_ACKREQ | (last && e->send_eom_at_end ? D_EOM : 0));
        send_packet(e, desc, e->acked + (uint32_t)e->sent, e->sendq + e->sent, n);
        e->sent += n;
    }
    e->send_seq = e->acked + (uint32_t)e->sendlen;
    e->rexmit = now_ms() + 1000;
}

static void got_ack(dsp_end *e, uint32_t next)
{
    int done = (int)(next - e->acked);
    if (done <= 0 || done > e->sendlen)
        return;
    memmove(e->sendq, e->sendq + done, (size_t)(e->sendlen - done));
    e->sendlen -= done;
    e->sent -= done;
    if (e->sent < 0)
        e->sent = 0;
    e->acked = next;
    if (!e->sendlen)
        e->send_eom_at_end = false;
}

static void closed(dsp_end *e, uint8_t why)
{
    set_state(e, sClosed);
    event(e, why);
    serve_reads(e);
}

/* ---------------------------------------------------------------------- */
/* Packets */

static dsp_end *end_on(uint8_t skt)
{
    for (int i = 0; i < MAX_ENDS; i++)
        if (ends[i].used && ends[i].skt == skt)
            return &ends[i];
    return NULL;
}

static void adsp_in(uint8_t skt, uint8_t snode, uint8_t sskt, uint8_t type, const uint8_t *p, int len)
{
    dsp_end *e = end_on(skt);
    if (!e || type != DDP_ADSP || len < HDR)
        return;
    uint16_t scid = (uint16_t)(p[0] << 8 | p[1]);
    uint32_t first = be32(p + 2), next = be32(p + 6);
    uint8_t desc = p[12];
    int code = desc & 0x0F;
    const uint8_t *data = p + HDR;
    int n = len - HDR;
    if (desc & D_CONTROL && (code == C_OPENREQ || code == C_OPENREQACK || code == C_OPENACK || code == C_DENY)) {
        if (e->listener) { /* a request for a listener: to its CLListen */
            if (code != C_OPENREQ || !e->listen_pb)
                return;
            uint32_t f = e->filter;
            if ((uint8_t)(f >> 8) && (uint8_t)(f >> 8) != snode)
                return;
            gaddr pb = e->listen_pb;
            e->listen_pb = 0;
            wr16(pb + P_REMOTECID, scid);
            wr16(pb + P_REMOTEADDR, 0);
            wr8(pb + P_REMOTEADDR + 2, snode);
            wr8(pb + P_REMOTEADDR + 3, sskt);
            wr32(pb + P_SENDSEQ, next);
            wr16(pb + P_SENDWINDOW, (uint16_t)(p[10] << 8 | p[11]));
            wr32(pb + P_ATTNSENDSEQ, n >= 8 ? be32(data + 4) : 0);
            finish(pb, noErr);
            return;
        }
        if (code == C_DENY && e->state == sOpening) {
            set_state(e, sClosed);
            gaddr pb = e->open_pb;
            e->open_pb = 0;
            if (pb)
                finish(pb, errOpenDenied);
            return;
        }
        if ((code == C_OPENREQ && e->state == sPassive) || (code == C_OPENREQACK && e->state == sOpening)) {
            e->rnode = snode;
            e->rskt = sskt;
            e->rcid = scid;
            e->recv_seq = first;
            e->acked = e->send_seq = next;
            if (code == C_OPENREQ) {
                send_open(e, C_OPENREQACK); /* and wait for its ack */
                return;
            }
            send_open(e, C_OPENACK);
        } else if (!(code == C_OPENACK && (e->state == sPassive || e->state == sOpening))) {
            return;
        }
        if (e->ccb) {
            wr16(e->ccb + CCB_REMOTE, 0);
            wr8(e->ccb + CCB_REMOTE + 2, e->rnode);
            wr8(e->ccb + CCB_REMOTE + 3, e->rskt);
        }
        set_state(e, sOpen);
        gaddr pb = e->open_pb;
        e->open_pb = 0;
        if (pb) {
            wr16(pb + P_REMOTECID, e->rcid);
            wr8(pb + P_REMOTEADDR + 2, e->rnode);
            wr8(pb + P_REMOTEADDR + 3, e->rskt);
            finish(pb, noErr);
        }
        push(e);
        return;
    }
    if (e->state != sOpen && e->state != sClosing)
        return;
    got_ack(e, next);
    if (desc & D_CONTROL) {
        switch (code) {
        case C_CLOSE: closed(e, eClosed); return;
        case C_RESET:
            e->recvlen = e->neoms = 0;
            e->recv_seq = first;
            send_control(e, C_RESETACK, false);
            event(e, eFwdReset);
            for (int i = 0; i < e->nreads; i++)
                finish(e->reads[i], errFwdReset);
            e->nreads = 0;
            return;
        case C_RETRANSMIT: e->sent = 0; push(e); return;
        default: break; /* probe or ack */
        }
        if (desc & D_ACKREQ)
            send_control(e, C_PROBE, false);
        return;
    }
    if (desc & D_ATTN) {
        if (n >= 2 && e->attnbuf) {
            int an = n - 2 > 570 ? 570 : n - 2;
            gput(e->attnbuf, data + 2, (uint32_t)an);
            wr16(e->ccb + CCB_ATTNCODE, (uint16_t)(data[0] << 8 | data[1]));
            wr16(e->ccb + CCB_ATTNSIZE, (uint16_t)an);
            wr32(e->ccb + CCB_ATTNPTR, e->attnbuf);
        }
        e->attn_recv++;
        send_packet(e, D_CONTROL | D_ATTN | C_PROBE, e->send_seq, NULL, 0);
        event(e, eAttention);
        return;
    }
    if (first != e->recv_seq) { /* out of order: ask for the rest again */
        if ((int32_t)(first - e->recv_seq) > 0)
            send_control(e, C_RETRANSMIT, false);
        else if (desc & D_ACKREQ)
            send_control(e, C_PROBE, false);
        return;
    }
    int room = e->recvcap - e->recvlen;
    if (n > room)
        n = room;
    memcpy(e->recvq + e->recvlen, data, (size_t)n);
    e->recvlen += n;
    e->recv_seq += (uint32_t)n;
    if (desc & D_EOM && e->neoms < 64)
        e->eoms[e->neoms++] = e->recvlen;
    if (desc & D_ACKREQ || desc & D_EOM)
        send_control(e, C_PROBE, false);
    serve_reads(e);
}

/* ---------------------------------------------------------------------- */
/* Time: open requests and unacknowledged data again. */

void adsp_poll(void)
{
    uint64_t t = now_ms();
    for (int i = 0; i < MAX_ENDS; i++) {
        dsp_end *e = &ends[i];
        if (!e->used)
            continue;
        if (e->state == sOpening && e->open_pb && t >= e->oc_next) {
            if (e->oc_tries-- <= 0) {
                set_state(e, sClosed);
                gaddr pb = e->open_pb;
                e->open_pb = 0;
                finish(pb, errOpening);
                continue;
            }
            send_open(e, C_OPENREQ);
            e->oc_next = t + e->oc_interval;
        }
        if (e->state == sOpen && e->sent > 0 && t >= e->rexmit) {
            e->sent = 0; /* again, from the first unacknowledged byte */
            push(e);
        }
    }
}

void adsp_reset(void) { memset(ends, 0, sizeof ends); }

/* ---------------------------------------------------------------------- */
/* The driver */

static int16_t wait_for(gaddr pb, bool async)
{
    wr16(pb + IOParam_ioResult_off, ioInProgress);
    if (async)
        return ioInProgress;
    while ((int16_t)rd16(pb + IOParam_ioResult_off) == ioInProgress && !g_cpu->halted) {
        struct timespec ts = { 0, 2000000 };
        nanosleep(&ts, NULL);
        appletalk_poll();
    }
    return (int16_t)rd16(pb + IOParam_ioResult_off);
}

static void free_end(dsp_end *e)
{
    at_close_socket(e->skt);
    free(e->sendq);
    free(e->recvq);
    for (int i = 0; i < e->nreads; i++)
        finish(e->reads[i], errAborted);
    if (e->open_pb)
        finish(e->open_pb, errAborted);
    if (e->listen_pb)
        finish(e->listen_pb, errAborted);
    memset(e, 0, sizeof *e);
}

static int16_t new_end(gaddr pb, bool listener)
{
    dsp_end *e = NULL;
    for (int i = 0; i < MAX_ENDS && !e; i++)
        if (!ends[i].used)
            e = &ends[i];
    if (!e)
        return errRefNum;
    int sq = (int16_t)rd16(pb + P_SENDQSIZE), rq = (int16_t)rd16(pb + P_RECVQSIZE);
    if (!listener && (sq < 100 || rq < 100))
        return errDSPQueueSize;
    int skt = at_open_socket(rd8(pb + P_LOCALSKT), adsp_in);
    if (!skt)
        return -91; /* ddpSktErr */
    memset(e, 0, sizeof *e);
    e->used = true;
    e->listener = listener;
    e->ccb = rd32(pb + P_CCB);
    e->routine = rd32(pb + P_ROUTINE);
    e->attnbuf = rd32(pb + P_ATTNPTR);
    e->skt = (uint8_t)skt;
    e->sendcap = sq > 0 ? sq : 0;
    e->recvcap = rq > 0 ? rq : 0;
    e->sendq = malloc((size_t)(e->sendcap ? e->sendcap : 1));
    e->recvq = malloc((size_t)(e->recvcap ? e->recvcap : 1));
    if (e->ccb) {
        gmemset(e->ccb, 0, 242);
        wr16(e->ccb + 4, (uint16_t)refnum_of(e));
        wr8(e->ccb + CCB_SOCKET, (uint8_t)skt);
    }
    set_state(e, listener ? sListening : sClosed);
    wr16(pb + P_REFNUM, (uint16_t)refnum_of(e));
    wr8(pb + P_LOCALSKT, (uint8_t)skt);
    LOGF(LOG_TRAPS, "  ADSP %s %d on socket %d\n", listener ? "listener" : "connection end", refnum_of(e), skt);
    return noErr;
}

static int16_t dsp_open(dsp_end *e, gaddr pb, bool async)
{
    if (e->listener || (e->state != sClosed))
        return errState;
    e->cid = rd16(pb + P_LOCALCID) ? rd16(pb + P_LOCALCID) : ++next_cid;
    wr16(pb + P_LOCALCID, e->cid);
    e->rnode = rd8(pb + P_REMOTEADDR + 2);
    e->rskt = rd8(pb + P_REMOTEADDR + 3);
    e->filter = rd32(pb + P_FILTER);
    e->send_seq = e->acked = rd32(pb + P_SENDSEQ);
    e->recv_seq = rd32(pb + P_RECVSEQ);
    e->sendlen = e->recvlen = e->sent = e->neoms = 0;
    int mode = rd8(pb + P_OCMODE);
    int interval = rd8(pb + P_OCINTERVAL), max = rd8(pb + P_OCMAX);
    e->oc_interval = (uint64_t)(interval ? interval : 6) * 10 * 1000 / 60;
    e->oc_tries = max ? max : 3;
    switch (mode) {
    case ocEstablish:
        e->rcid = rd16(pb + P_REMOTECID);
        set_state(e, sOpen);
        return noErr;
    case ocAccept: /* a listener's request: acknowledge it and ask in turn */
        e->rcid = rd16(pb + P_REMOTECID);
        e->open_pb = pb;
        set_state(e, sOpening);
        send_open(e, C_OPENREQACK);
        e->oc_next = now_ms() + e->oc_interval * (uint64_t)(e->oc_tries + 1); /* no repeats: just a timeout */
        e->oc_tries = 0;
        return wait_for(pb, async);
    case ocPassive:
        e->open_pb = pb;
        set_state(e, sPassive);
        return wait_for(pb, async);
    case ocRequest:
        e->rcid = 0;
        e->open_pb = pb;
        set_state(e, sOpening);
        e->oc_next = 0; /* the poll sends it */
        adsp_poll();
        return wait_for(pb, async);
    default: return errState;
    }
}

static int16_t dsp_control(gaddr pb, bool async)
{
    int code = (int16_t)rd16(pb + CntrlParam_csCode_off);
    if (code == dspInit || code == dspCLInit)
        return new_end(pb, code == dspCLInit);
    if (code == dspNewCID) {
        wr16(pb + P_NEWCID, ++next_cid);
        return noErr;
    }
    dsp_end *e = end_of((int16_t)rd16(pb + P_REFNUM));
    if (!e)
        return errRefNum;
    switch (code) {
    case dspRemove:
    case dspCLRemove:
        if (e->state == sOpen)
            send_control(e, C_CLOSE, false);
        free_end(e);
        return noErr;
    case dspOpen: return dsp_open(e, pb, async);
    case dspClose:
        if (e->state == sOpen) {
            if (!rd8(pb + P_ABORT))
                push(e);
            send_control(e, C_CLOSE, false);
        }
        set_state(e, sClosed);
        serve_reads(e);
        if (e->open_pb) {
            finish(e->open_pb, errAborted);
            e->open_pb = 0;
        }
        return noErr;
    case dspCLListen:
        if (!e->listener || e->listen_pb)
            return errState;
        e->filter = rd32(pb + P_FILTER);
        e->listen_pb = pb;
        return wait_for(pb, async);
    case dspCLDeny: {
        dsp_end tmp = *e; /* the deny goes from the listener's socket */
        tmp.cid = 0;
        tmp.rcid = rd16(pb + P_REMOTECID);
        tmp.rnode = rd8(pb + P_REMOTEADDR + 2);
        tmp.rskt = rd8(pb + P_REMOTEADDR + 3);
        send_open(&tmp, C_DENY);
        return noErr;
    }
    case dspStatus:
        wr32(pb + P_STATUSCCB, e->ccb);
        wr16(pb + P_SENDPEND, (uint16_t)e->sendlen);
        wr16(pb + P_SENDFREE, (uint16_t)(e->sendcap - e->sendlen));
        wr16(pb + P_RECVPEND, (uint16_t)e->recvlen);
        wr16(pb + P_RECVFREE, (uint16_t)(e->recvcap - e->recvlen));
        return noErr;
    case dspRead:
        if (e->state != sOpen && e->state != sClosing && !e->recvlen)
            return errState;
        if (e->nreads >= MAX_WAIT)
            return errState;
        wr16(pb + P_ACTCOUNT, 0);
        if (!e->nreads && read_done(e, pb, e->state == sClosed))
            return noErr; /* there already */
        e->reads[e->nreads++] = pb;
        return wait_for(pb, async);
    case dspWrite: {
        if (e->state != sOpen)
            return errState;
        int n = (int16_t)rd16(pb + P_REQCOUNT);
        int room = e->sendcap - e->sendlen;
        if (n > room)
            n = room;
        if (n > 0)
            gget(e->sendq + e->sendlen, rd32(pb + P_DATAPTR), (uint32_t)n);
        e->sendlen += n;
        wr16(pb + P_ACTCOUNT, (uint16_t)n);
        if (rd8(pb + P_EOM))
            e->send_eom_at_end = true;
        if (rd8(pb + P_FLUSH) || rd8(pb + P_EOM) || e->sendlen >= e->sendcap / 2)
            push(e);
        return noErr;
    }
    case dspAttention: {
        if (e->state != sOpen)
            return errState;
        int n = (int16_t)rd16(pb + P_ATTNSIZE);
        if (n < 0 || n > 570)
            return errAttention;
        uint8_t x[2 + 570];
        uint16_t acode = rd16(pb + P_ATTNCODE);
        x[0] = (uint8_t)(acode >> 8);
        x[1] = (uint8_t)acode;
        if (n)
            gget(x + 2, rd32(pb + P_ATTNDATA), (uint32_t)n);
        send_packet(e, D_ATTN | D_ACKREQ, e->attn_send++, x, 2 + n);
        return noErr;
    }
    case dspReset:
        if (e->state != sOpen)
            return errState;
        e->sendlen = e->sent = 0;
        send_control(e, C_RESET, true);
        return noErr;
    case dspOptions: return noErr;
    default: return -17; /* controlErr */
    }
}

int16_t adsp_control(gaddr pb, bool async) { return dsp_control(pb, async); }

static int16_t dsp_driver_open(gaddr pb) { return at_open(); }

const native_driver dsp_driver = { ".DSP", 42, dsp_driver_open, NULL, dsp_control, NULL, NULL, NULL, NULL };
