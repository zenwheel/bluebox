/*
 * Open Transport's AppleTalk (OTATalkClientLib, OTATalkSharedLib,
 * OpenTptAppleTalkLib): ddp, atp and adsp endpoints, NBP's mapper and
 * AppleTalk services, on the stack appletalk.c runs for the .MPP, .ATP
 * and .DSP drivers, so Open Transport programs and older ones share one
 * network.
 *
 * A ddp endpoint is a DDP socket of host code's whose datagrams wait in a
 * queue for OTRcvUData.  An atp endpoint is a socket of ATP's host side
 * (appletalk_atp.c): requests heard wait for OTRcvURequest, and requests
 * sent run on while the program does (OTRcvUReply).  An adsp endpoint
 * drives the .DSP driver itself, through parameter blocks of its own in
 * guest memory: a connection end, and a connection listener when bound
 * with a queue.  ADSP's orderly release is local, as Open Transport's was:
 * the end that closes and the other both see T_ORDREL.
 *
 * NBP names are "name:type@zone" strings (a backslash escapes ':', '@'
 * and itself); an NBPEntity is the three as Pascal strings, packed.  The
 * network has no router (zone "*"), as for the drivers.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "machine.h"
#include "ot_internal.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

enum { AF_ATALK_DDP = 0x0100, AF_ATALK_DDPNBP = 0x0101, AF_ATALK_NBP = 0x0102, AF_ATALK_MNODE = 0x0103 };
enum { DDP_MAX = 586, ATP_MAX = 578, NBP_BUF = 105, ENTITY = 99, NAME_MAX_LEN = 32, DDP_ADDR = 8 };
enum { DDP_ATP = 3, DDP_ADSP = 7 };
enum { MAX_DGRAMS = 16, MAX_REQS = 8, MAX_OUT = 4, MAX_NAMES = 8 };
/* .DSP control codes and DSPParamBlock offsets (appletalk_adsp.c's) */
enum {
    dspInit = 255, dspRemove = 254, dspOpen = 253, dspClose = 252, dspCLInit = 251, dspCLRemove = 250,
    dspCLListen = 249, dspCLDeny = 248, dspStatus = 247, dspRead = 246, dspWrite = 245, dspAttention = 244,
};
enum {
    P_REFNUM = 32, P_CCB = 34, P_SENDQSIZE = 42, P_RECVQSIZE = 48, P_ATTNPTR = 54, P_LOCALSKT = 58,
    P_LOCALCID = 34, P_REMOTECID = 36, P_REMOTEADDR = 38, P_FILTER = 42, P_OCMODE = 64, P_OCINTERVAL = 65,
    P_OCMAX = 66, P_ABORT = 34, P_SENDPEND = 38, P_SENDFREE = 40, P_RECVPEND = 42, P_REQCOUNT = 34,
    P_ACTCOUNT = 36, P_DATAPTR = 38, P_EOM = 42, P_FLUSH = 43, P_ATTNCODE = 34, P_ATTNSIZE = 36, P_ATTNDATA = 38,
    PB_SIZE = 80,
};
enum { ocRequest = 1, ocAccept = 3, sClosed = 6, CCB_STATE = 6, CCB_REMOTE = 10, CCB_SIZE = 242, ADSP_QUEUE = 8192 };
enum { errOpenDenied = -1273, errOpening = -1277 };

/* A datagram waiting for OTRcvUData. */
typedef struct {
    uint8_t node, skt, type;
    int len;
    uint8_t data[DDP_MAX];
} dgram;

/* An ATP request heard, waiting for OTRcvURequest, then for its reply. */
typedef struct {
    bool used, taken;
    uint32_t seq;
    uint8_t node, rskt;
    uint16_t tid;
    bool xo;
    uint32_t user;
    int len;
    uint8_t data[ATP_MAX];
} heard;

/* An ATP request of ours, its reply coming. */
typedef struct {
    bool used, told;
    uint32_t seq;
    atp_reply reply;
} asked;

typedef struct ot_atalk {
    bool held;             /* holds self-send */
    uint8_t skt;           /* DDP and ATP: the bound socket */
    uint8_t ddptype;       /* DDP: the type bound to (0: any) */
    gaddr nte;             /* the name bound with, registered */
    /* ddp */
    dgram q[MAX_DGRAMS];
    int nq;
    /* atp */
    heard reqs[MAX_REQS];
    asked out[MAX_OUT];
    uint32_t seq;
    bool told_request;
    /* adsp */
    int16_t ref, lref;     /* the connection end and the listener (0: none) */
    gaddr ccb, attn, pb, pb_open, pb_listen;
    bool listening, have_call, accepting, told_attn;
    provider *acceptor;    /* the listener whose OTAccept is opening this end */
    /* NBP's mapper */
    int lookup;            /* at_nbp_start's handle */
    gaddr lreq, lreply;
    uint32_t lmax, lgot;
    bool lfull;
    int verify;            /* a registration's check for the name elsewhere */
    bool dup;
    gaddr rreq, rreply;
    uint8_t rentity[ENTITY];
    int rlen;
    uint8_t rskt;
    uint8_t own_skt;       /* a socket opened for names given no address */
    gaddr names[MAX_NAMES];
} ot_atalk;

static int16_t dsp(gaddr pb, int code, bool async)
{
    wr16(pb + CntrlParam_csCode_off, (uint16_t)code);
    return adsp_control(pb, async);
}

static int16_t pb_result(gaddr pb) { return pb ? (int16_t)rd16(pb + IOParam_ioResult_off) : 0; }

/* A slice of waiting in a blocking call, the stack running. */
static bool wait_slice(provider *p)
{
    struct timespec ts = { 0, 1000000 };
    nanosleep(&ts, NULL);
    appletalk_poll();
    return ot_wait_slice(p);
}

static bool blocking(provider *p) { return p->blocking && !p->async; }

/* ---------------------------------------------------------------------- */
/* Names and addresses */

/* An address in a netbuf: DDP, NBP or both. */
typedef struct {
    bool ddp;
    uint16_t net;
    uint8_t node, skt, type;
    char name[NBP_BUF + 1];
    int namelen;
} at_addr;

static bool get_addr(gaddr buf, uint32_t len, at_addr *a)
{
    memset(a, 0, sizeof *a);
    if (!buf || len < 2)
        return false;
    uint16_t t = rd16(buf);
    uint32_t name_at = 0;
    if (t == AF_ATALK_DDP || t == AF_ATALK_MNODE || t == AF_ATALK_DDPNBP) {
        if (len < DDP_ADDR)
            return false;
        a->ddp = true;
        a->net = rd16(buf + 2);
        a->node = rd8(buf + 4);
        a->skt = rd8(buf + 5);
        a->type = rd8(buf + 6);
        name_at = t == AF_ATALK_DDPNBP ? DDP_ADDR : 0;
    } else if (t == AF_ATALK_NBP) {
        name_at = 2;
    } else {
        return false;
    }
    if (name_at && len > name_at) {
        a->namelen = (int)(len - name_at > NBP_BUF ? NBP_BUF : len - name_at);
        gget(a->name, buf + name_at, (uint32_t)a->namelen);
    }
    return true;
}

static void put_ddp(gaddr a, uint16_t net, uint8_t node, uint8_t skt, uint8_t type)
{
    wr16(a, AF_ATALK_DDP);
    wr16(a + 2, net);
    wr8(a + 4, node);
    wr8(a + 5, skt);
    wr8(a + 6, type);
    wr8(a + 7, 0);
}

/* A TNetbuf (maxlen, len, buf) given a DDP address. */
static void netbuf_ddp(gaddr nb, uint16_t net, uint8_t node, uint8_t skt, uint8_t type)
{
    if (!nb || !rd32(nb + 8) || rd32(nb) < DDP_ADDR)
        return;
    put_ddp(rd32(nb + 8), net, node, skt, type);
    wr32(nb + 4, DDP_ADDR);
}

/* "name:type@zone" to a packed entity; false if a part was cut short. */
static bool parse_nbp(const char *s, int len, uint8_t *out, int *outlen)
{
    uint8_t part[3][NAME_MAX_LEN + 1] = { { 0 } };
    bool whole = true;
    int k = 0;
    for (int i = 0; i < len && s[i]; i++) {
        char ch = s[i];
        if (ch == '\\' && i + 1 < len) {
            ch = s[++i];
        } else if (ch == ':' && k == 0) {
            k = 1;
            continue;
        } else if (ch == '@' && k < 2) {
            k = 2;
            continue;
        }
        if (part[k][0] < NAME_MAX_LEN)
            part[k][1 + part[k][0]++] = (uint8_t)ch;
        else
            whole = false;
    }
    int n = 0;
    for (int i = 0; i < 3; i++) {
        memcpy(out + n, part[i], (size_t)part[i][0] + 1);
        n += part[i][0] + 1;
    }
    *outlen = n;
    return whole;
}

/* One part of a packed entity (0 name, 1 type, 2 zone), escaped. */
static int escape_part(const uint8_t *packed, int which, char *out)
{
    const uint8_t *p = packed;
    for (int i = 0; i < which; i++)
        p += (p[0] > NAME_MAX_LEN ? NAME_MAX_LEN : p[0]) + 1;
    int n = 0, len = p[0] > NAME_MAX_LEN ? NAME_MAX_LEN : p[0];
    for (int i = 0; i < len; i++) {
        char ch = (char)p[1 + i];
        if (ch == '\\' || ch == ':' || ch == '@')
            out[n++] = '\\';
        out[n++] = ch;
    }
    out[n] = 0;
    return n;
}

/* A packed entity as "name:type@zone" (no zone part when it is empty). */
static int format_nbp(const uint8_t *packed, char *out)
{
    int n = escape_part(packed, 0, out);
    out[n++] = ':';
    n += escape_part(packed, 1, out + n);
    char zone[2 * NAME_MAX_LEN + 1];
    if (escape_part(packed, 2, zone)) {
        out[n++] = '@';
        strcpy(out + n, zone);
        n += (int)strlen(zone);
    }
    out[n] = 0;
    return n;
}

static int packed_len(const uint8_t *packed)
{
    int n = 0;
    for (int i = 0; i < 3; i++)
        n += (packed[n] > NAME_MAX_LEN ? NAME_MAX_LEN : packed[n]) + 1;
    return n;
}

static bool wildcard(const uint8_t *packed)
{
    for (int i = 0, n = 0; i < 2; i++) {
        int len = packed[n];
        for (int k = 0; k < len; k++)
            if (packed[n + 1 + k] == '=' || packed[n + 1 + k] == 0xC5)
                return true;
        n += len + 1;
    }
    return false;
}

/* The first entity answering a name, for an NBP address used to send
 * (as Open Transport resolved one for the program). */
static void first_answer(void *ctx, const uint8_t *t, int len)
{
    uint8_t *got = ctx;
    if (!got[0]) {
        got[0] = 1;
        memcpy(got + 1, t, 4);
    }
}

static bool resolve(provider *p, const at_addr *a, uint16_t *net, uint8_t *node, uint8_t *skt)
{
    if (a->ddp) {
        *net = a->net;
        *node = a->node;
        *skt = a->skt;
        return true;
    }
    uint8_t packed[ENTITY];
    int n;
    parse_nbp(a->name, a->namelen, packed, &n);
    uint8_t got[5] = { 0 };
    int h = at_nbp_start(packed, n, 3, 250, first_answer, got);
    if (!h)
        return false;
    while (!got[0] && at_nbp_busy(h) && wait_slice(p))
        ;
    at_nbp_stop(h);
    if (!got[0])
        return false;
    *net = (uint16_t)(got[1] << 8 | got[2]);
    *node = got[3];
    *skt = got[4];
    return true;
}

/* ---------------------------------------------------------------------- */
/* Providers */

bool ota_kind(const char *config, provider_kind *kind)
{
    /* the first name in the path: "ddp", "adsp(EnableEOM=1)", "atp,..." */
    char name[16] = { 0 };
    int n = 0;
    while (config[n] && config[n] != '(' && config[n] != ',' && config[n] != ':' && n < 15) {
        name[n] = config[n];
        n++;
    }
    if (!strcmp(name, "ddp"))
        *kind = P_DDP;
    else if (!strcmp(name, "atp"))
        *kind = P_ATP;
    else if (!strcmp(name, "adsp"))
        *kind = P_ADSP;
    else if (!strcmp(name, "nbp"))
        *kind = P_NBP_MAPPER;
    else
        return false;
    return true;
}

/* TEndpointInfo: addr, options, tsdu, etsdu, connect, discon, servtype, flags. */
void ota_info(gaddr info, provider_kind kind)
{
    wr32(info + 0, DDP_ADDR + NBP_BUF); /* kAppleTalkAddressLength */
    wr32(info + 4, 1024);
    wr32(info + 8, kind == P_DDP ? DDP_MAX : kind == P_ATP ? 8 * ATP_MAX : 0);
    wr32(info + 12, kind == P_ADSP ? 572 : (uint32_t)-2);
    wr32(info + 16, kind == P_ADSP ? 0 : (uint32_t)-2);
    wr32(info + 20, kind == P_ADSP ? 0 : (uint32_t)-2);
    wr32(info + 24, kind == P_DDP ? 3 : kind == P_ATP ? 5 : 2); /* T_CLTS, T_TRANS, T_COTS_ORD */
    wr32(info + 28, 0);
}

int32_t ota_open(provider *p)
{
    if (!appletalk_enabled() || at_open() != noErr)
        return kOTNotFoundErr;
    p->at = calloc(1, sizeof *p->at);
    if (!p->at)
        return kOTOutOfMemoryErr;
    if (p->kind == P_DDP || p->kind == P_ATP || p->kind == P_ADSP) {
        at_hold_selfsend(true);
        p->at->held = true;
    }
    return kOTNoError;
}

static void free_ptr(gaddr *p)
{
    if (*p)
        tb_DisposePtr(*p);
    *p = 0;
}

static void adsp_remove_end(ot_atalk *a)
{
    if (a->ref) {
        wr16(a->pb + P_REFNUM, (uint16_t)a->ref);
        dsp(a->pb, dspRemove, false);
        a->ref = 0;
    }
}

void ota_close(provider *p)
{
    ot_atalk *a = p->at;
    if (!a)
        return;
    if (a->held)
        at_hold_selfsend(false);
    if (a->nte)
        at_remove_name(a->nte);
    for (int i = 0; i < MAX_NAMES; i++)
        if (a->names[i])
            at_remove_name(a->names[i]);
    if (a->lookup)
        at_nbp_stop(a->lookup);
    if (a->verify)
        at_nbp_stop(a->verify);
    if (a->own_skt)
        at_close_socket(a->own_skt);
    if (p->kind == P_DDP && a->skt)
        at_close_socket(a->skt);
    if (p->kind == P_ATP && a->skt)
        atp_host_close(a->skt); /* our requests end with it */
    if (p->kind == P_ADSP) {
        adsp_remove_end(a);
        if (a->lref) {
            wr16(a->pb + P_REFNUM, (uint16_t)a->lref);
            dsp(a->pb, dspCLRemove, false);
        }
        /* another endpoint's end being opened for our call goes on */
        for (int i = 0; i < MAX_PROVIDERS; i++)
            if (ot_providers[i].used && ot_providers[i].at && ot_providers[i].at->acceptor == p)
                ot_providers[i].at->acceptor = NULL;
        free_ptr(&a->ccb);
        free_ptr(&a->attn);
        free_ptr(&a->pb);
        free_ptr(&a->pb_open);
        free_ptr(&a->pb_listen);
    }
    free(a);
    p->at = NULL;
}

/* ---------------------------------------------------------------------- */
/* Binding */

static provider *ddp_owner(uint8_t skt)
{
    for (int i = 0; i < MAX_PROVIDERS; i++) {
        provider *p = &ot_providers[i];
        if (p->used && p->kind == P_DDP && p->at && p->at->skt == skt)
            return p;
    }
    return NULL;
}

static void ddp_in(uint8_t skt, uint8_t snode, uint8_t sskt, uint8_t type, const uint8_t *data, int len)
{
    provider *p = ddp_owner(skt);
    if (!p || (p->at->ddptype && p->at->ddptype != type) || p->at->nq >= MAX_DGRAMS)
        return;
    dgram *d = &p->at->q[p->at->nq++];
    d->node = snode;
    d->skt = sskt;
    d->type = type;
    d->len = len > DDP_MAX ? DDP_MAX : len;
    memcpy(d->data, data, (size_t)d->len);
}

static void atp_heard(void *ctx, uint8_t skt, uint8_t node, uint8_t rskt, uint16_t tid, uint32_t user,
                      const uint8_t *data, int len)
{
    provider *p = ctx;
    if (!p->used || !p->at)
        return;
    for (int i = 0; i < MAX_REQS; i++) {
        heard *h = &p->at->reqs[i];
        if (h->used)
            continue;
        memset(h, 0, sizeof *h);
        h->used = true;
        h->node = node;
        h->rskt = rskt;
        h->tid = tid;
        h->xo = atp_host_xo(skt, node, rskt, tid);
        h->user = user;
        h->len = len > ATP_MAX ? ATP_MAX : len;
        memcpy(h->data, data, (size_t)h->len);
        return;
    }
}

static gaddr new_pb(void) { return tb_NewPtr(PB_SIZE, true, true); }

/* An ADSP connection end on a socket (0 any): 0 or the driver's error. */
static int16_t adsp_new_end(ot_atalk *a, uint8_t want)
{
    if (!a->pb)
        a->pb = new_pb();
    if (!a->ccb)
        a->ccb = tb_NewPtr(CCB_SIZE, true, true);
    if (!a->attn)
        a->attn = tb_NewPtr(572, true, true);
    if (!a->pb_open)
        a->pb_open = new_pb();
    gmemset(a->pb, 0, PB_SIZE);
    wr32(a->pb + P_CCB, a->ccb);
    wr16(a->pb + P_SENDQSIZE, ADSP_QUEUE);
    wr16(a->pb + P_RECVQSIZE, ADSP_QUEUE);
    wr32(a->pb + P_ATTNPTR, a->attn);
    wr8(a->pb + P_LOCALSKT, want);
    int16_t e = dsp(a->pb, dspInit, false);
    if (e != noErr)
        return e;
    a->ref = (int16_t)rd16(a->pb + P_REFNUM);
    a->skt = rd8(a->pb + P_LOCALSKT);
    return noErr;
}

static void adsp_listen(ot_atalk *a)
{
    gmemset(a->pb_listen, 0, PB_SIZE);
    wr16(a->pb_listen + P_REFNUM, (uint16_t)a->lref);
    a->listening = dsp(a->pb_listen, dspCLListen, true) == ioInProgress;
    a->have_call = false;
}

static void unbind(provider *p);

/* OTBind(ep, reqAddr TBind *, retAddr TBind *): a socket (the one asked
 * for, or any), and the name in the address registered on it. */
void ota_Bind(ppc_cpu *c, provider *p)
{
    ot_atalk *a = p->at;
    gaddr req = R(1), ret = R(2);
    at_addr want;
    bool have = req && get_addr(rd32(req + 8), rd32(req + 4), &want);
    int qlen = req ? (int)rd32(req + 12) : 0;
    if (p->state != T_UNBND) {
        RETURN((uint32_t)kOTOutStateErr);
        return;
    }
    uint8_t skt = have && want.ddp ? want.skt : 0;
    int32_t result = kOTNoError;
    switch (p->kind) {
    case P_DDP:
        a->skt = (uint8_t)at_open_socket(skt, ddp_in);
        a->ddptype = have ? want.type : 0;
        if (!a->skt)
            result = kOTAddressBusyErr;
        break;
    case P_ATP:
        if (skt) { /* host ATP sockets are dynamic ones */
            result = kOTBadAddressErr;
            break;
        }
        a->skt = (uint8_t)atp_host_open(atp_heard, p);
        if (!a->skt)
            result = kOTAddressBusyErr;
        break;
    case P_ADSP:
        if (qlen > 0) {
            a->pb = a->pb ? a->pb : new_pb();
            a->pb_listen = new_pb();
            gmemset(a->pb, 0, PB_SIZE);
            wr8(a->pb + P_LOCALSKT, skt);
            if (dsp(a->pb, dspCLInit, false) != noErr) {
                result = kOTAddressBusyErr;
                break;
            }
            a->lref = (int16_t)rd16(a->pb + P_REFNUM);
            a->skt = rd8(a->pb + P_LOCALSKT);
            p->qlen = 1; /* one request at a time */
            adsp_listen(a);
        } else if (adsp_new_end(a, skt) != noErr) {
            result = kOTAddressBusyErr;
        }
        break;
    default:
        result = kOTNotSupportedErr;
        break;
    }
    if (result == kOTNoError && have && want.namelen) {
        uint8_t packed[ENTITY];
        int n;
        parse_nbp(want.name, want.namelen, packed, &n);
        if (at_name_taken(packed, n))
            result = kOTAddressBusyErr;
        else
            a->nte = at_register_name(packed, n, a->skt);
    }
    if (result != kOTNoError)
        unbind(p); /* whatever was opened */
    if (result == kOTNoError) {
        p->state = T_IDLE;
        if (ret) {
            netbuf_ddp(ret, at_network(), at_node(), a->skt, p->kind == P_DDP ? a->ddptype : 0);
            wr32(ret + 12, (uint32_t)p->qlen);
        }
        LOGF(LOG_TRAPS, "OT: %s endpoint %d bound to socket %u\n",
             p->kind == P_DDP ? "ddp" : p->kind == P_ATP ? "atp" : "adsp", (int)(p - ot_providers), a->skt);
    }
    ot_complete(c, p, T_BINDCOMPLETE, result, ret);
}

/* What binding took: the name, the socket, the ADSP ends. */
static void unbind(provider *p)
{
    ot_atalk *a = p->at;
    if (a->nte) {
        at_remove_name(a->nte);
        a->nte = 0;
    }
    if (p->kind == P_DDP && a->skt)
        at_close_socket(a->skt);
    if (p->kind == P_ATP && a->skt)
        atp_host_close(a->skt);
    if (p->kind == P_ADSP) {
        adsp_remove_end(a);
        if (a->lref) {
            wr16(a->pb + P_REFNUM, (uint16_t)a->lref);
            dsp(a->pb, dspCLRemove, false);
            a->lref = 0;
        }
        a->listening = a->have_call = false;
    }
    a->skt = 0;
    a->nq = 0;
    memset(a->reqs, 0, sizeof a->reqs);
    memset(a->out, 0, sizeof a->out);
    p->qlen = 0;
    p->state = T_UNBND;
}

void ota_Unbind(ppc_cpu *c, provider *p)
{
    unbind(p);
    ot_complete(c, p, T_UNBINDCOMPLETE, kOTNoError, 0);
}

/* OTGetProtAddress(ep, boundAddr TBind *, peerAddr TBind *) */
void ota_GetProtAddress(ppc_cpu *c, provider *p)
{
    ot_atalk *a = p->at;
    if (R(1)) {
        wr32(R(1) + 4, 0);
        if (a->skt)
            netbuf_ddp(R(1), at_network(), at_node(), a->skt, 0);
    }
    if (R(2)) {
        wr32(R(2) + 4, 0);
        if (p->kind == P_ADSP && a->ccb && (p->state == T_DATAXFER || p->state == T_INREL || p->state == T_OUTREL))
            netbuf_ddp(R(2), rd16(a->ccb + CCB_REMOTE), rd8(a->ccb + CCB_REMOTE + 2), rd8(a->ccb + CCB_REMOTE + 3), 0);
    }
    ot_complete(c, p, T_GETPROTADDRCOMPLETE, kOTNoError, R(2));
}

/* OTResolveAddress(ep, reqAddr TBind *, retAddr TBind *, timeout): an NBP
 * name's DDP address. */
void ota_ResolveAddress(ppc_cpu *c, provider *p)
{
    gaddr req = R(1), ret = R(2);
    at_addr want;
    uint16_t net;
    uint8_t node, skt;
    if (!req || !get_addr(rd32(req + 8), rd32(req + 4), &want)) {
        RETURN((uint32_t)kOTBadAddressErr);
        return;
    }
    int32_t r = resolve(p, &want, &net, &node, &skt) ? kOTNoError : kOTNotFoundErr;
    if (r == kOTNoError && ret)
        netbuf_ddp(ret, net, node, skt, want.type);
    ot_complete(c, p, T_RESOLVEADDRCOMPLETE, r, ret);
}

/* ---------------------------------------------------------------------- */
/* DDP: TUnitData is addr, opt, udata (TNetbufs) */

void ota_SndUData(ppc_cpu *c, provider *p)
{
    ot_atalk *a = p->at;
    gaddr ud = R(1);
    at_addr to;
    uint16_t net;
    uint8_t node, skt;
    if (p->kind != P_DDP) {
        RETURN((uint32_t)kOTNotSupportedErr);
        return;
    }
    if (p->state != T_IDLE) {
        RETURN((uint32_t)kOTOutStateErr);
        return;
    }
    if (!ud || !get_addr(rd32(ud + 8), rd32(ud + 4), &to) || !resolve(p, &to, &net, &node, &skt)) {
        RETURN((uint32_t)kOTBadAddressErr);
        return;
    }
    uint32_t n = ot_data_len(rd32(ud + 32), rd32(ud + 28));
    if (n > DDP_MAX) {
        RETURN((uint32_t)kOTBadDataErr);
        return;
    }
    uint8_t data[DDP_MAX];
    ot_data_get(data, rd32(ud + 32), rd32(ud + 28), n);
    uint8_t type = to.type ? to.type : a->ddptype;
    at_send_ddp(node ? node : at_node(), skt, a->skt, type, data, (int)n);
    appletalk_poll();
    RETURN(kOTNoError);
}

void ota_RcvUData(ppc_cpu *c, provider *p)
{
    ot_atalk *a = p->at;
    gaddr ud = R(1);
    if (R(2))
        wr32(R(2), 0);
    if (p->kind != P_DDP) {
        RETURN((uint32_t)kOTNotSupportedErr);
        return;
    }
    while (!a->nq) {
        if (!blocking(p) || !wait_slice(p)) {
            p->told_data = false;
            RETURN((uint32_t)kOTNoDataErr);
            return;
        }
    }
    dgram d = a->q[0];
    memmove(a->q, a->q + 1, sizeof a->q[0] * (size_t)--a->nq);
    if (ud) {
        uint32_t room = rd32(ud + 24), n = (uint32_t)d.len < room ? (uint32_t)d.len : room;
        gput(rd32(ud + 32), d.data, n);
        wr32(ud + 28, n);
        if (n < (uint32_t)d.len && R(2))
            wr32(R(2), T_MORE); /* the rest is dropped */
        wr32(ud + 4, 0);
        netbuf_ddp(ud, at_network(), d.node, d.skt, d.type);
        wr32(ud + 16, 0); /* no options */
    }
    if (!a->nq)
        p->told_data = false;
    RETURN(kOTNoError);
}

/* ---------------------------------------------------------------------- */
/* ATP: TUnitRequest is addr, opt, udata, sequence; TUnitReply opt, udata,
 * sequence.  The first four bytes of a request's data, and of a reply's,
 * are the ATP user bytes (of its first response). */

/* OTSndURequest(ep, req TUnitRequest *, reqFlags) */
static void ota_SndURequest(ppc_cpu *c)
{
    provider *p = ot_provider_of(R(0));
    if (!p || p->kind != P_ATP) {
        RETURN((uint32_t)(p ? kOTNotSupportedErr : kOTBadReferenceErr));
        return;
    }
    ot_atalk *a = p->at;
    gaddr req = R(1);
    at_addr to;
    uint16_t net;
    uint8_t node, skt;
    if (p->state != T_IDLE) {
        RETURN((uint32_t)kOTOutStateErr);
        return;
    }
    if (!req || !get_addr(rd32(req + 8), rd32(req + 4), &to) || !resolve(p, &to, &net, &node, &skt)) {
        RETURN((uint32_t)kOTBadAddressErr);
        return;
    }
    uint32_t n = ot_data_len(rd32(req + 32), rd32(req + 28));
    if (n > 4 + ATP_MAX) {
        RETURN((uint32_t)kOTBadDataErr);
        return;
    }
    uint8_t data[4 + ATP_MAX] = { 0 };
    ot_data_get(data, rd32(req + 32), rd32(req + 28), n);
    asked *q = NULL;
    for (int i = 0; i < MAX_OUT && !q; i++)
        if (!a->out[i].used)
            q = &a->out[i];
    if (!q) {
        RETURN((uint32_t)kOTFlowErr);
        return;
    }
    uint32_t user = (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 | (uint32_t)data[2] << 8 | data[3];
    /* OPT_RETRYCNT 8 and OPT_INTERVAL 2 seconds, Open Transport's defaults */
    int16_t e = atp_host_start(a->skt, node ? node : at_node(), skt, user, data + 4, n > 4 ? (int)n - 4 : 0,
                               (R(2) & T_ACKNOWLEDGED) != 0, 8, 2000, 8, &q->reply);
    if (e != noErr) {
        RETURN((uint32_t)kOTFlowErr);
        return;
    }
    q->used = true;
    q->told = false;
    q->seq = ++a->seq;
    wr32(req + 36, q->seq);
    appletalk_poll();
    RETURN(kOTNoError);
}

static asked *answered(ot_atalk *a)
{
    for (int i = 0; i < MAX_OUT; i++)
        if (a->out[i].used && a->out[i].reply.result != ioInProgress)
            return &a->out[i];
    return NULL;
}

static bool waiting_request(ot_atalk *a)
{
    for (int i = 0; i < MAX_REQS; i++)
        if (a->reqs[i].used && !a->reqs[i].taken)
            return true;
    return false;
}

static bool asking(ot_atalk *a)
{
    for (int i = 0; i < MAX_OUT; i++)
        if (a->out[i].used)
            return true;
    return false;
}

/* OTRcvUReply(ep, reply TUnitReply *, &replyFlags) */
static void ota_RcvUReply(ppc_cpu *c)
{
    provider *p = ot_provider_of(R(0));
    if (!p || p->kind != P_ATP) {
        RETURN((uint32_t)(p ? kOTNotSupportedErr : kOTBadReferenceErr));
        return;
    }
    ot_atalk *a = p->at;
    gaddr reply = R(1);
    if (R(2))
        wr32(R(2), 0);
    asked *q;
    while (!(q = answered(a))) {
        if (!asking(a) || !blocking(p) || !wait_slice(p)) {
            RETURN((uint32_t)kOTNoDataErr);
            return;
        }
    }
    q->used = false;
    if (reply)
        wr32(reply + 24, q->seq);
    if (q->reply.result != noErr) {
        if (reply)
            wr32(reply + 16, 0);
        RETURN((uint32_t)kETIMEDOUTErr);
        return;
    }
    if (reply) {
        uint8_t data[4 + 8 * ATP_MAX];
        uint32_t u = q->reply.user[0];
        data[0] = (uint8_t)(u >> 24);
        data[1] = (uint8_t)(u >> 16);
        data[2] = (uint8_t)(u >> 8);
        data[3] = (uint8_t)u;
        int n = 4;
        for (int i = 0; i < q->reply.count; i++) {
            memcpy(data + n, q->reply.data[i], (size_t)q->reply.len[i]);
            n += q->reply.len[i];
        }
        uint32_t room = rd32(reply + 12), k = (uint32_t)n < room ? (uint32_t)n : room;
        gput(rd32(reply + 20), data, k);
        wr32(reply + 16, k);
        wr32(reply + 4, 0);
    }
    RETURN(kOTNoError);
}

/* OTRcvURequest(ep, req TUnitRequest *, &reqFlags) */
static void ota_RcvURequest(ppc_cpu *c)
{
    provider *p = ot_provider_of(R(0));
    if (!p || p->kind != P_ATP) {
        RETURN((uint32_t)(p ? kOTNotSupportedErr : kOTBadReferenceErr));
        return;
    }
    ot_atalk *a = p->at;
    gaddr req = R(1);
    if (R(2))
        wr32(R(2), 0);
    heard *h;
    for (;;) {
        h = NULL;
        for (int i = 0; i < MAX_REQS && !h; i++)
            if (a->reqs[i].used && !a->reqs[i].taken)
                h = &a->reqs[i];
        if (h)
            break;
        if (!blocking(p) || !wait_slice(p)) {
            a->told_request = false;
            RETURN((uint32_t)kOTNoDataErr);
            return;
        }
    }
    h->taken = true;
    h->seq = ++a->seq;
    if (req) {
        uint8_t data[4 + ATP_MAX];
        data[0] = (uint8_t)(h->user >> 24);
        data[1] = (uint8_t)(h->user >> 16);
        data[2] = (uint8_t)(h->user >> 8);
        data[3] = (uint8_t)h->user;
        memcpy(data + 4, h->data, (size_t)h->len);
        uint32_t n = 4 + (uint32_t)h->len, room = rd32(req + 24), k = n < room ? n : room;
        gput(rd32(req + 32), data, k);
        wr32(req + 28, k);
        wr32(req + 4, 0);
        netbuf_ddp(req, at_network(), h->node, h->rskt, DDP_ATP);
        wr32(req + 16, 0);
        wr32(req + 36, h->seq);
    }
    if (R(2) && h->xo)
        wr32(R(2), T_ACKNOWLEDGED);
    RETURN(kOTNoError);
}

/* OTSndUReply(ep, reply TUnitReply *, replyFlags): up to eight responses. */
static void ota_SndUReply(ppc_cpu *c)
{
    provider *p = ot_provider_of(R(0));
    if (!p || p->kind != P_ATP) {
        RETURN((uint32_t)(p ? kOTNotSupportedErr : kOTBadReferenceErr));
        return;
    }
    ot_atalk *a = p->at;
    gaddr reply = R(1);
    heard *h = NULL;
    for (int i = 0; i < MAX_REQS && !h && reply; i++)
        if (a->reqs[i].used && a->reqs[i].taken && a->reqs[i].seq == rd32(reply + 24))
            h = &a->reqs[i];
    if (!h) {
        RETURN((uint32_t)kOTBadSequenceErr);
        return;
    }
    uint32_t n = ot_data_len(rd32(reply + 20), rd32(reply + 16));
    if (n > 4 + 8 * ATP_MAX) {
        RETURN((uint32_t)kOTBadDataErr);
        return;
    }
    static uint8_t data[4 + 8 * ATP_MAX];
    memset(data, 0, 4);
    ot_data_get(data, rd32(reply + 20), rd32(reply + 16), n);
    uint8_t (*pkts)[578] = calloc(8, sizeof *pkts);
    int lens[8] = { 0 }, count = 0;
    uint32_t users[8] = { (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 | (uint32_t)data[2] << 8 | data[3] };
    for (uint32_t off = 4; count < 8 && (off < n || count == 0); off += ATP_MAX) {
        int k = n > off ? (int)(n - off > ATP_MAX ? ATP_MAX : n - off) : 0;
        memcpy(pkts[count], data + off, (size_t)k);
        lens[count++] = k;
    }
    atp_host_respond(a->skt, h->node, h->rskt, h->tid, count, (const uint8_t(*)[578])pkts, lens, users);
    free(pkts);
    h->used = false;
    appletalk_poll();
    RETURN(kOTNoError);
}

/* OTCancelURequest(ep, seq) and OTCancelUReply(ep, seq) */
static void ota_CancelURequest(ppc_cpu *c)
{
    provider *p = ot_provider_of(R(0));
    if (!p || p->kind != P_ATP) {
        RETURN((uint32_t)kOTBadReferenceErr);
        return;
    }
    for (int i = 0; i < MAX_OUT; i++)
        if (p->at->out[i].used && p->at->out[i].seq == R(1)) {
            atp_host_cancel(&p->at->out[i].reply);
            p->at->out[i].used = false;
            RETURN(kOTNoError);
            return;
        }
    RETURN((uint32_t)kOTBadSequenceErr);
}

static void ota_CancelUReply(ppc_cpu *c)
{
    provider *p = ot_provider_of(R(0));
    if (!p || p->kind != P_ATP) {
        RETURN((uint32_t)kOTBadReferenceErr);
        return;
    }
    for (int i = 0; i < MAX_REQS; i++)
        if (p->at->reqs[i].used && p->at->reqs[i].taken && p->at->reqs[i].seq == R(1)) {
            p->at->reqs[i].used = false;
            RETURN(kOTNoError);
            return;
        }
    RETURN((uint32_t)kOTBadSequenceErr);
}

/* ---------------------------------------------------------------------- */
/* ADSP: the .DSP driver's calls on parameter blocks of our own */

static bool adsp_open(provider *p) { return p->state == T_DATAXFER || p->state == T_INREL || p->state == T_OUTREL; }

static int adsp_status(ot_atalk *a, int field)
{
    if (!a->ref)
        return 0;
    wr16(a->pb + P_REFNUM, (uint16_t)a->ref);
    if (dsp(a->pb, dspStatus, false) != noErr)
        return 0;
    return (int16_t)rd16(a->pb + field);
}

static bool adsp_closed(ot_atalk *a) { return a->ccb && rd16(a->ccb + CCB_STATE) == sClosed; }

static int32_t open_error(int16_t e)
{
    return e == errOpenDenied ? kECONNREFUSEDErr : e == errOpening ? kETIMEDOUTErr : kECONNRESETErr;
}

/* OTConnect(ep, sndCall TCall *, rcvCall TCall *): TCall is addr, opt,
 * udata, sequence. */
void ota_Connect(ppc_cpu *c, provider *p)
{
    ot_atalk *a = p->at;
    gaddr snd = R(1), rcv = R(2);
    at_addr to;
    uint16_t net;
    uint8_t node, skt;
    if (p->kind != P_ADSP) {
        RETURN((uint32_t)kOTNotSupportedErr);
        return;
    }
    if (p->state != T_IDLE || !a->ref) {
        RETURN((uint32_t)kOTOutStateErr);
        return;
    }
    if (!snd || !get_addr(rd32(snd + 8), rd32(snd + 4), &to) || !resolve(p, &to, &net, &node, &skt)) {
        RETURN((uint32_t)kOTBadAddressErr);
        return;
    }
    gaddr pb = a->pb_open;
    gmemset(pb, 0, PB_SIZE);
    wr16(pb + P_REFNUM, (uint16_t)a->ref);
    wr16(pb + P_REMOTEADDR, net);
    wr8(pb + P_REMOTEADDR + 2, node ? node : at_node());
    wr8(pb + P_REMOTEADDR + 3, skt);
    wr8(pb + P_OCMODE, ocRequest);
    wr8(pb + P_OCMAX, 3);
    p->connected = p->told_connect = p->disconnected = p->told_disc = false;
    p->eof = p->told_ordrel = p->told_data = false;
    int16_t e = dsp(pb, dspOpen, true);
    if (e != ioInProgress && e != noErr) {
        RETURN((uint32_t)open_error(e));
        return;
    }
    p->state = T_OUTCON;
    LOGF(LOG_TRAPS, "OT: adsp %d connects to node %u socket %u\n", (int)(p - ot_providers), node, skt);
    if (p->async || !p->blocking) {
        RETURN((uint32_t)kOTNoDataErr); /* T_CONNECT comes */
        return;
    }
    while (pb_result(pb) == ioInProgress && wait_slice(p))
        ;
    e = pb_result(pb);
    if (e != noErr) {
        p->state = T_IDLE;
        RETURN((uint32_t)open_error(e));
        return;
    }
    p->state = T_DATAXFER;
    p->connected = p->told_connect = true;
    if (rcv)
        netbuf_ddp(rcv, net, node, skt, 0);
    RETURN(kOTNoError);
}

/* OTRcvConnect(ep, call TCall *) */
void ota_RcvConnect(ppc_cpu *c, provider *p)
{
    ot_atalk *a = p->at;
    if (p->kind != P_ADSP || p->state != T_OUTCON) {
        RETURN((uint32_t)kOTOutStateErr);
        return;
    }
    while (pb_result(a->pb_open) == ioInProgress && blocking(p) && wait_slice(p))
        ;
    int16_t e = pb_result(a->pb_open);
    if (e == ioInProgress) {
        RETURN((uint32_t)kOTNoDataErr);
        return;
    }
    if (e != noErr) {
        p->disconnected = true;
        p->disc_reason = open_error(e);
        RETURN((uint32_t)kOTLookErr);
        return;
    }
    p->state = T_DATAXFER;
    p->connected = p->told_connect = true;
    if (R(1))
        netbuf_ddp(R(1), rd16(a->pb_open + P_REMOTEADDR), rd8(a->pb_open + P_REMOTEADDR + 2),
                   rd8(a->pb_open + P_REMOTEADDR + 3), 0);
    RETURN(kOTNoError);
}

/* OTListen(ep, call TCall *): the open request the listener heard. */
void ota_Listen(ppc_cpu *c, provider *p)
{
    ot_atalk *a = p->at;
    if (p->kind != P_ADSP || !a->lref) {
        RETURN((uint32_t)kOTNotSupportedErr);
        return;
    }
    while (!a->have_call) {
        if (a->listening && pb_result(a->pb_listen) == noErr) {
            a->listening = false;
            a->have_call = true;
            break;
        }
        if (!blocking(p) || !wait_slice(p)) {
            p->told_listen = false;
            RETURN((uint32_t)kOTNoDataErr);
            return;
        }
    }
    gaddr call = R(1);
    if (call) {
        netbuf_ddp(call, rd16(a->pb_listen + P_REMOTEADDR), rd8(a->pb_listen + P_REMOTEADDR + 2),
                   rd8(a->pb_listen + P_REMOTEADDR + 3), 0);
        wr32(call + 16, 0);
        wr32(call + 28, 0);
        wr32(call + 36, 1); /* sequence */
    }
    p->state = T_INCON;
    RETURN(kOTNoError);
}

/* OTAccept(listener, worker, call): the worker's end (made if need be)
 * answers the request, from the listener's socket's call. */
void ota_Accept(ppc_cpu *c, provider *p, provider *w)
{
    ot_atalk *a = p->at;
    if (!w || !w->at || w->kind != P_ADSP) {
        RETURN((uint32_t)kOTBadReferenceErr);
        return;
    }
    if (p->kind != P_ADSP || !a->have_call) {
        RETURN((uint32_t)kOTNoDataErr);
        return;
    }
    ot_atalk *b = w->at;
    if (!b->ref && adsp_new_end(b, 0) != noErr) {
        RETURN((uint32_t)kOTOutOfMemoryErr);
        return;
    }
    if (w != p && w->state == T_UNBND)
        w->state = T_IDLE;
    gaddr pb = b->pb_open;
    gmemset(pb, 0, PB_SIZE);
    gmemcpy(pb + 34, a->pb_listen + 34, 30); /* the remote's connection ID, address and sequences */
    wr16(pb + P_REFNUM, (uint16_t)b->ref);
    wr8(pb + P_OCMODE, ocAccept);
    wr8(pb + P_OCMAX, 3);
    w->connected = w->told_connect = w->disconnected = w->told_disc = false;
    w->eof = w->told_ordrel = w->told_data = false;
    a->have_call = false;
    p->told_listen = false;
    p->state = T_IDLE;
    int16_t e = dsp(pb, dspOpen, true);
    if (w != p)
        adsp_listen(a); /* for the next */
    if (e != ioInProgress && e != noErr) {
        RETURN((uint32_t)open_error(e));
        return;
    }
    if (p->async) {
        b->accepting = true;
        b->acceptor = p;
        w->state = T_INCON;
        RETURN(kOTNoError); /* T_ACCEPTCOMPLETE comes */
        return;
    }
    while (pb_result(pb) == ioInProgress && wait_slice(w))
        ;
    e = pb_result(pb);
    if (e != noErr) {
        w->state = T_IDLE;
        RETURN((uint32_t)open_error(e));
        return;
    }
    w->state = T_DATAXFER;
    if (w != p)
        ot_post(w, T_PASSCON, kOTNoError, 0);
    RETURN(kOTNoError);
}

/* OTSnd(ep, buf, nbytes, flags): bytes sent */
void ota_Snd(ppc_cpu *c, provider *p)
{
    ot_atalk *a = p->at;
    gaddr buf = R(1);
    uint32_t n = R(2), done = 0;
    if (p->kind != P_ADSP) {
        RETURN((uint32_t)kOTNotSupportedErr);
        return;
    }
    if (p->state != T_DATAXFER && p->state != T_INREL) {
        RETURN((uint32_t)(p->disconnected || p->eof ? kOTLookErr : kOTOutStateErr));
        return;
    }
    if (n == kNetbufDataIsOTData) { /* the chain in one guest buffer, sent from there */
        n = ot_data_len(buf, n);
        gaddr flat = tb_NewPtr((int32_t)(n ? n : 1), false, true);
        if (!flat) {
            RETURN((uint32_t)kOTOutOfMemoryErr);
            return;
        }
        uint8_t *tmp = malloc(n ? n : 1);
        ot_data_get(tmp, buf, kNetbufDataIsOTData, n);
        gput(flat, tmp, n);
        free(tmp);
        R(1) = flat;
        R(2) = n;
        ota_Snd(c, p);
        tb_DisposePtr(flat);
        return;
    }
    if (R(3) & T_EXPEDITED) { /* an attention message: its code, then data */
        if (n < 2 || n > 572) {
            RETURN((uint32_t)kOTBadDataErr);
            return;
        }
        gaddr pb = a->pb;
        wr16(pb + P_REFNUM, (uint16_t)a->ref);
        wr16(pb + P_ATTNCODE, rd16(buf));
        wr16(pb + P_ATTNSIZE, (uint16_t)(n - 2));
        wr32(pb + P_ATTNDATA, buf + 2);
        RETURN(dsp(pb, dspAttention, false) == noErr ? n : (uint32_t)kOTLookErr);
        return;
    }
    while (done < n) {
        int room = adsp_status(a, P_SENDFREE);
        if (adsp_closed(a)) {
            RETURN(done ? done : (uint32_t)kOTLookErr);
            return;
        }
        if (room <= 0) {
            if (blocking(p) && wait_slice(p))
                continue;
            if (!done) {
                p->flow = true;
                RETURN((uint32_t)kOTFlowErr);
                return;
            }
            break;
        }
        uint32_t k = n - done < (uint32_t)room ? n - done : (uint32_t)room;
        gaddr pb = a->pb;
        wr16(pb + P_REFNUM, (uint16_t)a->ref);
        wr16(pb + P_REQCOUNT, (uint16_t)k);
        wr32(pb + P_DATAPTR, buf + done);
        wr8(pb + P_EOM, 0);
        wr8(pb + P_FLUSH, 1);
        if (dsp(pb, dspWrite, false) != noErr)
            break;
        done += rd16(pb + P_ACTCOUNT);
    }
    appletalk_poll();
    RETURN(done);
}

/* OTRcv(ep, buf, nbytes, &flags): bytes received */
void ota_Rcv(ppc_cpu *c, provider *p)
{
    ot_atalk *a = p->at;
    gaddr buf = R(1), flags = R(3);
    uint32_t n = R(2);
    if (flags)
        wr32(flags, 0);
    if (p->kind != P_ADSP) {
        RETURN((uint32_t)kOTNotSupportedErr);
        return;
    }
    if (!adsp_open(p)) {
        RETURN((uint32_t)(p->disconnected ? kOTLookErr : kOTOutStateErr));
        return;
    }
    int have;
    for (;;) {
        have = adsp_status(a, P_RECVPEND);
        if (have > 0)
            break;
        if (p->disconnected || adsp_closed(a)) {
            RETURN((uint32_t)kOTLookErr); /* T_ORDREL (or T_DISCONNECT) */
            return;
        }
        if (!blocking(p) || !wait_slice(p)) {
            p->told_data = false;
            RETURN((uint32_t)kOTNoDataErr);
            return;
        }
    }
    gaddr pb = a->pb;
    wr16(pb + P_REFNUM, (uint16_t)a->ref);
    /* what is there, so the driver's read finishes at once */
    uint32_t want = n < (uint32_t)have ? n : (uint32_t)have;
    wr16(pb + P_REQCOUNT, (uint16_t)(want > 0x7FFF ? 0x7FFF : want));
    wr32(pb + P_DATAPTR, buf);
    if (dsp(pb, dspRead, false) != noErr) {
        RETURN((uint32_t)kOTLookErr);
        return;
    }
    uint32_t got = rd16(pb + P_ACTCOUNT);
    if (flags && !rd8(pb + P_EOM))
        wr32(flags, T_MORE); /* a stream: more may come */
    RETURN(got);
}

/* OTLook(ep): the event waiting */
void ota_Look(ppc_cpu *c, provider *p)
{
    ot_atalk *a = p->at;
    ota_poll(p);
    if (p->kind == P_DDP)
        RETURN(a->nq ? T_DATA : 0);
    else if (p->kind == P_ATP)
        RETURN(answered(a) ? T_REPLY : waiting_request(a) ? T_REQUEST : 0);
    else if (p->kind != P_ADSP)
        RETURN(0);
    else if (p->disconnected)
        RETURN(T_DISCONNECT);
    else if (p->state == T_OUTCON && pb_result(a->pb_open) == noErr)
        RETURN(T_CONNECT);
    else if (a->have_call || (a->listening && pb_result(a->pb_listen) == noErr))
        RETURN(T_LISTEN);
    else if (adsp_open(p) && adsp_status(a, P_RECVPEND) > 0)
        RETURN(T_DATA);
    else if (p->eof)
        RETURN(T_ORDREL);
    else
        RETURN(0);
}

void ota_CountDataBytes(ppc_cpu *c, provider *p)
{
    ot_atalk *a = p->at;
    int n = 0;
    if (p->kind == P_ADSP)
        n = adsp_open(p) ? adsp_status(a, P_RECVPEND) : 0;
    else if (p->kind == P_DDP)
        n = a->nq ? a->q[0].len : 0;
    if (R(1))
        wr32(R(1), (uint32_t)n);
    RETURN(n > 0 ? kOTNoError : kOTNoDataErr);
}

static void adsp_close(ot_atalk *a, bool abort)
{
    if (!a->ref)
        return;
    wr16(a->pb + P_REFNUM, (uint16_t)a->ref);
    wr8(a->pb + P_ABORT, abort);
    dsp(a->pb, dspClose, false);
}

static void adsp_reset_flags(provider *p)
{
    p->connected = p->told_connect = p->disconnected = p->told_disc = false;
    p->eof = p->told_ordrel = p->told_data = p->flow = false;
}

/* ADSP's orderly release is local: the connection closes, its data sent,
 * and T_ORDREL comes to this end as to the other. */
void ota_SndOrderlyDisconnect(ppc_cpu *c, provider *p)
{
    if (p->kind != P_ADSP || (p->state != T_DATAXFER && p->state != T_INREL)) {
        RETURN((uint32_t)(p->kind == P_ADSP ? kOTOutStateErr : kOTNotSupportedErr));
        return;
    }
    adsp_close(p->at, false);
    if (p->state == T_INREL) {
        p->state = T_IDLE;
        adsp_reset_flags(p);
    } else {
        p->state = T_OUTREL;
    }
    RETURN(kOTNoError);
}

void ota_RcvOrderlyDisconnect(ppc_cpu *c, provider *p)
{
    if (p->kind != P_ADSP || !p->eof) {
        RETURN((uint32_t)(p->kind == P_ADSP ? kOTNoReleaseErr : kOTNotSupportedErr));
        return;
    }
    if (p->state == T_OUTREL) {
        p->state = T_IDLE;
        adsp_reset_flags(p);
    } else {
        p->state = T_INREL;
    }
    RETURN(kOTNoError);
}

/* OTSndDisconnect(ep, call): abort the connection, or refuse a request. */
void ota_SndDisconnect(ppc_cpu *c, provider *p)
{
    ot_atalk *a = p->at;
    if (p->kind != P_ADSP) {
        RETURN((uint32_t)kOTNotSupportedErr);
        return;
    }
    if (a->have_call) {
        gaddr pb = a->pb;
        gmemset(pb, 0, PB_SIZE);
        gmemcpy(pb + 34, a->pb_listen + 34, 30);
        wr16(pb + P_REFNUM, (uint16_t)a->lref);
        dsp(pb, dspCLDeny, false);
        p->state = T_IDLE;
        p->told_listen = false;
        adsp_listen(a);
    } else {
        adsp_close(a, true);
        p->state = T_IDLE;
        adsp_reset_flags(p);
    }
    ot_complete(c, p, T_DISCONNECTCOMPLETE, kOTNoError, R(1));
}

/* OTRcvDisconnect(ep, discon TDiscon *): udata, reason, sequence. */
void ota_RcvDisconnect(ppc_cpu *c, provider *p)
{
    if (p->kind != P_ADSP || !p->disconnected) {
        RETURN((uint32_t)kOTNoDisconnectErr);
        return;
    }
    if (R(1)) {
        wr32(R(1) + 4, 0);
        wr32(R(1) + 12, (uint32_t)p->disc_reason);
    }
    adsp_close(p->at, true);
    p->state = T_IDLE;
    adsp_reset_flags(p);
    RETURN(kOTNoError);
}

/* ---------------------------------------------------------------------- */
/* NBP's mapper: TRegisterRequest is name, addr, flags; TRegisterReply
 * addr, nameid; TLookupRequest name, addr, maxcnt, timeout, flags;
 * TLookupReply names, rspcount.  Names found go in the reply's buffer as
 * TLookupBuffers (address length, name length, the address, the name),
 * each from a four-byte boundary. */

static void lookup_answer(void *ctx, const uint8_t *t, int len)
{
    provider *p = ctx;
    ot_atalk *a = p->used ? p->at : NULL;
    if (!a || !a->lreply || a->lfull)
        return;
    char name[2 * ENTITY + 4];
    int nl = format_nbp(t + 5, name);
    gaddr nb = a->lreply, buf = rd32(nb + 8);
    uint32_t used = rd32(nb + 4), max = rd32(nb);
    for (uint32_t off = 0; off + 4 <= used;) { /* already there? */
        uint32_t al = rd16(buf + off), l = rd16(buf + off + 2);
        if (al == DDP_ADDR && l == (uint32_t)nl && rd16(buf + off + 4 + 2) == (uint16_t)(t[0] << 8 | t[1]) &&
            rd8(buf + off + 4 + 4) == t[2] && rd8(buf + off + 4 + 5) == t[3]) {
            char had[2 * ENTITY + 4];
            gget(had, buf + off + 4 + DDP_ADDR, l);
            if (!memcmp(had, name, l))
                return;
        }
        off += (4 + al + l + 3) & ~3u;
    }
    uint32_t size = (4 + DDP_ADDR + (uint32_t)nl + 3) & ~3u;
    if (!buf || used + 4 + DDP_ADDR + (uint32_t)nl > max) {
        a->lfull = true; /* kOTBufferOverflowErr */
        return;
    }
    wr16(buf + used, DDP_ADDR);
    wr16(buf + used + 2, (uint16_t)nl);
    put_ddp(buf + used + 4, (uint16_t)(t[0] << 8 | t[1]), t[2], t[3], 0);
    gput(buf + used + 4 + DDP_ADDR, name, (uint32_t)nl);
    wr32(nb + 4, used + size > max ? max : used + size);
    wr32(a->lreply + 12, ++a->lgot);
    if (p->async)
        ot_post(p, T_LKUPNAMERESULT, kOTNoError, a->lreply);
    if (a->lmax && a->lgot >= a->lmax)
        a->lfull = true;
    (void)len;
}

static bool lookup_running(ot_atalk *a) { return a->lookup && at_nbp_busy(a->lookup) && !a->lfull; }

static int32_t lookup_done(provider *p)
{
    ot_atalk *a = p->at;
    at_nbp_stop(a->lookup);
    a->lookup = 0;
    bool overflow = a->lfull && !(a->lmax && a->lgot >= a->lmax);
    gaddr reply = a->lreply;
    a->lreply = 0;
    LOGF(LOG_TRAPS, "OT: NBP lookup found %u\n", a->lgot);
    int32_t r = overflow ? kOTBufferOverflowErr : kOTNoError;
    if (p->async)
        ot_post(p, T_LKUPNAMECOMPLETE, r, reply);
    return r;
}

/* OTLookupName(ref, req, reply) */
static void ota_LookupName(ppc_cpu *c)
{
    provider *p = ot_provider_of(R(0));
    if (!p || p->kind != P_NBP_MAPPER) {
        RETURN((uint32_t)kOTBadReferenceErr);
        return;
    }
    ot_atalk *a = p->at;
    gaddr req = R(1), reply = R(2);
    if (!req || !reply || a->lookup) {
        RETURN((uint32_t)(a->lookup ? kOTOutStateErr : kOTBadNameErr));
        return;
    }
    char s[NBP_BUF + 1];
    uint32_t n = rd32(req + 4) > NBP_BUF ? NBP_BUF : rd32(req + 4);
    gget(s, rd32(req + 8), n);
    uint8_t packed[ENTITY];
    int len;
    parse_nbp(s, (int)n, packed, &len);
    uint32_t timeout = rd32(req + 28);
    int tries = timeout ? (int)(timeout / 250) : 4;
    a->lreply = reply;
    a->lmax = rd32(req + 24);
    a->lgot = 0;
    a->lfull = false;
    wr32(reply + 4, 0);
    wr32(reply + 12, 0);
    a->lookup = at_nbp_start(packed, len, tries > 0 ? tries : 1, 250, lookup_answer, p);
    if (!a->lookup) {
        a->lreply = 0;
        RETURN((uint32_t)kOTOutOfMemoryErr);
        return;
    }
    if (p->async) {
        RETURN(kOTNoError); /* T_LKUPNAMERESULTs, then T_LKUPNAMECOMPLETE */
        return;
    }
    while (lookup_running(a) && wait_slice(p))
        ;
    RETURN((uint32_t)lookup_done(p));
}

static void verify_answer(void *ctx, const uint8_t *t, int len)
{
    provider *p = ctx;
    if (p->used && p->at && t[2] != at_node())
        p->at->dup = true;
    (void)len;
}

static bool verify_running(ot_atalk *a) { return a->verify && at_nbp_busy(a->verify) && !a->dup; }

static int32_t register_done(provider *p)
{
    ot_atalk *a = p->at;
    at_nbp_stop(a->verify);
    a->verify = 0;
    gaddr reply = a->rreply;
    a->rreply = 0;
    int32_t r = kOTNoError;
    int slot = -1;
    for (int i = 0; i < MAX_NAMES && slot < 0; i++)
        if (!a->names[i])
            slot = i;
    if (a->dup)
        r = kOTDuplicateFoundErr;
    else if (slot < 0 || !(a->names[slot] = at_register_name(a->rentity, a->rlen, a->rskt)))
        r = kOTOutOfMemoryErr;
    if (r == kOTNoError && reply) {
        wr32(reply + 4, 0);
        netbuf_ddp(reply, at_network(), at_node(), a->rskt, 0);
        wr32(reply + 12, (uint32_t)slot + 1);
    }
    LOGF(LOG_TRAPS, "OT: NBP register -> %d\n", r);
    if (p->async)
        ot_post(p, T_REGNAMECOMPLETE, r, reply);
    return r;
}

/* OTRegisterName(ref, req, reply): the name, at the address given or at
 * a socket of the mapper's own, once nobody else answers to it. */
static void ota_RegisterName(ppc_cpu *c)
{
    provider *p = ot_provider_of(R(0));
    if (!p || p->kind != P_NBP_MAPPER) {
        RETURN((uint32_t)kOTBadReferenceErr);
        return;
    }
    ot_atalk *a = p->at;
    gaddr req = R(1);
    if (!req || a->verify) {
        RETURN((uint32_t)(a->verify ? kOTOutStateErr : kOTBadNameErr));
        return;
    }
    char s[NBP_BUF + 1];
    uint32_t n = rd32(req + 4) > NBP_BUF ? NBP_BUF : rd32(req + 4);
    gget(s, rd32(req + 8), n);
    parse_nbp(s, (int)n, a->rentity, &a->rlen);
    if (wildcard(a->rentity) || !a->rentity[0]) {
        RETURN((uint32_t)kOTBadNameErr);
        return;
    }
    at_addr at;
    if (rd32(req + 16) && get_addr(rd32(req + 20), rd32(req + 16), &at) && at.ddp && at.skt) {
        a->rskt = at.skt;
    } else {
        if (!a->own_skt)
            a->own_skt = (uint8_t)at_open_socket(0, NULL);
        a->rskt = a->own_skt;
    }
    if (at_name_taken(a->rentity, a->rlen)) {
        RETURN((uint32_t)kOTDuplicateFoundErr);
        return;
    }
    a->dup = false;
    a->rreq = req;
    a->rreply = R(2);
    a->verify = at_nbp_start(a->rentity, a->rlen, 2, 200, verify_answer, p);
    if (!a->verify) {
        RETURN((uint32_t)kOTOutOfMemoryErr);
        return;
    }
    if (p->async) {
        RETURN(kOTNoError); /* T_REGNAMECOMPLETE */
        return;
    }
    while (verify_running(a) && wait_slice(p))
        ;
    RETURN((uint32_t)register_done(p));
}

static int32_t delete_name(provider *p, int slot)
{
    ot_atalk *a = p->at;
    if (slot < 0 || slot >= MAX_NAMES || !a->names[slot])
        return kOTNotFoundErr;
    at_remove_name(a->names[slot]);
    a->names[slot] = 0;
    return kOTNoError;
}

/* OTDeleteName(ref, name TNetbuf *) */
static void ota_DeleteName(ppc_cpu *c)
{
    provider *p = ot_provider_of(R(0));
    if (!p || p->kind != P_NBP_MAPPER || !R(1)) {
        RETURN((uint32_t)kOTBadReferenceErr);
        return;
    }
    char s[NBP_BUF + 1];
    uint32_t n = rd32(R(1) + 4) > NBP_BUF ? NBP_BUF : rd32(R(1) + 4);
    gget(s, rd32(R(1) + 8), n);
    uint8_t want[ENTITY];
    int len, slot = -1;
    parse_nbp(s, (int)n, want, &len);
    for (int i = 0; i < MAX_NAMES && slot < 0; i++) {
        if (!p->at->names[i])
            continue;
        uint8_t had[ENTITY];
        gget(had, p->at->names[i] + 9, ENTITY);
        bool same = packed_len(had) == len;
        for (int k = 0; k < len && same; k++)
            same = tolower(had[k]) == tolower(want[k]);
        if (same)
            slot = i;
    }
    ot_complete(c, p, T_DELNAMECOMPLETE, delete_name(p, slot), R(1));
}

/* OTDeleteNameByID(ref, nameID) */
static void ota_DeleteNameByID(ppc_cpu *c)
{
    provider *p = ot_provider_of(R(0));
    if (!p || p->kind != P_NBP_MAPPER) {
        RETURN((uint32_t)kOTBadReferenceErr);
        return;
    }
    ot_complete(c, p, T_DELNAMECOMPLETE, delete_name(p, (int)R(1) - 1), 0);
}

/* ---------------------------------------------------------------------- */
/* Events */

void ota_poll(provider *p)
{
    ot_atalk *a = p->at;
    if (!a)
        return;
    switch (p->kind) {
    case P_DDP:
        if (a->nq && !p->told_data) {
            p->told_data = true;
            ot_post(p, T_DATA, 0, 0);
        }
        break;
    case P_ATP:
        for (int i = 0; i < MAX_OUT; i++)
            if (a->out[i].used && !a->out[i].told && a->out[i].reply.result != ioInProgress) {
                a->out[i].told = true;
                ot_post(p, T_REPLY, 0, 0);
            }
        for (int i = 0; i < MAX_REQS; i++)
            if (a->reqs[i].used && !a->reqs[i].taken && !a->told_request) {
                a->told_request = true;
                ot_post(p, T_REQUEST, 0, 0);
            }
        break;
    case P_ADSP:
        if (p->state == T_OUTCON && !p->told_connect && !p->told_disc) {
            int16_t e = pb_result(a->pb_open);
            if (e == noErr) {
                p->connected = p->told_connect = true;
                ot_post(p, T_CONNECT, 0, 0);
            } else if (e != ioInProgress) {
                p->disconnected = p->told_disc = true;
                p->disc_reason = open_error(e);
                ot_post(p, T_DISCONNECT, 0, 0);
            }
        }
        if (a->accepting && pb_result(a->pb_open) != ioInProgress) {
            int16_t e = pb_result(a->pb_open);
            a->accepting = false;
            p->state = e == noErr ? T_DATAXFER : T_IDLE;
            if (e == noErr && a->acceptor != p)
                ot_post(p, T_PASSCON, 0, 0);
            if (a->acceptor)
                ot_post(a->acceptor, T_ACCEPTCOMPLETE, e == noErr ? kOTNoError : open_error(e), 0);
            a->acceptor = NULL;
        }
        if (a->listening && !p->told_listen && pb_result(a->pb_listen) == noErr) {
            p->told_listen = true;
            ot_post(p, T_LISTEN, 0, 0);
        }
        if (adsp_open(p)) {
            if (!p->told_data && adsp_status(a, P_RECVPEND) > 0) {
                p->told_data = true;
                ot_post(p, T_DATA, 0, 0);
            }
            if (adsp_closed(a) && !p->eof) {
                p->eof = p->told_ordrel = true;
                ot_post(p, T_ORDREL, 0, 0);
            }
            if (p->flow && adsp_status(a, P_SENDFREE) > 0) {
                p->flow = false;
                ot_post(p, T_GODATA, 0, 0);
            }
        }
        break;
    case P_NBP_MAPPER: /* (synchronous calls finish themselves) */
        if (p->async && a->lookup && !lookup_running(a))
            lookup_done(p);
        if (p->async && a->verify && !verify_running(a))
            register_done(p);
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------------- */
/* Opening: AppleTalk services and NBP's mapper */

static char *config_name(gaddr cfg, char *out, size_t size)
{
    out[0] = 0;
    if (cfg < 0x100 || cfg >= 0xFFFFFF00u) /* kDefault...ServicesPath and the like */
        return out;
    size_t i = 0;
    for (; i + 1 < size && rd8(cfg + (gaddr)i); i++)
        out[i] = (char)rd8(cfg + (gaddr)i);
    out[i] = 0;
    return out;
}

static int open_kind(provider_kind kind, int32_t *err)
{
    int i = ot_new_provider(kind);
    if (i < 0) {
        *err = kOTOutOfMemoryErr;
        return -1;
    }
    *err = ota_open(&ot_providers[i]);
    if (*err != kOTNoError) {
        wr32(ot_providers[i].ref, 0);
        tb_DisposePtr(ot_providers[i].ref);
        ot_providers[i].used = false;
        return -1;
    }
    return i;
}

/* OTOpenAppleTalkServices(config, flags, &err, ...): ATSvcRef */
static void ota_OpenAppleTalkServices(ppc_cpu *c)
{
    int32_t err;
    int i = open_kind(P_AT_SERVICES, &err);
    if (R(2))
        wr32(R(2), (uint32_t)err);
    RETURN(i >= 0 ? ot_providers[i].ref : 0);
}

/* OTAsyncOpenAppleTalkServices(config, flags, notifier, context, ...) */
static void ota_AsyncOpenAppleTalkServices(ppc_cpu *c)
{
    int32_t err;
    int i = open_kind(P_AT_SERVICES, &err);
    if (i < 0) {
        RETURN((uint32_t)err);
        return;
    }
    provider *p = &ot_providers[i];
    p->async = true;
    p->notifier = R(2);
    p->context = R(3);
    ot_post(p, T_OPENCOMPLETE, kOTNoError, p->ref);
    RETURN(kOTNoError);
}

/* OTOpenMapper(config, oflag, &err, ...): NBP's, for "nbp". */
static void ota_OpenMapper(ppc_cpu *c)
{
    char name[64];
    provider_kind kind;
    int32_t err = kOTNotSupportedErr;
    int i = -1;
    if (ota_kind(config_name(R(0), name, sizeof name), &kind) && kind == P_NBP_MAPPER)
        i = open_kind(P_NBP_MAPPER, &err);
    if (R(2))
        wr32(R(2), (uint32_t)err);
    RETURN(i >= 0 ? ot_providers[i].ref : 0);
}

/* OTAsyncOpenMapper(config, oflag, notifier, context, ...) */
static void ota_AsyncOpenMapper(ppc_cpu *c)
{
    char name[64];
    provider_kind kind;
    int32_t err = kOTNotSupportedErr;
    int i = -1;
    if (ota_kind(config_name(R(0), name, sizeof name), &kind) && kind == P_NBP_MAPPER)
        i = open_kind(P_NBP_MAPPER, &err);
    if (i < 0) {
        RETURN((uint32_t)err);
        return;
    }
    provider *p = &ot_providers[i];
    p->async = true;
    p->notifier = R(2);
    p->context = R(3);
    ot_post(p, T_OPENCOMPLETE, kOTNoError, p->ref);
    RETURN(kOTNoError);
}

#define SERVICES(p)                                                \
    provider *p = ot_provider_of(R(0));                            \
    if (!p || p->kind != P_AT_SERVICES) {                          \
        RETURN((uint32_t)kOTBadReferenceErr);                      \
        return;                                                    \
    }

/* Zone names in a TNetbuf, Pascal strings one after another. */
static int32_t put_zones(gaddr nb, int count)
{
    if (!nb)
        return kOTBadDataErr;
    static const uint8_t star[2] = { 1, '*' };
    if (count && rd32(nb) < 2)
        return kOTBufferOverflowErr;
    if (count)
        gput(rd32(nb + 8), star, 2);
    wr32(nb + 4, count ? 2 : 0);
    return kOTNoError;
}

/* OTATalkGetMyZone(ref, zone TNetbuf *): "*", there being no router. */
static void ota_GetMyZone(ppc_cpu *c)
{
    SERVICES(p);
    ot_complete(c, p, T_GETMYZONECOMPLETE, put_zones(R(1), 1), R(1));
}

/* OTATalkGetLocalZones and OTATalkGetZoneList: the one zone. */
static void ota_GetLocalZones(ppc_cpu *c)
{
    SERVICES(p);
    ot_complete(c, p, T_GETLOCALZONESCOMPLETE, put_zones(R(1), 1), R(1));
}

static void ota_GetZoneList(ppc_cpu *c)
{
    SERVICES(p);
    ot_complete(c, p, T_GETZONELISTCOMPLETE, put_zones(R(1), 1), R(1));
}

/* OTATalkGetInfo(ref, info TNetbuf *): AppleTalkInfo is our address, the
 * router's, the cable range and flags (extended on EtherTalk). */
static void ota_GetInfo(ppc_cpu *c)
{
    SERVICES(p);
    gaddr nb = R(1);
    int32_t r = kOTNoError;
    if (!nb || rd32(nb) < 22) {
        r = kOTBufferOverflowErr;
    } else {
        gaddr info = rd32(nb + 8);
        bool extended = at_bridge_interface() != NULL;
        put_ddp(info, at_network(), at_node(), 0, 0);
        put_ddp(info + 8, 0, 0, 0, 0);
        wr16(info + 16, extended ? at_network() : 0);
        wr16(info + 18, extended ? at_network() : 0);
        wr16(info + 20, extended ? 0x0005 : 0x0004); /* kATalkInfoIsExtended, kATalkInfoOneZone */
        wr32(nb + 4, 22);
    }
    ot_complete(c, p, T_GETATALKINFOCOMPLETE, r, nb);
}

/* ---------------------------------------------------------------------- */
/* Addresses and NBP entities (OTATalkSharedLib) */

static void cstring(gaddr p, char *out, size_t size)
{
    size_t i = 0;
    for (; p && i + 1 < size && rd8(p + (gaddr)i); i++)
        out[i] = (char)rd8(p + (gaddr)i);
    out[i] = 0;
}

/* OTInitDDPAddress(addr, net, node, socket, ddpType) */
static void ota_InitDDPAddress(ppc_cpu *c)
{
    if (R(0))
        put_ddp(R(0), (uint16_t)R(1), (uint8_t)R(2), (uint8_t)R(3), (uint8_t)R(4));
    c->pc = c->lr;
}

/* OTInitNBPAddress(addr, name): its length */
static void ota_InitNBPAddress(ppc_cpu *c)
{
    char name[NBP_BUF + 1];
    cstring(R(1), name, sizeof name);
    if (R(0)) {
        wr16(R(0), AF_ATALK_NBP);
        gput(R(0) + 2, name, (uint32_t)strlen(name));
    }
    RETURN(2 + strlen(name));
}

/* OTInitDDPNBPAddress(addr, name, net, node, socket, ddpType): its length */
static void ota_InitDDPNBPAddress(ppc_cpu *c)
{
    char name[NBP_BUF + 1];
    cstring(R(1), name, sizeof name);
    if (R(0)) {
        put_ddp(R(0), (uint16_t)R(2), (uint8_t)R(3), (uint8_t)R(4), (uint8_t)R(5));
        wr16(R(0), AF_ATALK_DDPNBP);
        gput(R(0) + DDP_ADDR, name, (uint32_t)strlen(name));
    }
    RETURN(DDP_ADDR + strlen(name));
}

/* OTCompareDDPAddresses(a, b): equal, 0 matching anything */
static void ota_CompareDDPAddresses(ppc_cpu *c)
{
    gaddr x = R(0), y = R(1);
    if (!x || !y || rd16(x) != AF_ATALK_DDP || rd16(y) != AF_ATALK_DDP) {
        RETURN(0);
        return;
    }
    bool same = true;
    uint16_t nx = rd16(x + 2), ny = rd16(y + 2);
    same &= !nx || !ny || nx == ny;
    for (int k = 4; k < 6; k++) {
        uint8_t bx = rd8(x + (gaddr)k), by = rd8(y + (gaddr)k);
        same &= !bx || !by || bx == by;
    }
    RETURN(same);
}

/* OTInitNBPEntity(entity): three empty strings */
static void ota_InitNBPEntity(ppc_cpu *c)
{
    if (R(0))
        gmemset(R(0), 0, ENTITY);
    c->pc = c->lr;
}

static void get_entity(gaddr e, uint8_t *packed)
{
    gget(packed, e, ENTITY); /* (lengths over 32 are taken as 32) */
}

/* OTGetNBPEntityLengthAsAddress(entity): the length of its string */
static void ota_GetNBPEntityLengthAsAddress(ppc_cpu *c)
{
    uint8_t packed[ENTITY];
    char s[2 * ENTITY + 4];
    if (!R(0)) {
        RETURN(0);
        return;
    }
    get_entity(R(0), packed);
    RETURN(format_nbp(packed, s));
}

/* OTSetAddressFromNBPEntity(nameBuf, entity): the string, escaped; its length */
static void ota_SetAddressFromNBPEntity(ppc_cpu *c)
{
    uint8_t packed[ENTITY];
    char s[2 * ENTITY + 4];
    if (!R(1)) {
        RETURN(0);
        return;
    }
    get_entity(R(1), packed);
    int n = format_nbp(packed, s);
    if (R(0))
        gput(R(0), s, (uint32_t)n);
    RETURN(n);
}

/* OTSetAddressFromNBPString(addrBuf, name, len): the name copied (len -1:
 * all of it); its length */
static void ota_SetAddressFromNBPString(ppc_cpu *c)
{
    char name[NBP_BUF + 1];
    cstring(R(1), name, sizeof name);
    int32_t len = (int32_t)R(2);
    uint32_t n = len < 0 || (size_t)len > strlen(name) ? (uint32_t)strlen(name) : (uint32_t)len;
    if (R(0))
        gput(R(0), name, n);
    RETURN(n);
}

/* OTSetNBPEntityFromAddress(entity, addrBuf, len): an NBP or DDP-NBP
 * address's name (or a bare name string) split into the entity; false
 * when a part was too long. */
static void ota_SetNBPEntityFromAddress(ppc_cpu *c)
{
    gaddr buf = R(1);
    uint32_t len = R(2);
    if (buf && len >= 2 && rd16(buf) == AF_ATALK_NBP) {
        buf += 2;
        len -= 2;
    } else if (buf && len >= DDP_ADDR && rd16(buf) == AF_ATALK_DDPNBP) {
        buf += DDP_ADDR;
        len -= DDP_ADDR;
    }
    char s[NBP_BUF + 1];
    len = len > NBP_BUF ? NBP_BUF : len;
    if (buf)
        gget(s, buf, len);
    uint8_t packed[ENTITY];
    int n;
    bool whole = parse_nbp(s, buf ? (int)len : 0, packed, &n);
    if (R(0)) {
        gmemset(R(0), 0, ENTITY);
        gput(R(0), packed, (uint32_t)n);
    }
    RETURN(whole);
}

/* OTSetNBPName/Type/Zone(entity, part): one part replaced (cut to 32
 * characters, and false, when longer). */
static void set_part(ppc_cpu *c, int which)
{
    uint8_t packed[ENTITY], out[ENTITY] = { 0 };
    char part[256];
    cstring(R(1), part, sizeof part);
    if (!R(0)) {
        RETURN(0);
        return;
    }
    get_entity(R(0), packed);
    size_t len = strlen(part);
    bool whole = len <= NAME_MAX_LEN;
    int n = 0, k = 0;
    for (int i = 0; i < 3; i++) {
        int l = packed[k] > NAME_MAX_LEN ? NAME_MAX_LEN : packed[k];
        if (i == which) {
            out[n] = (uint8_t)(whole ? len : NAME_MAX_LEN);
            memcpy(out + n + 1, part, out[n]);
        } else {
            out[n] = (uint8_t)l;
            memcpy(out + n + 1, packed + k + 1, (size_t)l);
        }
        n += out[n] + 1;
        k += l + 1;
    }
    gmemset(R(0), 0, ENTITY);
    gput(R(0), out, (uint32_t)n);
    RETURN(whole);
}

static void ota_SetNBPName(ppc_cpu *c) { set_part(c, 0); }
static void ota_SetNBPType(ppc_cpu *c) { set_part(c, 1); }
static void ota_SetNBPZone(ppc_cpu *c) { set_part(c, 2); }

/* OTExtractNBPName/Type/Zone(entity, char *): one part, escaped */
static void extract_part(ppc_cpu *c, int which)
{
    uint8_t packed[ENTITY];
    char s[2 * NAME_MAX_LEN + 1];
    if (R(0) && R(1)) {
        get_entity(R(0), packed);
        int n = escape_part(packed, which, s);
        gput(R(1), s, (uint32_t)n + 1);
    }
    c->pc = c->lr;
}

static void ota_ExtractNBPName(ppc_cpu *c) { extract_part(c, 0); }
static void ota_ExtractNBPType(ppc_cpu *c) { extract_part(c, 1); }
static void ota_ExtractNBPZone(ppc_cpu *c) { extract_part(c, 2); }

const ppc_export ota_exports[] = {
    /* OTATalkClientLib */
    { "OTOpenAppleTalkServices", ota_OpenAppleTalkServices },
    { "OTOpenAppleTalkServicesPriv", ota_OpenAppleTalkServices },
    { "OTOpenAppleTalkServicesInContext", ota_OpenAppleTalkServices },
    { "OTAsyncOpenAppleTalkServices", ota_AsyncOpenAppleTalkServices },
    { "OTAsyncOpenAppleTalkServicesPriv", ota_AsyncOpenAppleTalkServices },
    { "OTAsyncOpenAppleTalkServicesInContext", ota_AsyncOpenAppleTalkServices },
    { "OTATalkGetMyZone", ota_GetMyZone }, { "OTATalkGetLocalZones", ota_GetLocalZones },
    { "OTATalkGetZoneList", ota_GetZoneList }, { "OTATalkGetInfo", ota_GetInfo },
    /* OTClientLib: the mapper and transactions */
    { "OTOpenMapper", ota_OpenMapper }, { "OTOpenMapperPriv", ota_OpenMapper },
    { "OTOpenMapperInContext", ota_OpenMapper }, { "OTAsyncOpenMapper", ota_AsyncOpenMapper },
    { "OTAsyncOpenMapperPriv", ota_AsyncOpenMapper }, { "OTAsyncOpenMapperInContext", ota_AsyncOpenMapper },
    { "OTRegisterName", ota_RegisterName }, { "OTDeleteName", ota_DeleteName },
    { "OTDeleteNameByID", ota_DeleteNameByID }, { "OTLookupName", ota_LookupName },
    { "OTSndURequest", ota_SndURequest }, { "OTRcvUReply", ota_RcvUReply },
    { "OTRcvURequest", ota_RcvURequest }, { "OTSndUReply", ota_SndUReply },
    { "OTCancelURequest", ota_CancelURequest }, { "OTCancelUReply", ota_CancelUReply },
    /* OTATalkSharedLib */
    { "OTInitDDPAddress", ota_InitDDPAddress }, { "OTInitNBPAddress", ota_InitNBPAddress },
    { "OTInitDDPNBPAddress", ota_InitDDPNBPAddress }, { "OTCompareDDPAddresses", ota_CompareDDPAddresses },
    { "OTInitNBPEntity", ota_InitNBPEntity }, { "OTGetNBPEntityLengthAsAddress", ota_GetNBPEntityLengthAsAddress },
    { "OTSetAddressFromNBPEntity", ota_SetAddressFromNBPEntity },
    { "OTSetAddressFromNBPString", ota_SetAddressFromNBPString },
    { "OTSetNBPEntityFromAddress", ota_SetNBPEntityFromAddress }, { "OTSetNBPName", ota_SetNBPName },
    { "OTSetNBPType", ota_SetNBPType }, { "OTSetNBPZone", ota_SetNBPZone },
    { "OTExtractNBPName", ota_ExtractNBPName }, { "OTExtractNBPType", ota_ExtractNBPType },
    { "OTExtractNBPZone", ota_ExtractNBPZone },
};
const unsigned ota_export_count = sizeof ota_exports / sizeof ota_exports[0];

