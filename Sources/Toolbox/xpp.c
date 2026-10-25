/*
 * The .XPP driver's ASP workstation calls and AFP calls (AppleTalk.h's
 * ASPOpenSession ... AFPCommand), on asp.c's sessions: a program talks to
 * an AppleShare server, or any ASP server, itself.
 *
 * A session is opened at the server's address (its session listening
 * socket, found by NBP), and named by a session reference number.  Calls
 * are done when they return, asynchronous ones included (their completion
 * routines run afterwards).  AFPCommand sends an AFP command block as it
 * is, except that a login opens the session first and a logout closes it
 * after, and the read and write calls take the driver's AFPCommandBlock,
 * split into transactions of a quantum each.  A server's attentions go to
 * the session's attention routine (D0 the session, D1 the attention code).
 */
#include <stdlib.h>
#include <string.h>

#include "asp.h"
#include "legacy.h"
#include "tb_internal.h"

enum {
    openSess = 255, closeSess = 254, userCommand = 253, userWrite = 252, getStatus = 251, afpCall = 250,
    getParms = 249, abortOS = 248, closeAll = 247,
};
enum {
    aspBufTooSmall = -1067, aspNoServers = -1069, aspParamErr = -1070, aspSessClosed = -1072, aspSizeErr = -1073,
    aspTooMany = -1074, cbNotFound = -1102,
};
/* XPPPrmBlk after csCode, and ASPOpenPrm's and AFPLoginPrm's own */
enum {
    X_CMDRESULT = 18, X_SESS = 28, X_CBSIZE = 32, X_CBPTR = 34, X_RBSIZE = 38, X_RBPTR = 40, X_WDSIZE = 44,
    X_WDPTR = 46, X_SERVER = 32, X_ATTN = 40, X_LOGIN_ADDR = 44, X_LOGIN_ATTN = 52,
};
enum { afpLogin = 18, afpLogout = 20, afpRead = 27, afpWrite = 33, afpAddIcon = 192 };
enum { MAX_SESS = 8 };

typedef struct {
    asp_session *s;
    gaddr attn;
    int16_t refnum;
} xpp_session;
static xpp_session sessions[MAX_SESS];

void xpp_reset(void)
{
    /* the sessions went with the machine's AppleTalk */
    memset(sessions, 0, sizeof sessions);
}

static xpp_session *find(int16_t refnum)
{
    for (int i = 0; i < MAX_SESS; i++)
        if (sessions[i].s && sessions[i].refnum == refnum)
            return &sessions[i];
    return NULL;
}

/* The program's attention routine, as the driver called it. */
static void attention(void *ctx, uint16_t code)
{
    xpp_session *x = ctx;
    if (!x->attn || !g_cpu)
        return;
    cpu68k *c = g_cpu;
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d);
    memcpy(a, c->a, sizeof a);
    uint16_t sr = c->sr;
    c->d[0] = (uint32_t)(uint16_t)x->refnum;
    c->d[1] = code;
    guest_call_regs(x->attn);
    memcpy(c->d, d, sizeof d);
    memcpy(c->a, a, sizeof a);
    c->sr = sr;
}

/* A session at the server's AddrBlock, asking every aspTimeout seconds,
 * aspRetry times more. */
static int16_t open_at(gaddr pb, gaddr addr, gaddr attn, int16_t *refnum)
{
    int i = 0;
    while (i < MAX_SESS && sessions[i].s)
        i++;
    if (i == MAX_SESS)
        return aspTooMany;
    int16_t err;
    int secs = rd8(pb + 30) ? rd8(pb + 30) : 1;
    asp_session *s = asp_open_at(rd8(addr + 2), rd8(addr + 3), 1000 * secs, 1 + rd8(pb + 31), &err);
    if (!s)
        return err <= -1066 && err >= -1075 ? err : aspNoServers; /* the server's refusal, or nobody there */
    static int16_t next = 1;
    sessions[i] = (xpp_session){ s, attn, next++ };
    asp_set_attention(s, attention, &sessions[i]);
    *refnum = sessions[i].refnum;
    return noErr;
}

static void close_session(xpp_session *x)
{
    asp_close(x->s);
    x->s = NULL;
}

/* A command's reply into the program's buffer: its result in cmdResult,
 * its length in rbSize. */
static int16_t command(xpp_session *x, gaddr pb, const uint8_t *cmd, int len, bool write, const uint8_t *data,
                       int dlen)
{
    static uint8_t reply[ASP_QUANTUM];
    int cap = (int16_t)rd16(pb + X_RBSIZE), n = 0;
    if (cap < 0)
        return aspParamErr;
    if (cap > ASP_QUANTUM)
        cap = ASP_QUANTUM;
    int r = write ? asp_write(x->s, cmd, len, data, dlen, reply, cap, &n) : asp_command(x->s, cmd, len, reply, cap, &n);
    if (!asp_alive(x->s)) {
        close_session(x);
        return aspSessClosed;
    }
    if (n && rd32(pb + X_RBPTR))
        gput(rd32(pb + X_RBPTR), reply, (uint32_t)n);
    wr16(pb + X_RBSIZE, (uint16_t)n);
    wr32(pb + X_CMDRESULT, (uint32_t)r);
    return noErr;
}

static int16_t user_call(gaddr pb, bool write)
{
    xpp_session *x = find((int16_t)rd16(pb + X_SESS));
    if (!x)
        return aspParamErr;
    int len = (int16_t)rd16(pb + X_CBSIZE), dlen = write ? (int16_t)rd16(pb + X_WDSIZE) : 0;
    if (len < 0 || len > ASP_CMD_MAX)
        return aspSizeErr;
    if (dlen < 0 || dlen > ASP_QUANTUM)
        return aspSizeErr;
    uint8_t cmd[ASP_CMD_MAX];
    static uint8_t data[ASP_QUANTUM];
    gget(cmd, rd32(pb + X_CBPTR), (uint32_t)len);
    if (dlen)
        gget(data, rd32(pb + X_WDPTR), (uint32_t)dlen);
    return command(x, pb, cmd, len, write, data, dlen);
}

/* afpRead and afpWrite: the AFPCommandBlock (cmdByte, startEndFlag,
 * forkRefNum, rwOffset, reqCount, newLineFlag, newLineChar) as FPRead or
 * FPWrite, a quantum at a time; the bytes read in rbSize. */
static int16_t read_write(xpp_session *x, gaddr pb, gaddr blk)
{
    bool write = rd8(blk) == afpWrite;
    uint16_t fork = rd16(blk + 2);
    uint32_t offset = rd32(blk + 4), want = rd32(blk + 8);
    gaddr buf = write ? rd32(pb + X_WDPTR) : rd32(pb + X_RBPTR);
    if (write && want > (uint32_t)(uint16_t)rd16(pb + X_WDSIZE))
        want = (uint16_t)rd16(pb + X_WDSIZE);
    if (!write && want > (uint32_t)(uint16_t)rd16(pb + X_RBSIZE))
        want = (uint16_t)rd16(pb + X_RBSIZE);
    static uint8_t reply[ASP_QUANTUM];
    uint32_t done = 0;
    int r = 0;
    do {
        uint32_t k = want - done > ASP_QUANTUM ? ASP_QUANTUM : want - done;
        uint8_t cmd[14] = { rd8(blk), write ? rd8(blk + 1) : 0, (uint8_t)(fork >> 8), (uint8_t)fork };
        uint32_t at = offset + done;
        cmd[4] = (uint8_t)(at >> 24), cmd[5] = (uint8_t)(at >> 16), cmd[6] = (uint8_t)(at >> 8), cmd[7] = (uint8_t)at;
        cmd[8] = (uint8_t)(k >> 24), cmd[9] = (uint8_t)(k >> 16), cmd[10] = (uint8_t)(k >> 8), cmd[11] = (uint8_t)k;
        cmd[12] = write ? 0 : (rd8(blk + 12) ? 0xFF : 0);
        cmd[13] = write ? 0 : rd8(blk + 13);
        int n = 0;
        if (write) {
            static uint8_t data[ASP_QUANTUM];
            gget(data, buf + done, k);
            r = asp_write(x->s, cmd, 12, data, (int)k, reply, sizeof reply, &n);
        } else {
            r = asp_command(x->s, cmd, 14, reply, (int)k, &n);
            if (n > 0)
                gput(buf + done, reply, (uint32_t)n);
        }
        if (!asp_alive(x->s)) {
            close_session(x);
            return aspSessClosed;
        }
        done += write ? k : (uint32_t)n;
        if (r || (!write && (uint32_t)n < k))
            break; /* an error, the end of the fork or a newline */
    } while (done < want);
    if (!write)
        wr16(pb + X_RBSIZE, (uint16_t)done);
    else if (rd32(pb + X_RBPTR) && (int16_t)rd16(pb + X_RBSIZE) >= 4)
        gput(rd32(pb + X_RBPTR), reply, 4); /* the last offset written */
    wr32(pb + X_CMDRESULT, (uint32_t)r);
    return noErr;
}

static int16_t afp_call(gaddr pb)
{
    gaddr cb = rd32(pb + X_CBPTR);
    if (!cb)
        return aspParamErr;
    uint8_t code = rd8(cb);
    if (code == afpLogin) {
        int16_t refnum, err = open_at(pb, pb + X_LOGIN_ADDR, rd32(pb + X_LOGIN_ATTN), &refnum);
        if (err)
            return err;
        wr16(pb + X_SESS, (uint16_t)refnum);
        xpp_session *x = find(refnum);
        err = user_call(pb, false);
        int32_t r = (int32_t)rd32(pb + X_CMDRESULT);
        if (find(refnum) && (err || (r && r != -5001))) /* refused, unless kFPAuthContinue */
            close_session(x);
        return err;
    }
    xpp_session *x = find((int16_t)rd16(pb + X_SESS));
    if (!x)
        return aspParamErr;
    if (code == afpRead || code == afpWrite)
        return read_write(x, pb, cb);
    int16_t err = user_call(pb, code == afpAddIcon);
    if (code == afpLogout && find((int16_t)rd16(pb + X_SESS)))
        close_session(x);
    return err;
}

int16_t xpp_asp_control(gaddr pb, bool async)
{
    switch ((int16_t)rd16(pb + CntrlParam_csCode_off)) {
    case openSess: {
        int16_t refnum = 0, err = open_at(pb, pb + X_SERVER, rd32(pb + X_ATTN), &refnum);
        wr16(pb + X_SESS, (uint16_t)refnum);
        return err;
    }
    case closeSess: {
        xpp_session *x = find((int16_t)rd16(pb + X_SESS));
        if (!x)
            return aspParamErr;
        close_session(x);
        return noErr;
    }
    case userCommand: return user_call(pb, false);
    case userWrite: return user_call(pb, true);
    case getStatus: {
        static uint8_t status[ASP_QUANTUM];
        gaddr addr = pb + X_SERVER;
        int cap = (int16_t)rd16(pb + X_RBSIZE), n = 0;
        if (cap < 0)
            return aspParamErr;
        if (asp_status_at(rd8(addr + 2), rd8(addr + 3), status, cap > ASP_QUANTUM ? ASP_QUANTUM : cap, &n))
            return aspNoServers;
        if (n)
            gput(rd32(pb + X_RBPTR), status, (uint32_t)n);
        wr16(pb + X_RBSIZE, (uint16_t)n);
        return noErr;
    }
    case afpCall: return afp_call(pb);
    case getParms:
        wr16(pb + 28, ASP_CMD_MAX);  /* aspMaxCmdSize */
        wr16(pb + 30, ASP_QUANTUM);  /* aspQuantumSize */
        wr16(pb + 32, MAX_SESS);     /* numSesss */
        return noErr;
    case abortOS: return cbNotFound; /* opening is done before the call returns */
    case closeAll:
        for (int i = 0; i < MAX_SESS; i++)
            if (sessions[i].s)
                close_session(&sessions[i]);
        return noErr;
    default: return -17; /* controlErr */
    }
}
