/*
 * ASP, the AppleTalk Session Protocol, for the AppleShare client (afp.c),
 * on ATP transactions from host code (appletalk_atp.c).
 *
 * A server listens on its session listening socket (SLS), found by NBP.
 * The workstation asks there for the server's status (GetStatus) and
 * opens a session (OpenSess, exactly-once), giving its own session socket
 * (WSS); the reply names the server's session socket (SSS) and a session
 * ID.  ATP's four user bytes carry the function, the session ID and a
 * sequence number or result.  A command goes to the SSS as an XO request
 * of up to 578 bytes, and its reply is up to eight 578-byte responses,
 * the first one's user bytes the command's result.  A write is a command
 * whose data the server asks for: it sends WriteContinue to the WSS with
 * the room it has, and the workstation's responses carry the data.  Each
 * end tickles the other (the workstation at the SLS) and closes a session
 * not heard from in two minutes; either end may close it (CloseSess).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "asp.h"
#include "tb_internal.h"

enum { CLOSE_SESS = 1, COMMAND = 2, GET_STATUS = 3, OPEN_SESS = 4, TICKLE = 5, WRITE = 6, WRITE_CONTINUE = 7,
       ATTENTION = 8 };
enum { ASP_VERSION = 0x0100, ATP_PKT = 578 };
/* a command waits up to 2 s for each try, for half a minute */
enum { CMD_TIMEOUT = 2000, CMD_RETRIES = 15 };

struct asp_session {
    int wss;
    uint8_t node, sls, sss, sid;
    uint16_t seq;
    bool open;
    /* the write in progress: its data for WriteContinue */
    bool writing;
    uint16_t write_seq;
    const uint8_t *wdata;
    int wlen;
    /* the workstation's attention routine, if it has one */
    void (*attn)(void *ctx, uint16_t code);
    void *attn_ctx;
};

static uint32_t user_bytes(int fn, int b1, int word) { return (uint32_t)fn << 24 | (uint32_t)(b1 & 0xFF) << 16 | (uint16_t)word; }

/* Requests from the server to our WSS. */
static void incoming(void *ctx, uint8_t skt, uint8_t node, uint8_t rskt, uint16_t tid, uint32_t user,
                     const uint8_t *data, int len)
{
    asp_session *s = ctx;
    int fn = (int)(user >> 24), sid = (int)(user >> 16 & 0xFF);
    if (node != s->node || sid != s->sid)
        return;
    static uint8_t pkts[8][ATP_PKT];
    int lens[8] = { 0 };
    switch (fn) {
    case TICKLE: break; /* the server is there; no response */
    case ATTENTION:
        atp_host_respond(skt, node, rskt, tid, 1, (const uint8_t(*)[ATP_PKT])pkts, lens, NULL);
        if (s->attn)
            s->attn(s->attn_ctx, (uint16_t)user);
        break;
    case CLOSE_SESS:
        s->open = false;
        atp_host_respond(skt, node, rskt, tid, 1, (const uint8_t(*)[ATP_PKT])pkts, lens, NULL);
        break;
    case WRITE_CONTINUE: {
        if (!s->writing || (uint16_t)user != s->write_seq)
            return;
        int room = len >= 2 ? data[0] << 8 | data[1] : ASP_QUANTUM;
        int n = s->wlen < room ? s->wlen : room, count = 0;
        if (n > ASP_QUANTUM)
            n = ASP_QUANTUM;
        do {
            int k = n - count * ATP_PKT;
            k = k > ATP_PKT ? ATP_PKT : k;
            if (k > 0)
                memcpy(pkts[count], s->wdata + count * ATP_PKT, (size_t)k);
            lens[count++] = k;
        } while (count * ATP_PKT < n);
        atp_host_respond(skt, node, rskt, tid, count, (const uint8_t(*)[ATP_PKT])pkts, lens, NULL);
        break;
    }
    }
}

/* GetStatus at a server's SLS, from a socket of its own. */
static void ignore(void *ctx, uint8_t skt, uint8_t node, uint8_t rskt, uint16_t tid, uint32_t user, const uint8_t *data,
                   int len)
{
}

int16_t asp_status_at(uint8_t node, uint8_t sls, uint8_t *status, int cap, int *status_len)
{
    if (status_len)
        *status_len = 0;
    int skt = atp_host_open(ignore, NULL);
    if (!skt)
        return -1098; /* tooManySkts */
    static atp_reply r;
    int16_t e = atp_host_request(skt, node, sls, user_bytes(GET_STATUS, 0, 0), NULL, 0, false, 8, 1000, 3, &r);
    atp_host_close(skt);
    if (e != noErr)
        return e;
    int n = 0;
    for (int i = 0; i < r.count; i++) {
        int k = r.len[i];
        if (n + k > cap)
            k = cap - n;
        if (k > 0 && status)
            memcpy(status + n, r.data[i], (size_t)k);
        n += k > 0 ? k : 0;
    }
    if (status_len)
        *status_len = n;
    return noErr;
}

asp_session *asp_open_at(uint8_t node, uint8_t sls, int interval_ms, int tries, int16_t *err)
{
    asp_session *s = calloc(1, sizeof *s);
    if (!s) {
        *err = memFullErr;
        return NULL;
    }
    s->node = node;
    s->sls = sls;
    s->wss = atp_host_open(incoming, s);
    if (!s->wss) {
        *err = -1098; /* tooManySkts */
        free(s);
        return NULL;
    }
    static atp_reply r;
    int16_t e = atp_host_request(s->wss, node, sls, user_bytes(OPEN_SESS, s->wss, ASP_VERSION), NULL, 0, true, 1,
                                 interval_ms, tries, &r);
    int16_t spe = e == noErr ? (int16_t)(r.user[0] & 0xFFFF) : e;
    if (spe != noErr) {
        *err = spe;
        atp_host_close(s->wss);
        free(s);
        return NULL;
    }
    s->sss = (uint8_t)(r.user[0] >> 24);
    s->sid = (uint8_t)(r.user[0] >> 16);
    s->open = true;
    *err = noErr;
    return s;
}

void asp_set_attention(asp_session *s, void (*fn)(void *ctx, uint16_t code), void *ctx)
{
    s->attn = fn;
    s->attn_ctx = ctx;
}

asp_session *asp_open(const char *nbp_name, uint8_t *status, int *status_len, char *err, size_t errlen)
{
    char obj[33] = "", type[33] = "", zone[33] = "*";
    const char *colon = strchr(nbp_name, ':'), *at = strrchr(nbp_name, '@');
    size_t on = colon ? (size_t)(colon - nbp_name) : strlen(nbp_name);
    if (on > 32)
        on = 32;
    memcpy(obj, nbp_name, on);
    obj[on] = 0;
    if (colon) {
        size_t tn = at && at > colon ? (size_t)(at - colon - 1) : strlen(colon + 1);
        tn = tn > 32 ? 32 : tn;
        memcpy(type, colon + 1, tn);
        type[tn] = 0;
    } else {
        snprintf(type, sizeof type, "AFPServer");
    }
    if (at && at[1])
        snprintf(zone, sizeof zone, "%.32s", at + 1);
    if (!appletalk_enabled() || at_open() != noErr) {
        snprintf(err, errlen, "AppleTalk is off");
        return NULL;
    }
    uint16_t net;
    uint8_t node, sls;
    if (at_nbp_lookup(obj, type, zone, 4, 750, &net, &node, &sls) != noErr) {
        const char *bridge = at_bridge_interface();
        if (bridge && !et_heard())
            /* No AARP, no RTMP, nothing: the network isn't carrying EtherTalk. */
            snprintf(err, errlen, "no %s:%s@%s on the network (no AppleTalk at all heard on %s)", obj, type, zone,
                     bridge);
        else if (bridge)
            snprintf(err, errlen, "no %s:%s@%s on the network (%d AppleTalk frames heard on %s, none from it)", obj,
                     type, zone, et_heard(), bridge);
        else
            snprintf(err, errlen, "no %s:%s@%s on the network", obj, type, zone);
        return NULL;
    }
    asp_status_at(node, sls, status, ASP_QUANTUM, status_len);
    int16_t e;
    asp_session *s = asp_open_at(node, sls, 1000, 5, &e);
    if (!s) {
        if (e == -1098)
            snprintf(err, errlen, "no ATP socket");
        else
            snprintf(err, errlen, "the server didn't open a session (%d)", e);
        return NULL;
    }
    return s;
}

/* A reply's result and data. */
static int take_reply(asp_session *s, int16_t e, const atp_reply *r, uint8_t *reply, int cap, int *rlen)
{
    if (rlen)
        *rlen = 0;
    if (e != noErr) {
        s->open = false;
        return ASP_ERR_SESS_CLOSED;
    }
    int n = 0;
    for (int i = 0; i < r->count; i++) {
        int k = r->len[i];
        if (n + k > cap)
            k = cap - n;
        if (k > 0 && reply)
            memcpy(reply + n, r->data[i], (size_t)k);
        n += k > 0 ? k : 0;
    }
    if (rlen)
        *rlen = n;
    return (int32_t)r->user[0];
}

int asp_command(asp_session *s, const uint8_t *cmd, int len, uint8_t *reply, int cap, int *rlen)
{
    if (!s->open)
        return ASP_ERR_SESS_CLOSED;
    if (len > ASP_CMD_MAX)
        return -5019; /* kFPParmErr */
    static atp_reply r;
    int16_t e = atp_host_request(s->wss, s->node, s->sss, user_bytes(COMMAND, s->sid, s->seq++), cmd, len, true, 8,
                                 CMD_TIMEOUT, CMD_RETRIES, &r);
    return take_reply(s, e, &r, reply, cap, rlen);
}

int asp_write(asp_session *s, const uint8_t *cmd, int len, const uint8_t *data, int dlen, uint8_t *reply, int cap,
              int *rlen)
{
    if (!s->open)
        return ASP_ERR_SESS_CLOSED;
    if (len > ASP_CMD_MAX || dlen > ASP_QUANTUM)
        return -5019;
    static atp_reply r;
    s->writing = true;
    s->write_seq = s->seq;
    s->wdata = data;
    s->wlen = dlen;
    int16_t e = atp_host_request(s->wss, s->node, s->sss, user_bytes(WRITE, s->sid, s->seq++), cmd, len, true, 8,
                                 CMD_TIMEOUT, CMD_RETRIES, &r);
    s->writing = false;
    return take_reply(s, e, &r, reply, cap, rlen);
}

void asp_tickle(asp_session *s)
{
    if (s->open)
        atp_host_send_once(s->wss, s->node, s->sls, user_bytes(TICKLE, s->sid, 0), NULL, 0);
}

bool asp_alive(asp_session *s) { return s && s->open; }

void asp_close(asp_session *s)
{
    if (!s)
        return;
    if (s->open) {
        static atp_reply r;
        atp_host_request(s->wss, s->node, s->sss, user_bytes(CLOSE_SESS, s->sid, 0), NULL, 0, true, 1, 1000, 2, &r);
    }
    atp_host_close(s->wss);
    free(s);
}
