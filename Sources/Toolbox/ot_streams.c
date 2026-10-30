/*
 * Open Transport's raw STREAMS interface (OTNativeClientLib; for 68k code
 * the glue's OTClnt$NtvClient set): streams opened by a driver's name or a
 * configuration, the STREAMS head's ioctls, messages in and out, polling,
 * pipes, OTBuffer reads, notifiers, and the state machines built on them.
 *
 * Open Transport ran Mentat's STREAMS inside the Mac; here the network is
 * the host's sockets, which have no STREAMS below them.  A stream is a
 * provider (ot_internal.h) of kind P_STREAM with a head of its own: its
 * read queue, read and write modes, signals and the modules pushed on it.
 * The drivers behave as far as a stream with no network under it can:
 * "echo" sends every message back up, "nuls" swallows them, a pipe's two
 * ends carry them to each other; the transport drivers (tcp, udp, ip, ddp
 * ...) answer T_INFO_REQ with what their endpoints report and any other
 * TPI request with T_ERROR_ACK TNOTSUPPORT, and take no ioctl of their
 * own (EINVAL, as a STREAMS driver that doesn't know a command answers).
 * A module can be pushed only if Open Transport shipped it; one a program
 * brings along (a filter for IP) can't be loaded, and I_PUSH says EINVAL.
 * "sad" keeps the modules to push on a device's streams (autopush), and ip
 * answers the named-parameter ioctls for its tunables from the host's.
 * Endpoints for a stream's transport come from OTOpenEndpointOnStream.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#include "machine.h"
#include "ot_internal.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

void machine_pass_tick(void); /* trap.c: time passes while waiting */

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; } while (0)

enum {
    kEINVALErr = -3221, kENXIOErr = -3205, kEBADFErr = -3208, kEAGAINErr = -3210, kENOSRErr = -3271,
    kEBADMSGErr = -3272, kENODATAErr = -3275, kERANGEErr = -3233, kEPIPEErr = -3231, kENOSTRErr = -3274,
    kEEXISTErr = -3216, kENOENTErr = -3201,
};
enum {
    kGetmsgEvent = 0x21000002, kStreamReadEvent = 0x21000003, kStreamWriteEvent = 0x21000004,
    kStreamIoctlEvent = 0x21000005, kStreamOpenEvent = 0x21000007, kPollEvent = 0x21000008,
    kSIGNALEVENT = 0x22000000, kSIGHUP = 1, kSIGPOLL = 30,
};
enum { M_DATA = 0x00, M_PROTO = 0x01, M_PCPROTO = 0x83 };
enum {
    I_NREAD = 0x4101, I_PUSH, I_POP, I_LOOK, I_FLUSH, I_SRDOPT, I_GRDOPT, I_STR, I_SETSIG, I_GETSIG, I_FIND, I_LINK,
    I_UNLINK, I_PEEK = 0x410F, I_FDINSERT, I_SENDFD, I_RECVFD, I_FLUSHBAND, I_SWROPT, I_GWROPT, I_LIST, I_ATMARK,
    I_CKBAND, I_GETBAND, I_CANPUT, I_SETCLTIME, I_GETCLTIME, I_PLINK, I_PUNLINK,
    I_GETMSG = 0x4128, I_PUTMSG, I_POLL, I_SETDELAY, I_GETDELAY, I_RUN_QUEUES, I_GETPMSG, I_PUTPMSG, I_AUTOPUSH,
    I_PIPE, I_HEAP_REPORT, I_FIFO,
    I_OTGetMiscellaneousEvents = 0x4F01, I_OTSetFramingType, I_OTSetRawMode, I_OTConnect, I_OTDisconnect, I_OTScript,
};
enum { RS_HIPRI = 1, MSG_HIPRI = 1, MSG_BAND = 2, MSG_ANY = 4, MORECTL = 1, MOREDATA = 2 };
enum { FLUSHR = 1, FLUSHW = 2, ANYMARK = 1, LASTMARK = 2, SNDZERO = 1 };
enum { RNORM = 0x01, RMSGD = 0x02, RMSGN = 0x04, RPROTNORM = 0x10, RPROTDIS = 0x20, RPROTDAT = 0x40 };
enum { S_INPUT = 0x01, S_HIPRI = 0x02, S_RDNORM = 0x10, S_RDBAND = 0x20 };
enum {
    POLLIN = 0x001, POLLPRI = 0x002, POLLOUT = 0x004, POLLERR = 0x008, POLLHUP = 0x010, POLLNVAL = 0x020,
    POLLRDNORM = 0x040, POLLRDBAND = 0x080, POLLWRBAND = 0x100,
};
enum { T_INFO_REQ = 107, T_ERROR_ACK = 127, T_INFO_ACK = 129, TNOTSUPPORT = 18, T_DATA_REQ = 104, T_EXDATA_REQ = 106 };
enum { MAX_QUEUE = 128, MAX_MODS = 8, NAMESZ = 32 };

typedef enum { D_ECHO, D_NULS, D_PIPE, D_TPI, D_SAD, D_OTHER } drv_kind;

typedef struct {
    uint8_t *ctl, *data;
    int32_t clen, dlen;  /* -1: the part isn't there */
    uint8_t band;
    bool hipri;
} smsg;

struct ot_stream {
    char driver[NAMESZ];
    drv_kind drv;
    provider_kind tpi;            /* the transport, for D_TPI */
    char mods[MAX_MODS][NAMESZ];  /* pushed, the one under the head first */
    int nmods;
    smsg q[MAX_QUEUE];
    int nq;
    int32_t rdopt, wropt, sigs, cltime;
    uint32_t ctlmask;
    bool hangup, syncidle, misc_events;
    int peer;                     /* a pipe's other end, -1 */
    /* an asynchronous getmsg waiting for a message */
    bool get_pending;
    gaddr get_ctl, get_data, get_flags, get_band;
    bool get_any_band;
};

/* ---- Streams as providers ---- */

static provider *stream_of(gaddr ref)
{
    provider *p = ot_provider_of(ref);
    return p && p->kind == P_STREAM && p->st ? p : NULL;
}

#define STREAM(p)                                      \
    provider *p = stream_of(R(0));                     \
    if (!p) {                                          \
        RETURN((uint32_t)kEBADFErr);                   \
        return;                                        \
    }
#define STREAM_VOID(p)                                 \
    provider *p = stream_of(R(0));                     \
    if (!p) {                                          \
        c->pc = c->lr;                                 \
        return;                                        \
    }

static const char *const tpi_names[] = { "tcp", "udp", "rawip", "ip", "ddp", "atp", "adsp", "pap", "asp", "nbp", "zip",
                                         "aarp", "arp", "dnr", "atlk", "ltlk", NULL };
/* The modules Open Transport shipped that a stream may have pushed. */
static const char *const known_modules[] = { "timod", "tirdwr", "pipemod", "tilisten", "sockmod", "tmod", NULL };

static bool in_list(const char *const *list, const char *s)
{
    for (int i = 0; list[i]; i++)
        if (!strcmp(list[i], s))
            return true;
    return false;
}

static provider_kind tpi_kind(const char *name)
{
    provider_kind k;
    if (ota_kind(name, &k))
        return k;
    return !strcmp(name, "udp") ? P_UDP : P_TCP;
}

/* The name of the driver at the bottom of a configuration's string: its
 * first element, without options ("tcp(Port=enet)", "tilisten,tcp"). */
static void top_name(const char *path, char out[NAMESZ])
{
    size_t n = 0;
    while (path[n] && path[n] != '(' && path[n] != ',' && path[n] != ':' && n < NAMESZ - 1)
        n++;
    memcpy(out, path, n);
    out[n] = 0;
}

static void cstr_at(gaddr a, char *out, size_t size)
{
    size_t i = 0;
    for (; a && i < size - 1 && rd8(a + i); i++)
        out[i] = (char)rd8(a + i);
    out[i] = 0;
}

/* A device the program may open by name: a driver Open Transport has, or a
 * port (enet, enet1 ...). */
static bool driver_known(const char *name, drv_kind *kind)
{
    if (!strcmp(name, "echo"))
        *kind = D_ECHO;
    else if (!strcmp(name, "nuls"))
        *kind = D_NULS;
    else if (in_list(tpi_names, name))
        *kind = D_TPI;
    else if (!strcmp(name, "sad"))
        *kind = D_SAD;
    else if (!strcmp(name, "log") || !strcmp(name, "sc") ||
             !strncmp(name, "enet", 4) || !strncmp(name, "fenet", 5) || !strncmp(name, "tokn", 4) ||
             !strncmp(name, "fddi", 4) || !strncmp(name, "ppp", 3) || !strncmp(name, "slip", 4))
        *kind = D_OTHER;
    else
        return false;
    return true;
}

static void free_msg(smsg *m)
{
    free(m->ctl);
    free(m->data);
    m->ctl = m->data = NULL;
}

static void stream_free(provider *p)
{
    struct ot_stream *s = p->st;
    if (!s)
        return;
    for (int i = 0; i < s->nq; i++)
        free_msg(&s->q[i]);
    if (s->peer >= 0 && ot_providers[s->peer].used && ot_providers[s->peer].st) {
        struct ot_stream *o = ot_providers[s->peer].st;
        o->peer = -1;
        o->hangup = true; /* the other end of the pipe is gone */
        ot_post(&ot_providers[s->peer], kSIGNALEVENT + kSIGHUP, 0, 0);
    }
    free(s);
    p->st = NULL;
}

void ot_stream_close(provider *p) { stream_free(p); }

/* ---- Autopush (the "sad" driver): modules pushed on every stream opened
 * on a device, by the device's name as Open Transport keeps them. ---- */

enum { MAX_AUTOPUSH = 16, kAutopushMax = 8, kSAP_ONE = 1, kSAP_RANGE, kSAP_ALL, kSAP_CLEAR };
enum { I_SAD_SAP = 0x6701, I_SAD_GAP, I_SAD_VML, kENODEVErr = -3218 };
static struct {
    char device[NAMESZ];
    int32_t minor, lastminor, cmd;
    int npush;
    char list[kAutopushMax][NAMESZ];
} autopush[MAX_AUTOPUSH];

static int autopush_of(const char *device)
{
    for (int i = 0; i < MAX_AUTOPUSH; i++)
        if (autopush[i].device[0] && !strcmp(autopush[i].device, device))
            return i;
    return -1;
}

/* OTAutopushInfo: sap_cmd, sap_device_name[32], sap_minor, sap_lastminor,
 * sap_npush, sap_list[8][32]. */
static int32_t sad_ioctl(uint32_t cmd, gaddr info)
{
    if (!info)
        return kEINVALErr;
    if (cmd == I_SAD_VML) { /* str_list: sl_nmods, sl_modlist */
        int32_t n = (int32_t)rd32(info);
        gaddr list = rd32(info + 4);
        if (n < 1 || !list)
            return kEINVALErr;
        for (int32_t i = 0; i < n; i++) {
            char m[NAMESZ];
            cstr_at(list + 32 * (uint32_t)i, m, sizeof m);
            if (!in_list(known_modules, m))
                return 1;
        }
        return 0;
    }
    char device[NAMESZ];
    cstr_at(info + 4, device, sizeof device);
    int at = autopush_of(device);
    if (cmd == I_SAD_GAP) {
        if (at < 0)
            return kENODEVErr;
        wr32(info, (uint32_t)autopush[at].cmd);
        wr32(info + 36, (uint32_t)autopush[at].minor);
        wr32(info + 40, (uint32_t)autopush[at].lastminor);
        wr32(info + 44, (uint32_t)autopush[at].npush);
        gmemset(info + 48, 0, kAutopushMax * 32);
        for (int i = 0; i < autopush[at].npush; i++)
            gput(info + 48 + 32 * (uint32_t)i, autopush[at].list[i], (uint32_t)strlen(autopush[at].list[i]));
        return 0;
    }
    if (cmd != I_SAD_SAP)
        return kEINVALErr;
    int32_t sap = (int32_t)rd32(info);
    if (sap == kSAP_CLEAR) {
        if (at < 0)
            return kENODEVErr;
        autopush[at].device[0] = 0;
        return 0;
    }
    int32_t npush = (int32_t)rd32(info + 44);
    if (sap < kSAP_ONE || sap > kSAP_ALL || npush < 1 || npush > kAutopushMax || !device[0])
        return kEINVALErr;
    char list[kAutopushMax][NAMESZ];
    for (int32_t i = 0; i < npush; i++) {
        cstr_at(info + 48 + 32 * (uint32_t)i, list[i], NAMESZ);
        if (!in_list(known_modules, list[i]))
            return kEINVALErr; /* a module Open Transport can't load */
    }
    if (at >= 0)
        return kEEXISTErr;
    for (at = 0; at < MAX_AUTOPUSH && autopush[at].device[0]; at++)
        ;
    if (at == MAX_AUTOPUSH)
        return kENOSRErr;
    snprintf(autopush[at].device, NAMESZ, "%s", device);
    autopush[at].cmd = sap;
    autopush[at].minor = (int32_t)rd32(info + 36);
    autopush[at].lastminor = (int32_t)rd32(info + 40);
    autopush[at].npush = (int)npush;
    memcpy(autopush[at].list, list, sizeof list);
    return 0;
}

/* ---- Named parameters (ND_GET, ND_SET: Mentat's "nd" ioctls, by I_STR to
 * ip): the IP stack's tunables, read from the host's.  Setting one changes
 * what this run's programs see, never the host. ---- */

enum { ND_GET = 0x6300, ND_SET = 0x6301 };
static struct {
    const char *name;
    const char *sysctl;
    int32_t value;
    bool set;
} nd_params[] = {
    { "ip_forwarding", "net.inet.ip.forwarding", 0, false },
    { "ip_def_ttl", "net.inet.ip.ttl", 64, false },
};

static int32_t nd_value(int i)
{
    if (!nd_params[i].set) {
        nd_params[i].set = true;
#ifdef __APPLE__
        int v;
        size_t len = sizeof v;
        if (sysctlbyname(nd_params[i].sysctl, &v, &len, NULL, 0) == 0)
            nd_params[i].value = v;
#endif
    }
    return nd_params[i].value;
}

/* The buffer holds the name; ND_GET leaves the value in it as text (or, for
 * "?", the names), ND_SET has the value after the name's NUL. */
static int32_t nd_ioctl(const struct ot_stream *s, uint32_t cmd, gaddr buf, int32_t len)
{
    if (strcmp(s->driver, "ip") || !buf || len <= 0)
        return kEINVALErr;
    char name[64];
    cstr_at(buf, name, sizeof name);
    char out[256] = "";
    int n = (int)(sizeof nd_params / sizeof nd_params[0]);
    if (cmd == ND_GET && !strcmp(name, "?")) {
        for (int i = 0; i < n; i++)
            snprintf(out + strlen(out), sizeof out - strlen(out), "%s (read and write)\n", nd_params[i].name);
    } else {
        int i = 0;
        while (i < n && strcmp(nd_params[i].name, name))
            i++;
        if (i == n)
            return kENOENTErr;
        if (cmd == ND_SET) {
            size_t at = strlen(name) + 1;
            char val[32];
            cstr_at(buf + (uint32_t)at, val, sizeof val);
            nd_value(i);
            nd_params[i].value = (int32_t)strtol(val, NULL, 0);
            return 0;
        }
        snprintf(out, sizeof out, "%d", nd_value(i));
    }
    uint32_t want = (uint32_t)strlen(out) + 1;
    if (want > (uint32_t)len)
        want = (uint32_t)len;
    gput(buf, out, want - 1);
    wr8(buf + want - 1, 0);
    return 0;
}

static int open_stream(const char *name, int32_t *err)
{
    drv_kind kind;
    if (!strncmp(name, "/dev/", 5)) /* the Unix path to a device is its name */
        name += 5;
    if (!name[0] || !driver_known(name, &kind)) {
        LOGF(LOG_UNIMPL, "OT: no driver \"%s\" to open a stream on\n", name);
        *err = kENXIOErr;
        return -1;
    }
    LOGF(LOG_TRAPS, "OT: stream on \"%s\"\n", name);
    int i = ot_new_provider(P_STREAM);
    if (i < 0) {
        *err = kENOSRErr;
        return -1;
    }
    struct ot_stream *s = calloc(1, sizeof *s);
    if (!s) {
        ot_close_provider(&ot_providers[i]);
        *err = kENOSRErr;
        return -1;
    }
    snprintf(s->driver, sizeof s->driver, "%s", name);
    s->drv = kind;
    s->tpi = kind == D_TPI ? tpi_kind(name) : P_TCP;
    s->rdopt = RNORM | RPROTNORM;
    s->peer = -1;
    s->cltime = 15 * 1000;
    ot_providers[i].st = s;
    int ap = autopush_of(name);
    if (ap >= 0) { /* the modules set up for the device, the last pushed first under the head */
        for (int k = 0; k < autopush[ap].npush && s->nmods < MAX_MODS; k++) {
            memmove(s->mods[1], s->mods[0], sizeof s->mods[0] * (size_t)s->nmods);
            snprintf(s->mods[0], NAMESZ, "%s", autopush[ap].list[k]);
            s->nmods++;
        }
    }
    *err = kOTNoError;
    return i;
}

/* ---- The read queue ---- */

static int32_t bytes_of(const smsg *m) { return m->dlen > 0 ? m->dlen : 0; }

/* Whether the notifier is to hear of a message arriving (I_SETSIG). */
static void signal_input(provider *p, const smsg *m)
{
    int32_t want = p->st->sigs, got = 0;
    if (m->hipri)
        got |= S_HIPRI;
    else {
        got |= S_INPUT;
        got |= m->band ? S_RDBAND : S_RDNORM;
    }
    if (want & got)
        ot_post(p, kSIGNALEVENT + kSIGPOLL, want & got, 0);
}

static bool take_msg(provider *p, gaddr ctl, gaddr data, gaddr flagsp, gaddr bandp, bool any_band, int32_t *result);

/* A message for the stream head: queued (a priority one first), and an
 * asynchronous getmsg waiting for it given it. */
static void deliver(provider *p, const uint8_t *ctl, int32_t clen, const uint8_t *data, int32_t dlen, uint8_t band,
                    bool hipri)
{
    struct ot_stream *s = p->st;
    if (!s || s->nq >= MAX_QUEUE)
        return;
    smsg m = { NULL, NULL, clen, dlen, band, hipri };
    if (clen > 0 && (m.ctl = malloc((size_t)clen)) != NULL)
        memcpy(m.ctl, ctl, (size_t)clen);
    if (dlen > 0 && (m.data = malloc((size_t)dlen)) != NULL)
        memcpy(m.data, data, (size_t)dlen);
    int at = s->nq;
    if (hipri) {
        at = 0;
        while (at < s->nq && s->q[at].hipri)
            at++;
    } else if (band) {
        at = 0;
        while (at < s->nq && (s->q[at].hipri || s->q[at].band >= band))
            at++;
    }
    memmove(&s->q[at + 1], &s->q[at], sizeof s->q[0] * (size_t)(s->nq - at));
    s->q[at] = m;
    s->nq++;
    if (s->get_pending) {
        int32_t r;
        if (take_msg(p, s->get_ctl, s->get_data, s->get_flags, s->get_band, s->get_any_band, &r)) {
            s->get_pending = false;
            ot_post(p, kGetmsgEvent, r, 0);
            return;
        }
    }
    signal_input(p, &m);
}

static void drop_first(struct ot_stream *s)
{
    free_msg(&s->q[0]);
    memmove(&s->q[0], &s->q[1], sizeof s->q[0] * (size_t)--s->nq);
}

/* strbuf: maxlen, len, buf. */
static uint32_t put_part(gaddr sb, const uint8_t *bytes, int32_t len, bool *more)
{
    *more = false;
    if (!sb)
        return 0;
    int32_t maxlen = (int32_t)rd32(sb);
    if (len < 0 || maxlen < 0) {
        if (maxlen >= 0)
            wr32(sb + 4, (uint32_t)-1);
        return 0;
    }
    int32_t n = len < maxlen ? len : maxlen;
    if (n > 0 && rd32(sb + 8))
        gput(rd32(sb + 8), bytes, (uint32_t)n);
    wr32(sb + 4, (uint32_t)n);
    *more = n < len;
    return (uint32_t)n;
}

/* getmsg: the first message that fits the request into the strbufs.  A part
 * that doesn't fit stays for the next call, which MORECTL and MOREDATA say. */
static bool take_msg(provider *p, gaddr ctl, gaddr data, gaddr flagsp, gaddr bandp, bool any_band, int32_t *result)
{
    struct ot_stream *s = p->st;
    int32_t flags = flagsp ? (int32_t)rd32(flagsp) : 0;
    int k = 0;
    if (any_band) {
        uint32_t band = bandp ? rd32(bandp) : 0;
        if (flags & MSG_HIPRI) {
            if (!s->nq || !s->q[0].hipri)
                return false;
        } else if (flags & MSG_BAND) {
            while (k < s->nq && (s->q[k].hipri || s->q[k].band < band))
                k++;
            if (k >= s->nq)
                return false;
        }
    } else if (flags & RS_HIPRI) {
        if (!s->nq || !s->q[0].hipri)
            return false;
    }
    if (k >= s->nq)
        return false;
    smsg *m = &s->q[k];
    bool cmore, dmore;
    uint32_t cn = put_part(ctl, m->ctl, m->clen, &cmore);
    uint32_t dn = put_part(data, m->data, m->dlen, &dmore);
    if (flagsp)
        wr32(flagsp, any_band ? (m->hipri ? MSG_HIPRI : MSG_BAND) : (m->hipri ? RS_HIPRI : 0));
    if (bandp)
        wr32(bandp, m->band);
    *result = (cmore ? MORECTL : 0) | (dmore ? MOREDATA : 0);
    if (!cmore && !dmore) {
        smsg one = s->q[k];
        memmove(&s->q[k], &s->q[k + 1], sizeof s->q[0] * (size_t)(s->nq - k - 1));
        s->nq--;
        free_msg(&one);
        return true;
    }
    /* What's left of the message stays at the head of the queue. */
    if (cmore) {
        memmove(m->ctl, m->ctl + cn, (size_t)(m->clen - (int32_t)cn));
        m->clen -= (int32_t)cn;
    } else if (m->clen >= 0) {
        free(m->ctl);
        m->ctl = NULL;
        m->clen = -1;
    }
    if (dmore) {
        memmove(m->data, m->data + dn, (size_t)(m->dlen - (int32_t)dn));
        m->dlen -= (int32_t)dn;
    } else if (m->dlen >= 0) {
        free(m->data);
        m->data = NULL;
        m->dlen = -1;
    }
    return true;
}

/* Wait for a message in blocking mode; false if there is none (non-blocking,
 * the machine stopped, or the stream hung up with nothing left). */
static bool wait_for_msg(provider *p)
{
    while (p->used && p->st && !p->st->nq) {
        if (!p->blocking || p->st->hangup)
            return false;
        if (!ot_wait_slice(p))
            return false;
        opentransport_poll();
    }
    return p->used && p->st && p->st->nq > 0;
}

/* ---- The drivers ---- */

static void be32put(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
}

static uint32_t be32get(const uint8_t *b)
{
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

/* A transport driver's answer to a TPI request. */
static void tpi_request(provider *p, const uint8_t *ctl, int32_t clen)
{
    if (clen < 4)
        return;
    uint32_t prim = be32get(ctl);
    if (prim == T_DATA_REQ || prim == T_EXDATA_REQ) /* not connected: nowhere to go */
        return;
    if (prim == T_INFO_REQ) {
        gaddr info = tb_NewPtr(40, false, true);
        uint8_t ack[44];
        be32put(ack, T_INFO_ACK);
        if (info) {
            ot_put_info(info, p->st->tpi);
            /* TEndpointInfo: addr, options, tsdu, etsdu, connect, discon, servtype, flags */
            be32put(ack + 4, rd32(info + 8));   /* TSDU_size */
            be32put(ack + 8, rd32(info + 12));  /* ETSDU_size */
            be32put(ack + 12, rd32(info + 16)); /* CDATA_size */
            be32put(ack + 16, rd32(info + 20)); /* DDATA_size */
            be32put(ack + 20, rd32(info + 0));  /* ADDR_size */
            be32put(ack + 24, rd32(info + 4));  /* OPT_size */
            be32put(ack + 28, rd32(info + 8) ? rd32(info + 8) : 1024); /* TIDU_size */
            be32put(ack + 32, rd32(info + 24)); /* SERV_type */
            be32put(ack + 36, T_UNBND);         /* CURRENT_state */
            be32put(ack + 40, rd32(info + 28) & 0x3); /* PROVIDER_flag */
            tb_DisposePtr(info);
        }
        deliver(p, ack, sizeof ack, NULL, -1, 0, true);
        return;
    }
    uint8_t err[16];
    be32put(err, T_ERROR_ACK);
    be32put(err + 4, prim);
    be32put(err + 8, TNOTSUPPORT);
    be32put(err + 12, 0);
    deliver(p, err, sizeof err, NULL, -1, 0, true);
}

/* A message sent down the stream, as its driver takes it. */
static int32_t send_down(provider *p, const uint8_t *ctl, int32_t clen, const uint8_t *data, int32_t dlen, uint8_t band,
                         bool hipri)
{
    struct ot_stream *s = p->st;
    if (s->hangup)
        return s->drv == D_PIPE ? kEPIPEErr : kENXIOErr;
    switch (s->drv) {
    case D_ECHO: deliver(p, ctl, clen, data, dlen, band, hipri); break;
    case D_PIPE:
        if (s->peer < 0 || !ot_providers[s->peer].used)
            return kEPIPEErr;
        deliver(&ot_providers[s->peer], ctl, clen, data, dlen, band, hipri);
        break;
    case D_TPI:
        if (clen >= 0)
            tpi_request(p, ctl, clen);
        break;
    case D_NULS:
    case D_SAD:
    case D_OTHER: break;
    }
    return kOTNoError;
}

/* ---- Opening and closing ---- */

/* OTStreamOpen(name, oFlags, &err[, client]): StreamRef */
static void s_StreamOpen(ppc_cpu *c)
{
    char name[NAMESZ * 4];
    cstr_at(R(0), name, sizeof name);
    char drv[NAMESZ];
    top_name(name, drv);
    int32_t err;
    int i = open_stream(drv, &err);
    if (R(2))
        wr32(R(2), (uint32_t)err);
    RETURN(i >= 0 ? ot_providers[i].ref : 0);
}

/* OTAsyncStreamOpen(name, oFlags, proc, context[, client]): kStreamOpenEvent
 * with the StreamRef as cookie. */
static void s_AsyncStreamOpen(ppc_cpu *c)
{
    char name[NAMESZ * 4];
    cstr_at(R(0), name, sizeof name);
    char drv[NAMESZ];
    top_name(name, drv);
    int32_t err;
    int i = open_stream(drv, &err);
    if (i < 0) {
        RETURN((uint32_t)err);
        return;
    }
    provider *p = &ot_providers[i];
    p->async = true;
    p->notifier = R(2);
    p->context = R(3);
    ot_post(p, kStreamOpenEvent, kOTNoError, p->ref);
    RETURN(kOTNoError);
}

/* The driver a configuration names. */
static void config_driver(gaddr cfg, char out[NAMESZ])
{
    char path[256];
    cstr_at(cfg, path, sizeof path);
    top_name(path, out);
}

/* OTCreateStream(cfig, oFlags, &err): StreamRef; the configuration is used up. */
static void s_CreateStream(ppc_cpu *c)
{
    char drv[NAMESZ];
    config_driver(R(0), drv);
    int32_t err;
    int i = open_stream(drv, &err);
    if (R(2))
        wr32(R(2), (uint32_t)err);
    RETURN(i >= 0 ? ot_providers[i].ref : 0);
}

/* OTAsyncCreateStream(cfig, oFlags, proc, context) */
static void s_AsyncCreateStream(ppc_cpu *c)
{
    char drv[NAMESZ];
    config_driver(R(0), drv);
    int32_t err;
    int i = open_stream(drv, &err);
    if (i < 0) {
        RETURN((uint32_t)err);
        return;
    }
    provider *p = &ot_providers[i];
    p->async = true;
    p->notifier = R(2);
    p->context = R(3);
    ot_post(p, kStreamOpenEvent, kOTNoError, p->ref);
    RETURN(kOTNoError);
}

/* OTStreamPipe(StreamRef streams[2][, client]): two streams, each the
 * other's end of a pipe. */
static void s_StreamPipe(ppc_cpu *c)
{
    if (!R(0)) {
        RETURN((uint32_t)kEINVALErr);
        return;
    }
    int32_t err;
    int a = open_stream("nuls", &err);
    int b = a >= 0 ? open_stream("nuls", &err) : -1;
    if (b < 0) {
        if (a >= 0) {
            stream_free(&ot_providers[a]);
            ot_close_provider(&ot_providers[a]);
        }
        RETURN((uint32_t)err);
        return;
    }
    struct ot_stream *sa = ot_providers[a].st, *sb = ot_providers[b].st;
    snprintf(sa->driver, sizeof sa->driver, "pipe");
    snprintf(sb->driver, sizeof sb->driver, "pipe");
    sa->drv = sb->drv = D_PIPE;
    sa->peer = b;
    sb->peer = a;
    wr32(R(0), ot_providers[a].ref);
    wr32(R(0) + 4, ot_providers[b].ref);
    RETURN(kOTNoError);
}

/* OTStreamClose(strm) */
static void s_StreamClose(ppc_cpu *c)
{
    STREAM(p);
    stream_free(p);
    ot_close_provider(p);
    RETURN(kOTNoError);
}

/* ---- Modes and notifiers ---- */

static void s_SetBlocking(ppc_cpu *c) { STREAM_VOID(p); p->blocking = true; c->pc = c->lr; }
static void s_SetNonBlocking(ppc_cpu *c) { STREAM_VOID(p); p->blocking = false; c->pc = c->lr; }
static void s_IsBlocking(ppc_cpu *c) { provider *p = stream_of(R(0)); RETURN(p && p->blocking); }
static void s_SetSynchronous(ppc_cpu *c) { STREAM_VOID(p); p->async = false; c->pc = c->lr; }
static void s_SetAsynchronous(ppc_cpu *c) { STREAM_VOID(p); p->async = true; c->pc = c->lr; }
static void s_IsSynchronous(ppc_cpu *c) { provider *p = stream_of(R(0)); RETURN(p && !p->async); }

/* OTStreamInstallNotifier(strm, proc, context) */
static void s_InstallNotifier(ppc_cpu *c)
{
    STREAM(p);
    if (p->notifier) {
        RETURN((uint32_t)kOTAccessErr);
        return;
    }
    p->notifier = R(1);
    p->context = R(2);
    p->m68k = ot_from_68k;
    RETURN(kOTNoError);
}

static void s_RemoveNotifier(ppc_cpu *c)
{
    STREAM_VOID(p);
    p->notifier = 0;
    c->pc = c->lr;
}

/* OTStreamUseSyncIdleEvents(strm, useEvents) */
static void s_UseSyncIdleEvents(ppc_cpu *c)
{
    STREAM(p);
    p->st->syncidle = (R(1) & 0xFF) != 0;
    RETURN(kOTNoError);
}

/* OTStreamSetControlMask(strm, mask, setClear) */
static void s_SetControlMask(ppc_cpu *c)
{
    STREAM_VOID(p);
    if (R(2) & 0xFF)
        p->st->ctlmask |= R(1);
    else
        p->st->ctlmask &= ~R(1);
    c->pc = c->lr;
}

/* ---- Messages ---- */

/* The part a strbuf describes: its length, -1 for none (no strbuf, or len -1). */
static int32_t get_part(gaddr sb, uint8_t **out)
{
    *out = NULL;
    if (!sb)
        return -1;
    int32_t len = (int32_t)rd32(sb + 4);
    if (len < 0)
        return -1;
    if (len > 0x100000)
        return -2;
    if (len > 0) {
        *out = malloc((size_t)len);
        if (*out)
            gget(*out, rd32(sb + 8), (uint32_t)len);
    }
    return len;
}

static int32_t put_message(provider *p, gaddr ctlbuf, gaddr databuf, uint8_t band, bool hipri)
{
    uint8_t *ctl, *data;
    int32_t clen = get_part(ctlbuf, &ctl), dlen = get_part(databuf, &data);
    int32_t r;
    if (clen == -2 || dlen == -2)
        r = kERANGEErr;
    else if (hipri && clen < 0)
        r = kEINVALErr; /* a priority message needs a control part */
    else if (clen < 0 && dlen < 0)
        r = kOTNoError; /* nothing to send */
    else
        r = send_down(p, ctl, clen, data, dlen, band, hipri);
    free(ctl);
    free(data);
    return r;
}

/* OTStreamPutMessage(strm, ctlbuf, databuf, flags) */
static void s_PutMessage(ppc_cpu *c)
{
    STREAM(p);
    int32_t r = put_message(p, R(1), R(2), 0, (R(3) & RS_HIPRI) != 0);
    ot_complete(c, p, kStreamWriteEvent, r, 0);
}

/* OTStreamPutPriorityMessage(strm, ctlbuf, databuf, band, flags) */
static void s_PutPriorityMessage(ppc_cpu *c)
{
    STREAM(p);
    int32_t r = (R(4) & MSG_HIPRI) && R(3) ? kEINVALErr
                                           : put_message(p, R(1), R(2), (uint8_t)R(3), (R(4) & MSG_HIPRI) != 0);
    ot_complete(c, p, kStreamWriteEvent, r, 0);
}

static void get_message(ppc_cpu *c, provider *p, gaddr ctl, gaddr data, gaddr bandp, gaddr flagsp, bool any_band)
{
    int32_t r;
    if (take_msg(p, ctl, data, flagsp, bandp, any_band, &r)) {
        ot_complete(c, p, kGetmsgEvent, r, 0);
        return;
    }
    if (p->async && p->notifier) { /* answered when a message comes */
        struct ot_stream *s = p->st;
        s->get_pending = true;
        s->get_ctl = ctl;
        s->get_data = data;
        s->get_flags = flagsp;
        s->get_band = bandp;
        s->get_any_band = any_band;
        RETURN(kOTNoError);
        return;
    }
    if (wait_for_msg(p) && take_msg(p, ctl, data, flagsp, bandp, any_band, &r)) {
        RETURN((uint32_t)r);
        return;
    }
    RETURN((uint32_t)(p->used && p->st && p->st->hangup ? 0 : kEAGAINErr));
}

/* OTStreamGetMessage(strm, ctlbuf, databuf, &flags) */
static void s_GetMessage(ppc_cpu *c)
{
    STREAM(p);
    get_message(c, p, R(1), R(2), 0, R(3), false);
}

/* OTStreamGetPriorityMessage(strm, ctlbuf, databuf, &band, &flags) */
static void s_GetPriorityMessage(ppc_cpu *c)
{
    STREAM(p);
    get_message(c, p, R(1), R(2), R(3), R(4), true);
}

/* OTStreamRead(strm, buf, len): the stream as bytes, by the read mode. */
static void s_StreamRead(ppc_cpu *c)
{
    STREAM(p);
    struct ot_stream *s = p->st;
    gaddr buf = R(1);
    uint32_t len = R(2), got = 0;
    if (!s->nq && !wait_for_msg(p)) {
        int32_t r = s->hangup ? 0 : kEAGAINErr;
        ot_complete(c, p, kStreamReadEvent, r, 0);
        return;
    }
    while (s->nq && got < len) {
        smsg *m = &s->q[0];
        if (m->clen >= 0) {
            if (s->rdopt & RPROTDIS) {
                free(m->ctl);
                m->ctl = NULL;
                m->clen = -1;
            } else if (s->rdopt & RPROTDAT) {
                /* the control part read as data, before the data part */
                int32_t n = m->clen + bytes_of(m);
                uint8_t *both = malloc(n > 0 ? (size_t)n : 1);
                if (!both)
                    break;
                if (m->clen > 0)
                    memcpy(both, m->ctl, (size_t)m->clen);
                if (m->dlen > 0)
                    memcpy(both + m->clen, m->data, (size_t)m->dlen);
                free_msg(m);
                m->data = both;
                m->dlen = n;
                m->clen = -1;
            } else {
                if (!got) {
                    ot_complete(c, p, kStreamReadEvent, kEBADMSGErr, 0);
                    return;
                }
                break;
            }
        }
        uint32_t n = (uint32_t)bytes_of(m);
        if (n > len - got)
            n = len - got;
        if (n && buf)
            gput(buf + got, m->data, n);
        got += n;
        if ((int32_t)n < bytes_of(m) && !(s->rdopt & RMSGD)) {
            memmove(m->data, m->data + n, (size_t)(m->dlen - (int32_t)n));
            m->dlen -= (int32_t)n;
            break;
        }
        drop_first(s);
        if (s->rdopt & (RMSGD | RMSGN))
            break; /* message modes: one message per read */
    }
    ot_complete(c, p, kStreamReadEvent, (int32_t)got, 0);
}

/* OTStreamWrite(strm, buf, len): the bytes as a data message. */
static void s_StreamWrite(ppc_cpu *c)
{
    STREAM(p);
    uint32_t len = R(2);
    if (len > 0x100000) {
        ot_complete(c, p, kStreamWriteEvent, kERANGEErr, 0);
        return;
    }
    if (!len && !(p->st->wropt & SNDZERO)) {
        ot_complete(c, p, kStreamWriteEvent, 0, 0);
        return;
    }
    uint8_t *data = malloc(len ? len : 1);
    if (!data) {
        ot_complete(c, p, kStreamWriteEvent, kENOSRErr, 0);
        return;
    }
    if (len)
        gget(data, R(1), len);
    int32_t r = send_down(p, NULL, -1, data, (int32_t)len, 0, false);
    free(data);
    ot_complete(c, p, kStreamWriteEvent, r < 0 ? r : (int32_t)len, 0);
}

/* OTReadInfo: fType, fCommand, fFiller, fBytes, fError. */
static void put_read_info(gaddr ri, const smsg *m)
{
    if (!ri)
        return;
    wr32(ri, m->clen >= 0 ? (m->hipri ? M_PCPROTO : M_PROTO) : M_DATA);
    wr32(ri + 4, m->clen >= 4 ? be32get(m->ctl) : 0);
    wr32(ri + 8, 0);
    wr32(ri + 12, (uint32_t)((m->clen > 0 ? m->clen : 0) + bytes_of(m)));
    wr32(ri + 16, 0);
}

/* OTPeekMessage(strm, &readInfo): what the next message is. */
static void s_PeekMessage(ppc_cpu *c)
{
    STREAM(p);
    if (!p->st->nq) {
        RETURN((uint32_t)kOTNoDataErr);
        return;
    }
    put_read_info(R(1), &p->st->q[0]);
    RETURN(kOTNoError);
}

/* OTReadMessage(strm, &readInfo): the next message as an OTBuffer chain (the
 * control part, then the data), released with OTReleaseBuffer. */
static void s_ReadMessage(ppc_cpu *c)
{
    STREAM(p);
    struct ot_stream *s = p->st;
    if (!s->nq) {
        if (R(1))
            wr32(R(1) + 16, (uint32_t)kOTNoDataErr);
        RETURN(0);
        return;
    }
    smsg *m = &s->q[0];
    put_read_info(R(1), m);
    gaddr first = 0, last = 0;
    for (int part = 0; part < 2; part++) {
        int32_t n = part ? m->dlen : m->clen;
        if (n < 0)
            continue;
        gaddr b = ot_buffer_new(part ? m->data : m->ctl, (uint32_t)n);
        if (!b)
            break;
        wr8(b + 24, m->band);                                                    /* fBand */
        wr8(b + 25, part ? M_DATA : (m->hipri ? M_PCPROTO : M_PROTO));           /* fType */
        if (last)
            wr32(last + 8, b);
        else
            first = b;
        last = b;
    }
    drop_first(s);
    RETURN(first);
}

/* The bytes of an OTBuffer chain from (buffer, offset): fNext +8, fData
 * +12, fLen +16. */
static uint8_t *chain_bytes(gaddr b, uint32_t skip, int32_t *len)
{
    uint32_t total = 0;
    for (gaddr x = b; x; x = rd32(x + 8))
        total += rd32(x + 16);
    total = skip < total ? total - skip : 0;
    uint8_t *out = malloc(total ? total : 1);
    uint32_t at = 0;
    for (gaddr x = b; x && out; x = rd32(x + 8)) {
        uint32_t n = rd32(x + 16);
        uint32_t from = skip < n ? skip : n;
        skip -= from;
        if (n > from)
            gget(out + at, rd32(x + 12) + from, n - from);
        at += n - from;
    }
    *len = (int32_t)total;
    return out;
}

static void release_chain(gaddr b)
{
    void (*rel)(ppc_cpu *) = cfm_export_fn("OTReleaseBuffer");
    if (!rel || !b)
        return;
    ppc_cpu t = { 0 };
    t.gpr[3] = b;
    rel(&t);
}

static void put_back(provider *p, gaddr buf, uint32_t skip)
{
    int32_t len;
    uint8_t *bytes = chain_bytes(buf, skip, &len);
    struct ot_stream *s = p->st;
    if (bytes && s->nq < MAX_QUEUE) {
        uint8_t type = buf ? rd8(buf + 25) : M_DATA;
        smsg m = { NULL, NULL, -1, -1, buf ? rd8(buf + 24) : 0, type == M_PCPROTO };
        if (type == M_DATA) {
            m.data = bytes;
            m.dlen = len;
        } else {
            m.ctl = bytes;
            m.clen = len;
        }
        memmove(&s->q[1], &s->q[0], sizeof s->q[0] * (size_t)s->nq);
        s->q[0] = m;
        s->nq++;
    } else {
        free(bytes);
    }
    release_chain(buf);
}

/* OTPutBackBuffer(strm, buffer): the message read put back at the head. */
static void s_PutBackBuffer(ppc_cpu *c)
{
    STREAM_VOID(p);
    put_back(p, R(1), 0);
    c->pc = c->lr;
}

/* OTPutBackPartialBuffer(strm, OTBufferInfo *, buffer): what's left of it
 * from where the OTBufferInfo (fBuffer, fOffset) has got to. */
static void s_PutBackPartialBuffer(ppc_cpu *c)
{
    STREAM_VOID(p);
    gaddr info = R(1), buf = R(2);
    uint32_t skip = 0;
    if (info) {
        /* bytes in the buffers before the current one, then its offset */
        gaddr cur = rd32(info);
        for (gaddr x = buf; x && x != cur; x = rd32(x + 8))
            skip += rd32(x + 16);
        skip += rd32(info + 4);
    }
    put_back(p, buf, skip);
    c->pc = c->lr;
}

/* ---- Polling ---- */

static uint16_t poll_events(provider *p, uint16_t want)
{
    if (!p)
        return POLLNVAL;
    struct ot_stream *s = p->st;
    uint16_t got = POLLOUT; /* never flow-controlled */
    if (s->nq) {
        const smsg *m = &s->q[0];
        got |= m->hipri ? POLLPRI : (m->band ? POLLRDBAND : (POLLIN | POLLRDNORM));
        if (m->band && !m->hipri)
            got |= POLLIN;
    }
    if (s->hangup)
        got = (uint16_t)((got & ~POLLOUT) | POLLHUP);
    if (s->drv != D_PIPE || s->peer >= 0)
        got |= POLLWRBAND & want;
    return (uint16_t)(got & (want | POLLHUP | POLLERR | POLLNVAL));
}

/* PollRef: filler (4), events (2), revents (2), ref (4). */
static int32_t poll_once(gaddr fds, uint32_t n)
{
    int32_t ready = 0;
    for (uint32_t i = 0; i < n; i++) {
        gaddr pr = fds + 12 * i;
        gaddr ref = rd32(pr + 8);
        uint16_t got = ref ? poll_events(stream_of(ref), rd16(pr + 4)) : 0;
        wr16(pr + 6, got);
        if (got)
            ready++;
    }
    return ready;
}

/* OTStreamPoll(fds, nfds, timeout ms; -1 forever): the number ready. */
static void s_StreamPoll(ppc_cpu *c)
{
    gaddr fds = R(0);
    uint32_t n = R(1);
    int32_t timeout = (int32_t)R(2);
    if (!fds || n > 1024) {
        RETURN((uint32_t)kEINVALErr);
        return;
    }
    uint32_t start = rd32(LM_Ticks);
    int32_t ready;
    while (!(ready = poll_once(fds, n)) && timeout != 0 && !g_cpu->halted) {
        if (timeout > 0 && (rd32(LM_Ticks) - start) * 50 / 3 >= (uint32_t)timeout)
            break;
        machine_pass_tick();
        opentransport_poll();
    }
    RETURN((uint32_t)ready);
}

/* OTAsyncStreamPoll(fds, nfds, timeout, proc, context): kPollEvent with
 * what's ready now. */
static void s_AsyncStreamPoll(ppc_cpu *c)
{
    gaddr fds = R(0);
    uint32_t n = R(1);
    if (!fds || n > 1024 || !n || !R(3)) {
        RETURN((uint32_t)kEINVALErr);
        return;
    }
    provider *p = stream_of(rd32(fds + 8));
    if (!p) {
        RETURN((uint32_t)kEBADFErr);
        return;
    }
    int32_t ready = poll_once(fds, n);
    if (!p->notifier) {
        p->notifier = R(3);
        p->context = R(4);
        p->m68k = ot_from_68k;
    }
    ot_post(p, kPollEvent, ready, fds);
    RETURN(kOTNoError);
}

/* ---- ioctl ---- */

static int find_module(struct ot_stream *s, const char *name)
{
    for (int i = 0; i < s->nmods; i++)
        if (!strcmp(s->mods[i], name))
            return i;
    return -1;
}

static void flush(struct ot_stream *s, int32_t how, int band, bool by_band)
{
    if (!(how & FLUSHR))
        return;
    for (int i = 0; i < s->nq;) {
        if (by_band && (s->q[i].hipri || s->q[i].band != band)) {
            i++;
            continue;
        }
        free_msg(&s->q[i]);
        memmove(&s->q[i], &s->q[i + 1], sizeof s->q[0] * (size_t)(s->nq - i - 1));
        s->nq--;
    }
}

/* One STREAMS head ioctl: its result, or a negative error. */
static int32_t stream_ioctl(provider *p, uint32_t cmd, gaddr data)
{
    struct ot_stream *s = p->st;
    char name[NAMESZ];
    switch (cmd) {
    case I_NREAD:
        if (data)
            wr32(data, s->nq ? (uint32_t)bytes_of(&s->q[0]) : 0);
        return s->nq;
    case I_PUSH:
        cstr_at(data, name, sizeof name);
        if (!in_list(known_modules, name))
            return kEINVALErr;
        if (s->nmods >= MAX_MODS)
            return kEINVALErr;
        memmove(s->mods[1], s->mods[0], sizeof s->mods[0] * (size_t)s->nmods);
        snprintf(s->mods[0], NAMESZ, "%s", name);
        s->nmods++;
        return 0;
    case I_POP:
        if (!s->nmods)
            return kEINVALErr;
        memmove(s->mods[0], s->mods[1], sizeof s->mods[0] * (size_t)--s->nmods);
        return 0;
    case I_LOOK:
        if (!s->nmods)
            return kEINVALErr;
        if (data)
            gput(data, s->mods[0], (uint32_t)strlen(s->mods[0]) + 1);
        return 0;
    case I_FIND:
        cstr_at(data, name, sizeof name);
        if (!name[0])
            return kEINVALErr;
        return find_module(s, name) >= 0;
    case I_LIST: {
        /* str_list: sl_nmods, sl_modlist -> str_mlist[] of 32-byte names,
         * the modules then the driver */
        int32_t total = s->nmods + 1;
        if (!data)
            return total;
        int32_t room = (int32_t)rd32(data);
        gaddr list = rd32(data + 4);
        if (room <= 0 || !list)
            return kEINVALErr;
        int32_t n = total < room ? total : room;
        for (int32_t i = 0; i < n; i++) {
            const char *nm = i < s->nmods ? s->mods[i] : s->driver;
            gmemset(list + 32 * (uint32_t)i, 0, 32);
            gput(list + 32 * (uint32_t)i, nm, (uint32_t)strlen(nm));
        }
        wr32(data, (uint32_t)n);
        return n < total ? 1 : 0;
    }
    case I_FLUSH:
        if (data & ~(uint32_t)(FLUSHR | FLUSHW))
            return kEINVALErr;
        flush(s, (int32_t)data, 0, false);
        return 0;
    case I_FLUSHBAND: /* bandinfo: bi_pri (byte), bi_flag (long at +4) */
        if (!data)
            return kEINVALErr;
        flush(s, (int32_t)rd32(data + 4), rd8(data), true);
        return 0;
    case I_SRDOPT:
        if ((data & 0x07) && (data & 0x07) != RNORM && (data & 0x07) != RMSGD && (data & 0x07) != RMSGN)
            return kEINVALErr;
        s->rdopt = (int32_t)((data & 0x07 ? data & 0x07 : (uint32_t)(s->rdopt & 0x07)) |
                             (data & 0x70 ? data & 0x70 : (uint32_t)(s->rdopt & 0x70)));
        return 0;
    case I_GRDOPT:
        if (data)
            wr32(data, (uint32_t)s->rdopt);
        return 0;
    case I_SWROPT:
        s->wropt = (int32_t)data;
        return 0;
    case I_GWROPT:
        if (data)
            wr32(data, (uint32_t)s->wropt);
        return 0;
    case I_SETSIG:
        s->sigs = (int32_t)data;
        return 0;
    case I_GETSIG:
        if (!s->sigs)
            return kEINVALErr;
        if (data)
            wr32(data, (uint32_t)s->sigs);
        return 0;
    case I_PEEK: { /* strpeek: ctlbuf, databuf, flags */
        if (!data)
            return kEINVALErr;
        if (!s->nq || ((rd32(data + 24) & RS_HIPRI) && !s->q[0].hipri))
            return 0;
        smsg *m = &s->q[0];
        bool more;
        put_part(data, m->ctl, m->clen, &more);
        put_part(data + 12, m->data, m->dlen, &more);
        wr32(data + 24, m->hipri ? RS_HIPRI : 0);
        return 1;
    }
    case I_ATMARK:
        if (data != ANYMARK && data != LASTMARK)
            return kEINVALErr;
        return 0; /* nothing here marks messages */
    case I_CKBAND:
        for (int i = 0; i < s->nq; i++)
            if (!s->q[i].hipri && s->q[i].band == (uint8_t)data)
                return 1;
        return 0;
    case I_GETBAND:
        if (!s->nq)
            return kENODATAErr;
        if (data)
            wr32(data, s->q[0].band);
        return 0;
    case I_CANPUT:
        if (data > 255)
            return kEINVALErr;
        return !s->hangup;
    case I_SETCLTIME:
        if (!data)
            return kEINVALErr;
        s->cltime = (int32_t)rd32(data);
        return 0;
    case I_GETCLTIME:
        if (data)
            wr32(data, (uint32_t)s->cltime);
        return 0;
    case I_SETDELAY: /* blocking status */
        p->blocking = data != 0;
        return 0;
    case I_GETDELAY:
        return p->blocking;
    case I_RUN_QUEUES:
        opentransport_poll();
        return 0;
    case I_OTGetMiscellaneousEvents:
        s->misc_events = data != 0;
        return 0;
    case I_STR: { /* strioctl: ic_cmd, ic_timout, ic_len, ic_dp */
        if (!data)
            return kEINVALErr;
        uint32_t sub = rd32(data);
        /* a head ioctl sent as I_STR is still the head's */
        if ((sub >> 8) == 'A' && sub != I_STR)
            return stream_ioctl(p, sub, rd32(data + 12));
        if (s->drv == D_SAD && (sub >> 8) == 'g')
            return sad_ioctl(sub, rd32(data + 12));
        if (sub == ND_GET || sub == ND_SET)
            return nd_ioctl(s, sub, rd32(data + 12), (int32_t)rd32(data + 8));
        return kEINVALErr; /* no module or driver here takes it */
    }
    case I_SAD_SAP:
    case I_SAD_GAP:
    case I_SAD_VML: return s->drv == D_SAD ? sad_ioctl(cmd, data) : kEINVALErr;
    case I_LINK:
    case I_PLINK:
    case I_UNLINK:
    case I_PUNLINK: return kEINVALErr; /* no multiplexing driver */
    case I_SENDFD:
    case I_RECVFD:
    case I_FDINSERT: return s->drv == D_PIPE ? kEINVALErr : kENXIOErr;
    default: return kEINVALErr;
    }
}

/* OTStreamIoctl(strm, cmd, data) */
static void s_StreamIoctl(ppc_cpu *c)
{
    STREAM(p);
    int32_t r = stream_ioctl(p, R(1), R(2));
    if (R(1) == I_PUSH || R(1) == I_FIND) {
        char m[NAMESZ];
        cstr_at(R(2), m, sizeof m);
        LOGF(LOG_TRAPS, "OT: ioctl %04X \"%s\" on \"%s\" -> %d\n", R(1), m, p->st->driver, r);
    } else if (R(1) == I_STR && R(2)) {
        char arg[40];
        cstr_at(rd32(R(2) + 12), arg, sizeof arg);
        LOGF(LOG_TRAPS, "OT: ioctl I_STR %04X (%d bytes, \"%s\") on \"%s\" -> %d\n", rd32(R(2)), rd32(R(2) + 8), arg,
             p->st->driver, r);
    } else {
        LOGF(LOG_TRAPS, "OT: ioctl %04X on \"%s\" -> %d\n", R(1), p->st->driver, r);
    }
    if (p->async)
        ot_post(p, kStreamIoctlEvent, r, R(2));
    RETURN(p->async ? kOTNoError : (uint32_t)r);
}

/* OTIoctl on a stream (opentransport.c hands streams on to here). */
int32_t ot_stream_ioctl(provider *p, uint32_t cmd, gaddr data) { return stream_ioctl(p, cmd, data); }

/* ---- Streams and providers ---- */

static int32_t to_endpoint(provider *p, gaddr *ref)
{
    *ref = 0;
    struct ot_stream *s = p->st;
    if (s->drv != D_TPI)
        return kOTBadConfigurationErr;
    gaddr cfg = tb_NewPtr(NAMESZ, false, true);
    if (!cfg)
        return kOTOutOfMemoryErr;
    gput(cfg, s->driver, (uint32_t)strlen(s->driver));
    void (*open)(ppc_cpu *) = cfm_export_fn("OTOpenEndpoint");
    gaddr errp = tb_NewPtr(4, false, true);
    ppc_cpu t = { 0 };
    t.gpr[3] = cfg;
    t.gpr[6] = errp;
    if (open && errp)
        open(&t);
    int32_t err = errp ? (int32_t)rd32(errp) : kOTOutOfMemoryErr;
    if (errp)
        tb_DisposePtr(errp);
    tb_DisposePtr(cfg);
    if (err == kOTNoError && t.gpr[3]) {
        *ref = t.gpr[3];
        provider *e = ot_provider_of(*ref);
        if (e) {
            e->async = p->async;
            e->blocking = p->blocking;
            e->notifier = p->notifier;
            e->context = p->context;
            e->m68k = p->m68k;
        }
        stream_free(p);
        ot_close_provider(p);
    }
    return err;
}

/* OTOpenEndpointOnStream(strm, &err[, client]) and OTOpenProviderOnStream:
 * the stream becomes an endpoint of its transport. */
static void s_OpenEndpointOnStream(ppc_cpu *c)
{
    provider *p = stream_of(R(0));
    gaddr ref = 0;
    int32_t err = p ? to_endpoint(p, &ref) : kOTBadReferenceErr;
    if (R(1))
        wr32(R(1), (uint32_t)err);
    RETURN(ref);
}

/* OTRemoveStreamFromProvider(ref): the provider's stream, as a raw stream. */
static void s_RemoveStreamFromProvider(ppc_cpu *c)
{
    provider *e = ot_provider_of(R(0));
    if (!e || e->kind == P_STREAM) {
        RETURN(e ? R(0) : 0);
        return;
    }
    static const char *const names[] = { [P_TCP] = "tcp", [P_UDP] = "udp", [P_INET_SERVICES] = "dnr",
                                         [P_STREAM] = "", [P_DDP] = "ddp", [P_ATP] = "atp", [P_ADSP] = "adsp",
                                         [P_AT_SERVICES] = "atlk", [P_NBP_MAPPER] = "nbp", [P_PAP] = "pap",
                                         [P_ASP] = "asp" };
    int32_t err;
    int i = open_stream(names[e->kind], &err);
    if (i < 0) {
        RETURN(0);
        return;
    }
    provider *p = &ot_providers[i];
    p->async = e->async;
    p->blocking = e->blocking;
    p->notifier = e->notifier;
    p->context = e->context;
    p->m68k = e->m68k;
    ot_close_provider(e);
    RETURN(p->ref);
}

/* OTCloseProviderByStream(strm, reason, doneDeal): the stream told it will
 * close, and closed. */
static void s_CloseProviderByStream(ppc_cpu *c)
{
    provider *p = ot_provider_of(R(0));
    if (p) {
        ot_post(p, 0x24000002 /* kOTProviderIsClosed */, (int32_t)R(1), 0);
        opentransport_poll();
        if (p->used) {
            stream_free(p);
            ot_close_provider(p);
        }
    }
    c->pc = c->lr;
}

/* OTCloseProvidersByUseCount(&useCount, reason, doneDeal) and
 * OTCloseProvidersByPortRef(port, reason, doneDeal): the program's
 * providers are its own to close; the use count is left as it is. */
static void s_CloseProvidersByUseCount(ppc_cpu *c) { c->pc = c->lr; }

/* OTCloseMatchingProviders(mask, port, reason, doneDeal) */
static void s_CloseMatchingProviders(ppc_cpu *c) { c->pc = c->lr; }

/* OTYieldPortRequest(ref, portRef, OTClientList *, size): no other client
 * holds the port, so none refuses. */
static void s_YieldPortRequest(ppc_cpu *c)
{
    if (R(2) && R(3) >= 4)
        wr32(R(2), 0);
    RETURN(kOTNoError);
}

/* ---- State machines ----
 * An OTStateMachine is the program's (fData, fCookie, fCode, fResult);
 * ours is kept beside it: the routine and state to call back, a stack of
 * them, the completion routine and the program's buffer.  A call the
 * machine makes (OTSMCreateStream, OTSMIoctl ...) completes at once and
 * calls the state routine back with its event, as the asynchronous
 * provider would have at deferred task time. */

enum { MAX_SM = 16, SM_DEPTH = 8 };
typedef struct {
    gaddr sm, buf;
    gaddr proc[SM_DEPTH];
    uint16_t state[SM_DEPTH];
    int depth;
    gaddr complete, complete_ctx, notify, notify_ctx;
    bool done, m68k;
} sm_rec;
static sm_rec sms[MAX_SM];

static int sm_index(gaddr sm)
{
    for (int i = 0; i < MAX_SM; i++)
        if (sms[i].sm && sms[i].sm == sm)
            return i;
    return -1;
}

/* The fields after fData: 8 bytes of it on 68k, 12 on PowerPC. */
static gaddr sm_fields(int i) { return sms[i].sm + (sms[i].m68k ? 8 : 12); }

static void sm_call(int i)
{
    if (i < 0 || !sms[i].depth)
        return;
    gaddr proc = sms[i].proc[sms[i].depth - 1];
    uint32_t arg = sms[i].sm;
    /* pascal void (*OTStateProcPtr)(OTStateMachine *) on 68k */
    ot_callproc_as(sms[i].m68k, proc, 0x000000C0, &arg, 1);
}

/* A call the machine made has finished: its event goes to the state routine. */
static void sm_event(int i, uint32_t code, int32_t result, gaddr cookie)
{
    gaddr f = sm_fields(i);
    wr32(f, cookie);
    wr32(f + 4, code);
    wr32(f + 8, (uint32_t)result);
    sm_call(i);
}

/* OTCreateStateMachine(buf, bufSize, structSize, proc, context) */
static void s_CreateStateMachine(ppc_cpu *c)
{
    gaddr buf = R(0);
    uint32_t size = R(1), need = R(2);
    if (!buf || size < (need ? need : 20)) {
        RETURN(0);
        return;
    }
    int i = 0;
    while (i < MAX_SM && sms[i].sm)
        i++;
    if (i == MAX_SM) {
        RETURN(0);
        return;
    }
    gmemset(buf, 0, need ? need : 20);
    sms[i] = (sm_rec){ .sm = buf, .buf = buf, .notify = R(3), .notify_ctx = R(4), .m68k = ot_from_68k };
    RETURN(buf);
}

static void s_DestroyStateMachine(ppc_cpu *c)
{
    int i = sm_index(R(0));
    if (i >= 0)
        sms[i].sm = 0;
    c->pc = c->lr;
}

/* OTSMCallStateProc(sm, proc, state): the routine pushed and called. */
static void s_SMCallStateProc(ppc_cpu *c)
{
    int i = sm_index(R(0));
    if (i < 0 || sms[i].depth >= SM_DEPTH) {
        RETURN(false);
        return;
    }
    sms[i].proc[sms[i].depth] = R(1);
    sms[i].state[sms[i].depth] = (uint16_t)R(2);
    sms[i].depth++;
    sms[i].done = false;
    sm_call(i);
    RETURN(true);
}

static void s_SMGetState(ppc_cpu *c)
{
    int i = sm_index(R(0));
    RETURN(i >= 0 && sms[i].depth ? sms[i].state[sms[i].depth - 1] : 0);
}

static void s_SMSetState(ppc_cpu *c)
{
    int i = sm_index(R(0));
    if (i >= 0 && sms[i].depth)
        sms[i].state[sms[i].depth - 1] = (uint16_t)R(1);
    c->pc = c->lr;
}

/* OTSMPopCallback(sm): back to the routine that called this one. */
static void s_SMPopCallback(ppc_cpu *c)
{
    int i = sm_index(R(0));
    if (i >= 0 && sms[i].depth)
        sms[i].depth--;
    c->pc = c->lr;
}

/* OTSMComplete(sm): this routine is done; the one under it hears of it, or
 * at the top the completion routine. */
static void s_SMComplete(ppc_cpu *c)
{
    int i = sm_index(R(0));
    c->pc = c->lr;
    if (i < 0 || !sms[i].depth)
        return;
    sms[i].depth--;
    if (sms[i].depth) {
        sm_call(i);
        return;
    }
    sms[i].done = true;
    if (sms[i].complete) {
        uint32_t arg = sms[i].complete_ctx;
        ot_callproc_as(sms[i].m68k, sms[i].complete, 0x000000C1, &arg, 1);
    }
}

static void s_SMWaitForComplete(ppc_cpu *c)
{
    int i = sm_index(R(0));
    RETURN(i < 0 || sms[i].done || !sms[i].depth);
}

static void s_SMReturnToCaller(ppc_cpu *c)
{
    int i = sm_index(R(0));
    RETURN(i >= 0 ? rd32(sm_fields(i) + 8) : (uint32_t)kOTBadReferenceErr);
}

static void s_SMGetClientData(ppc_cpu *c)
{
    int i = sm_index(R(0));
    RETURN(i >= 0 ? sms[i].notify_ctx : 0);
}

/* OTSMInstallCompletionProc(sm, proc, context) */
static void s_SMInstallCompletionProc(ppc_cpu *c)
{
    int i = sm_index(R(0));
    if (i >= 0) {
        sms[i].complete = R(1);
        sms[i].complete_ctx = R(2);
    }
    c->pc = c->lr;
}

/* The machine's calls: each made as its own export is, then the event. */
static void sm_do(ppc_cpu *c, const char *call, uint32_t event, int nargs, bool cookie_is_result)
{
    int i = sm_index(R(0));
    if (i < 0) {
        RETURN(false);
        return;
    }
    void (*fn)(ppc_cpu *) = cfm_export_fn(call);
    ppc_cpu t = { 0 };
    for (int k = 0; k < nargs; k++)
        t.gpr[3 + k] = c->gpr[4 + k];
    gaddr errp = tb_NewPtr(4, false, true);
    if (cookie_is_result)
        t.gpr[3 + nargs] = errp; /* the &err an open takes */
    bool saved = ot_from_68k;
    ot_from_68k = sms[i].m68k;
    if (fn)
        fn(&t);
    ot_from_68k = saved;
    int32_t result = cookie_is_result ? (errp ? (int32_t)rd32(errp) : kOTOutOfMemoryErr) : (int32_t)t.gpr[3];
    gaddr cookie = cookie_is_result ? t.gpr[3] : (nargs > 2 ? c->gpr[6] : 0);
    if (errp)
        tb_DisposePtr(errp);
    c->gpr[3] = true; /* in progress: the state routine hears */
    c->pc = c->lr;
    sm_event(i, event, result, cookie);
}

/* OTSMOpenStream(sm, name, flags) */
static void s_SMOpenStream(ppc_cpu *c) { sm_do(c, "OTStreamOpen", kStreamOpenEvent, 2, true); }
/* OTSMCreateStream(sm, cfig, flags) */
static void s_SMCreateStream(ppc_cpu *c) { sm_do(c, "OTCreateStream", kStreamOpenEvent, 2, true); }
/* OTSMCreateControlStream(sm, cfig, cfigor) */
static void s_SMCreateControlStream(ppc_cpu *c)
{
    c->gpr[5] = 0; /* no open flags in place of the configurator */
    sm_do(c, "OTCreateStream", kStreamOpenEvent, 2, true);
}
/* OTSMIoctl(sm, strm, cmd, data) */
static void s_SMIoctl(ppc_cpu *c) { sm_do(c, "OTStreamIoctl", kStreamIoctlEvent, 3, false); }
/* OTSMPutMessage(sm, strm, ctl, data, flags) */
static void s_SMPutMessage(ppc_cpu *c) { sm_do(c, "OTStreamPutMessage", kStreamWriteEvent, 4, false); }
/* OTSMGetMessage(sm, strm, ctl, data, &flags) */
static void s_SMGetMessage(ppc_cpu *c) { sm_do(c, "OTStreamGetMessage", kGetmsgEvent, 4, false); }

void ot_streams_reset(void)
{
    memset(sms, 0, sizeof sms);
    memset(autopush, 0, sizeof autopush);
    for (size_t i = 0; i < sizeof nd_params / sizeof nd_params[0]; i++)
        nd_params[i].set = false;
}

/* ---- The rest of OTNativeClientLib ---- */

/* OTNotifyUser(FSSpec *, rsrcID, index, parm1, parm2): a notice for the
 * user from the file's STR# resource.  There is no one to read it here but
 * the log. */
static void s_NotifyUser(ppc_cpu *c)
{
    char a[64], b[64];
    cstr_at(R(3), a, sizeof a);
    cstr_at(R(4), b, sizeof b);
    LOGF(LOG_TRAPS, "OT: notify user (STR# %d item %u: %s %s)\n", (int)R(1), R(2), a, b);
    RETURN(kOTNoError);
}

/* StoreIntoNetbuf(TNetbuf *, source, len): the bytes into the netbuf as
 * far as maxlen allows; true if all of them fit. */
static void s_StoreIntoNetbuf(ppc_cpu *c)
{
    gaddr nb = R(0);
    uint32_t len = R(2);
    if (!nb) {
        RETURN(false);
        return;
    }
    uint32_t room = rd32(nb), n = len < room ? len : room;
    if (n && R(1) && rd32(nb + 8))
        gmemcpy(rd32(nb + 8), R(1), n);
    wr32(nb + 4, n);
    RETURN(n == len);
}

static void s_void(ppc_cpu *c) { c->pc = c->lr; }
static void s_noErr(ppc_cpu *c) { RETURN(kOTNoError); }

/* OTNativeIdle / OTNativeDelay(seconds): as OTIdle and OTDelay. */
static void s_NativeIdle(ppc_cpu *c)
{
    opentransport_poll();
    c->pc = c->lr;
}

static void s_NativeDelay(ppc_cpu *c)
{
    void (*fn)(ppc_cpu *) = cfm_export_fn("OTDelay");
    if (fn)
        fn(c);
    else
        c->pc = c->lr;
}

const ppc_export otst_exports[] = {
    { "OTStreamOpen", s_StreamOpen }, { "OTStreamOpenPriv", s_StreamOpen },
    { "OTAsyncStreamOpen", s_AsyncStreamOpen }, { "OTAsyncStreamOpenPriv", s_AsyncStreamOpen },
    { "OTCreateStream", s_CreateStream }, { "OTAsyncCreateStream", s_AsyncCreateStream },
    { "OTStreamPipe", s_StreamPipe }, { "OTStreamPipePriv", s_StreamPipe },
    { "OTStreamClose", s_StreamClose }, { "OTStreamPoll", s_StreamPoll }, { "OTAsyncStreamPoll", s_AsyncStreamPoll },
    { "OTStreamRead", s_StreamRead }, { "OTStreamWrite", s_StreamWrite }, { "OTStreamIoctl", s_StreamIoctl },
    { "OTStreamSetBlocking", s_SetBlocking }, { "OTStreamSetNonBlocking", s_SetNonBlocking },
    { "OTStreamIsBlocking", s_IsBlocking }, { "OTStreamSetSynchronous", s_SetSynchronous },
    { "OTStreamSetAsynchronous", s_SetAsynchronous }, { "OTStreamIsSynchronous", s_IsSynchronous },
    { "OTStreamGetMessage", s_GetMessage }, { "OTStreamGetPriorityMessage", s_GetPriorityMessage },
    { "OTStreamPutMessage", s_PutMessage }, { "OTStreamPutPriorityMessage", s_PutPriorityMessage },
    { "OTStreamInstallNotifier", s_InstallNotifier }, { "OTStreamRemoveNotifier", s_RemoveNotifier },
    { "OTStreamUseSyncIdleEvents", s_UseSyncIdleEvents }, { "OTStreamSetControlMask", s_SetControlMask },
    { "OTPeekMessage", s_PeekMessage }, { "OTReadMessage", s_ReadMessage }, { "OTPutBackBuffer", s_PutBackBuffer },
    { "OTPutBackPartialBuffer", s_PutBackPartialBuffer },
    { "OTOpenEndpointOnStream", s_OpenEndpointOnStream }, { "OTOpenEndpointOnStreamPriv", s_OpenEndpointOnStream },
    { "OTOpenProviderOnStream", s_OpenEndpointOnStream }, { "OTOpenProviderOnStreamPriv", s_OpenEndpointOnStream },
    { "OTRemoveStreamFromProvider", s_RemoveStreamFromProvider },
    { "OTCloseProviderByStream", s_CloseProviderByStream },
    { "OTCloseProvidersByUseCount", s_CloseProvidersByUseCount },
    { "OTCloseProvidersByPortRef", s_CloseProvidersByUseCount },
    { "OTCloseMatchingProviders", s_CloseMatchingProviders },
    { "OTYieldPortRequest", s_YieldPortRequest },
    { "OTCreateStateMachine", s_CreateStateMachine }, { "OTDestroyStateMachine", s_DestroyStateMachine },
    { "OTSMCallStateProc", s_SMCallStateProc }, { "OTSMGetState", s_SMGetState }, { "OTSMSetState", s_SMSetState },
    { "OTSMComplete", s_SMComplete }, { "OTSMPopCallback", s_SMPopCallback },
    { "OTSMWaitForComplete", s_SMWaitForComplete }, { "OTSMReturnToCaller", s_SMReturnToCaller },
    { "OTSMGetClientData", s_SMGetClientData }, { "OTSMInstallCompletionProc", s_SMInstallCompletionProc },
    { "OTSMOpenStream", s_SMOpenStream }, { "OTSMCreateStream", s_SMCreateStream },
    { "OTSMCreateControlStream", s_SMCreateControlStream }, { "OTSMIoctl", s_SMIoctl },
    { "OTSMPutMessage", s_SMPutMessage }, { "OTSMGetMessage", s_SMGetMessage },
    { "OTNotifyUser", s_NotifyUser }, { "OTNativeIdle", s_NativeIdle }, { "OTNativeDelay", s_NativeDelay },
    { "OTSetServerMode", s_void }, { "OTPowerUpdate", s_void }, { "OTSetMemoryLimits", s_noErr },
    { "OTTryClientShutdown", s_noErr }, { "StoreIntoNetbuf", s_StoreIntoNetbuf },
};
const unsigned otst_export_count = sizeof otst_exports / sizeof otst_exports[0];
