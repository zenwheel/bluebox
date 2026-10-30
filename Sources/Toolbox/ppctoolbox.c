/*
 * The PPC Toolbox (_PPC, $A0DD, the routine selector in D0): program-to-
 * program sessions between ports, within a run and between Blue Box runs,
 * on the transport Apple Events between runs use (aem_remote.c): Unix
 * sockets in a folder kept per user and home folder, here
 * $TMPDIR/bluebox-ppc-XXXXXXXX.  The runs on this host are one machine:
 * a port's name is unique among them all, and their sessions are local
 * ones (no authentication, no user name).  Ports on other machines (an
 * NBP location) can't be reached.
 *
 * Each open port listens on a socket ("p<pid>.<refnum>") beside a file of
 * its PPCPortRec ("...name") for PPCStart and IPCListPorts to find.  A
 * session is a stream connection: the starter sends its port's name and
 * the userData ('S'); the other side's PPCInform takes the request and
 * accepts it ('A') or rejects it ('R', the rejectInfo).  Then each block
 * written is one or more fragments ('D': more, userData, creator, type,
 * data); a read takes the current block's bytes until its buffer is full
 * or the block ends.  Ending a session closes the connection, and the
 * other side's calls on it find sessClosedErr.
 *
 * Calls that wait (PPCInform, PPCStart, PPCRead) do so in the background
 * when asynchronous, their completion routines run at interrupt time (the
 * parameter block on the stack and in A0, the result in D0); synchronous
 * ones wait for their answer, interrupt-time tasks running meanwhile.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "legacy.h"
#include "tb_internal.h"

enum {
    noPortErr = -903, destPortErr = -906, sessTableErr = -907, noSessionErr = -908, badReqErr = -909,
    portNameExistsErr = -910, userRejectErr = -912, noResponseErr = -915, portClosedErr = -916,
    sessClosedErr = -917, badPortNameErr = -919, noUserRefErr = -924, badServiceMethodErr = -930,
};
enum { ppcServiceRealTime = 1, ppcLocalOrigin = 1, ppcNoLocation = 0 };
/* the parameter blocks' fields */
enum {
    P_COMPLETION = 12, P_RESULT = 16, P_PORT = 38, P_SESS = 40, P_SERVICE = 44, P_AUTO = 45, P_NAME = 46,
    P_LOCATION = 50, P_USERNAME = 54, P_REJECT = 54, P_USERDATA = 58, P_REQTYPE = 62, P_VISIBLE = 54,
    P_REGISTERED = 55, P_LEN = 44, P_ACTUAL = 48, P_BUF = 52, P_MORE = 56, P_CREATOR = 62, P_TYPE = 66,
    P_START = 40, P_REQCOUNT = 42, P_ACTCOUNT = 44, P_LISTBUF = 54,
};
enum { NAME = PPCPortRec_SIZEOF, MAX_PORTS = 16, MAX_SESSIONS = 32, MAX_WAIT = 64 };

/* ---------------------------------------------------------------------- */
/* State */

typedef struct {
    bool used;
    uint16_t refnum;
    uint8_t name[NAME];
    int sock;
    char path[600];
    /* connections the listener has taken, for PPCInform */
    int pending[8];
    int npending;
} port;

typedef struct {
    bool used, accepted;
    uint32_t refnum;
    uint16_t port;
    int fd;
    bool closed; /* the other end has gone */
    /* bytes received, not yet read */
    uint8_t *in;
    size_t nin, cap;
    /* the block being read: what's left of the current fragment */
    bool in_block;
} session;

typedef enum { W_INFORM, W_START, W_READ } wait_kind;
typedef struct {
    gaddr pb;
    wait_kind kind;
    bool async;
} waiting;

static port ports[MAX_PORTS];
static session sessions[MAX_SESSIONS];
static waiting waits[MAX_WAIT];
static int nwaits;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint16_t next_port = 1;
static uint32_t next_sess = 1;
/* completions to run at interrupt time */
typedef struct {
    gaddr pb;
    int16_t result;
} completion;
static completion done[MAX_WAIT];
static int ndone;

static const char *folder(void)
{
    static char path[512];
    if (path[0])
        return path;
    const char *tmp = getenv("TMPDIR"), *home = getenv("HOME");
    if (!tmp || !*tmp)
        tmp = "/tmp/";
    uint32_t h = 2166136261u; /* FNV-1a of the home folder, as aem_remote.c's */
    for (const char *p = home ? home : ""; *p; p++)
        h = (h ^ (uint8_t)*p) * 16777619u;
    bool slash = tmp[strlen(tmp) - 1] == '/';
    snprintf(path, sizeof path, "%s%sbluebox-ppc-%08x", tmp, slash ? "" : "/", h);
    mkdir(path, 0700);
    return path;
}

/* ---------------------------------------------------------------------- */
/* Messages: a type byte, a 4-byte length, the payload */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static bool write_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && (errno == EINTR || errno == EAGAIN)) {
            struct pollfd pf = { fd, POLLOUT, 0 };
            poll(&pf, 1, 100);
            continue;
        }
        if (k <= 0)
            return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

static bool send_msg(int fd, uint8_t type, const uint8_t *data, uint32_t len)
{
    uint8_t head[5] = { type };
    put32(head + 1, len);
    return write_all(fd, head, 5) && (!len || write_all(fd, data, len));
}

/* Whatever has arrived on a session, kept; false once the other end has gone. */
static void receive(session *s)
{
    if (s->closed)
        return;
    for (;;) {
        if (s->cap - s->nin < 4096) {
            size_t cap = s->cap ? s->cap * 2 : 8192;
            uint8_t *p = realloc(s->in, cap);
            if (!p)
                return;
            s->in = p;
            s->cap = cap;
        }
        ssize_t k = recv(s->fd, s->in + s->nin, s->cap - s->nin, MSG_DONTWAIT);
        if (k > 0) {
            s->nin += (size_t)k;
            continue;
        }
        if (k == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            s->closed = true;
        return;
    }
}

/* The next whole message received, or false. */
static bool take_msg(session *s, uint8_t *type, uint8_t **data, uint32_t *len)
{
    if (s->nin < 5)
        return false;
    uint32_t n = get32(s->in + 1);
    if (s->nin < 5 + (size_t)n)
        return false;
    *type = s->in[0];
    *len = n;
    *data = s->in + 5;
    return true;
}

static void drop_msg(session *s)
{
    size_t n = 5 + get32(s->in + 1);
    memmove(s->in, s->in + n, s->nin - n);
    s->nin -= n;
}

/* ---------------------------------------------------------------------- */
/* Ports */

static port *port_of(uint16_t refnum)
{
    for (int i = 0; i < MAX_PORTS; i++)
        if (ports[i].used && ports[i].refnum == refnum)
            return &ports[i];
    return NULL;
}

static session *session_of(uint32_t refnum)
{
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].used && sessions[i].refnum == refnum)
            return &sessions[i];
    return NULL;
}

/* Two port names the same: script, name, and the type or creator and type. */
static bool same_name(const uint8_t *a, const uint8_t *b)
{
    if (a[2] != b[2] || memcmp(a + 3, b + 3, a[2]) || a[36] != b[36] || a[37] != b[37])
        return false;
    if (a[37] == 1) /* ppcByCreatorAndType */
        return !memcmp(a + 38, b + 38, 8);
    return a[38] == b[38] && !memcmp(a + 39, b + 39, a[38]);
}

/* A name pattern: "=" for the name or the type matches any. */
static bool matches(const uint8_t *pat, const uint8_t *name)
{
    bool any_name = pat[2] == 1 && pat[3] == '=';
    if (!any_name && (pat[2] != name[2] || memcmp(pat + 3, name + 3, pat[2])))
        return false;
    if (pat[37] == 1)
        return name[37] == 1 && !memcmp(pat + 38, name + 38, 8);
    bool any_type = pat[38] == 1 && pat[39] == '=';
    if (any_type)
        return true;
    return name[37] != 1 && pat[38] == name[38] && !memcmp(pat + 39, name + 39, pat[38]);
}

static void *listener(void *arg)
{
    intptr_t i = (intptr_t)arg;
    int sock = ports[i].sock;
    for (;;) {
        int fd = accept(sock, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            return NULL; /* the port closed */
        }
        pthread_mutex_lock(&lock);
        if (ports[i].used && ports[i].sock == sock && ports[i].npending < 8)
            ports[i].pending[ports[i].npending++] = fd;
        else
            close(fd);
        pthread_mutex_unlock(&lock);
    }
}

/* Every port open on this host: its name and socket path; max at most. */
typedef struct {
    uint8_t name[NAME];
    char path[600];
} port_entry;

static int list_ports(port_entry *out, int max)
{
    DIR *d = opendir(folder());
    if (!d)
        return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        int pid;
        unsigned ref;
        char rest[8] = "";
        if (sscanf(e->d_name, "p%d.%u.%7s", &pid, &ref, rest) != 3 || strcmp(rest, "name"))
            continue;
        char path[600];
        snprintf(path, sizeof path, "%s/%s", folder(), e->d_name);
        if (kill(pid, 0) != 0 && errno == ESRCH) { /* left by a run that ended badly */
            unlink(path);
            path[strlen(path) - 5] = 0;
            unlink(path);
            continue;
        }
        FILE *f = fopen(path, "rb");
        if (!f)
            continue;
        bool ok = fread(out[n].name, 1, NAME, f) == NAME;
        fclose(f);
        if (!ok)
            continue;
        path[strlen(path) - 5] = 0;
        snprintf(out[n].path, sizeof out[n].path, "%s", path);
        n++;
    }
    closedir(d);
    return n;
}

static int16_t open_port(gaddr pb)
{
    gaddr namep = rd32(pb + P_NAME);
    if (!namep)
        return badPortNameErr;
    if (rd8(pb + P_SERVICE) != ppcServiceRealTime)
        return badServiceMethodErr;
    uint8_t name[NAME];
    gget(name, namep, NAME);
    if (!name[2] || name[2] > 32)
        return badPortNameErr;
    static port_entry all[64];
    int n = list_ports(all, 64);
    for (int i = 0; i < n; i++)
        if (same_name(all[i].name, name))
            return portNameExistsErr;
    int i = 0;
    while (i < MAX_PORTS && ports[i].used)
        i++;
    if (i == MAX_PORTS)
        return noPortErr; /* the port table is full */
    port *p = &ports[i];
    memset(p, 0, sizeof *p);
    p->refnum = next_port++;
    memcpy(p->name, name, NAME);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(p->path, sizeof p->path, "%s/p%d.%u", folder(), (int)getpid(), p->refnum);
    if (strlen(p->path) >= sizeof sa.sun_path)
        return noPortErr;
    strcpy(sa.sun_path, p->path);
    unlink(p->path);
    p->sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (p->sock < 0 || bind(p->sock, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(p->sock, 8) != 0) {
        if (p->sock >= 0)
            close(p->sock);
        return noPortErr;
    }
    char npath[620];
    snprintf(npath, sizeof npath, "%s.name", p->path);
    FILE *f = fopen(npath, "wb");
    if (f) {
        fwrite(name, 1, NAME, f);
        fclose(f);
    }
    signal(SIGPIPE, SIG_IGN);
    p->used = true;
    pthread_t t;
    pthread_create(&t, NULL, listener, (void *)(intptr_t)i);
    pthread_detach(t);
    wr16(pb + P_PORT, p->refnum);
    wr8(pb + P_REGISTERED, 0); /* not on the network */
    return noErr;
}

static void end_session(session *s)
{
    if (s->fd >= 0)
        close(s->fd);
    free(s->in);
    memset(s, 0, sizeof *s);
}

/* Waiting calls on a port or session, ended with an error. */
static void fail_waits(uint16_t portref, uint32_t sessref, int16_t err);

static int16_t close_port(uint16_t refnum)
{
    port *p = port_of(refnum);
    if (!p)
        return noPortErr;
    pthread_mutex_lock(&lock);
    p->used = false;
    shutdown(p->sock, SHUT_RDWR);
    close(p->sock);
    for (int k = 0; k < p->npending; k++)
        close(p->pending[k]);
    p->npending = 0;
    pthread_mutex_unlock(&lock);
    char npath[620];
    snprintf(npath, sizeof npath, "%s.name", p->path);
    unlink(npath);
    unlink(p->path);
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].used && sessions[i].port == refnum) {
            fail_waits(0, sessions[i].refnum, portClosedErr);
            end_session(&sessions[i]);
        }
    fail_waits(refnum, 0, portClosedErr);
    return noErr;
}

static void unlink_all(void)
{
    for (int i = 0; i < MAX_PORTS; i++)
        if (ports[i].used) {
            char npath[620];
            snprintf(npath, sizeof npath, "%s.name", ports[i].path);
            unlink(npath);
            unlink(ports[i].path);
        }
}

void ppc_toolbox_reset(void)
{
    static bool registered;
    if (!registered) {
        atexit(unlink_all);
        registered = true;
    }
    for (int i = 0; i < MAX_PORTS; i++)
        if (ports[i].used)
            close_port(ports[i].refnum);
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].used)
            end_session(&sessions[i]);
    nwaits = ndone = 0;
}

/* ---------------------------------------------------------------------- */
/* Calls that wait */

static void finish(gaddr pb, int16_t r, bool async)
{
    wr16(pb + P_RESULT, (uint16_t)r);
    if (async && rd32(pb + P_COMPLETION) && ndone < MAX_WAIT)
        done[ndone++] = (completion){ pb, r };
}

static void fail_waits(uint16_t portref, uint32_t sessref, int16_t err)
{
    for (int i = 0; i < nwaits;) {
        waiting *w = &waits[i];
        bool hit = (portref && (w->kind == W_INFORM || w->kind == W_START) && rd16(w->pb + P_PORT) == portref) ||
                   (sessref && w->kind == W_READ && rd32(w->pb + P_SESS) == sessref) ||
                   (sessref && w->kind == W_START && rd32(w->pb + P_SESS) == sessref);
        if (hit) {
            finish(w->pb, err, w->async);
            waits[i] = waits[--nwaits];
        } else {
            i++;
        }
    }
}

static session *new_session(uint16_t portref, int fd)
{
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (!sessions[i].used) {
            session *s = &sessions[i];
            memset(s, 0, sizeof *s);
            s->used = true;
            s->refnum = next_sess++;
            s->port = portref;
            s->fd = fd;
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            return s;
        }
    return NULL;
}

/* PPCInform: a request taken from the port, the session made for it.
 * Returns ioInProgress while there is none. */
static int16_t try_inform(gaddr pb)
{
    port *p = port_of(rd16(pb + P_PORT));
    if (!p)
        return noPortErr;
    for (;;) {
        pthread_mutex_lock(&lock);
        int fd = p->npending ? p->pending[0] : -1;
        if (fd >= 0)
            memmove(p->pending, p->pending + 1, sizeof p->pending[0] * (size_t)--p->npending);
        pthread_mutex_unlock(&lock);
        if (fd < 0)
            return ioInProgress;
        /* the starter's request: its port's name and the userData */
        uint8_t head[5], req[NAME + 4];
        struct pollfd pf = { fd, POLLIN, 0 };
        if (poll(&pf, 1, 2000) != 1 || recv(fd, head, 5, MSG_WAITALL) != 5 || head[0] != 'S' ||
            get32(head + 1) != sizeof req || recv(fd, req, sizeof req, MSG_WAITALL) != (ssize_t)sizeof req) {
            close(fd);
            continue; /* not a request after all */
        }
        session *s = new_session(p->refnum, fd);
        if (!s) {
            uint8_t why[4] = { 0 };
            send_msg(fd, 'R', why, 4);
            close(fd);
            return sessTableErr;
        }
        wr32(pb + P_SESS, s->refnum);
        wr8(pb + P_SERVICE, ppcServiceRealTime);
        if (rd32(pb + P_NAME))
            gput(rd32(pb + P_NAME), req, NAME);
        if (rd32(pb + P_LOCATION))
            wr16(rd32(pb + P_LOCATION), ppcNoLocation);
        if (rd32(pb + P_USERNAME))
            wr8(rd32(pb + P_USERNAME), 0); /* a local session has no user */
        wr32(pb + P_USERDATA, get32(req + NAME));
        wr8(pb + P_REQTYPE, ppcLocalOrigin);
        if (rd8(pb + P_AUTO)) {
            send_msg(fd, 'A', NULL, 0);
            s->accepted = true;
        }
        return noErr;
    }
}

/* PPCStart: the other side's answer; ioInProgress while there is none. */
static int16_t try_start(gaddr pb)
{
    session *s = session_of(rd32(pb + P_SESS));
    if (!s)
        return sessClosedErr;
    receive(s);
    uint8_t type, *data;
    uint32_t len;
    if (!take_msg(s, &type, &data, &len))
        return s->closed ? noResponseErr : ioInProgress;
    if (type == 'A') {
        drop_msg(s);
        s->accepted = true;
        wr8(pb + P_SERVICE, ppcServiceRealTime);
        return noErr;
    }
    wr32(pb + P_REJECT, len >= 4 ? get32(data) : 0);
    end_session(s);
    wr32(pb + P_SESS, 0);
    return userRejectErr;
}

/* PPCRead: the current block's bytes, until the buffer is full or the
 * block ends; ioInProgress while there are none to give. */
static int16_t try_read(gaddr pb)
{
    session *s = session_of(rd32(pb + P_SESS));
    if (!s)
        return noSessionErr;
    receive(s);
    uint32_t want = rd32(pb + P_LEN), got = rd32(pb + P_ACTUAL);
    gaddr buf = rd32(pb + P_BUF);
    uint8_t type, *data;
    uint32_t len;
    while (got < want && take_msg(s, &type, &data, &len)) {
        if (type != 'D' || len < 13) {
            drop_msg(s);
            continue;
        }
        if (!got && !s->in_block) { /* a block's first bytes: its description */
            wr32(pb + P_USERDATA, get32(data + 1));
            wr32(pb + P_CREATOR, get32(data + 5));
            wr32(pb + P_TYPE, get32(data + 9));
        }
        s->in_block = true;
        uint32_t k = len - 13 < want - got ? len - 13 : want - got;
        gput(buf + got, data + 13, k);
        got += k;
        wr32(pb + P_ACTUAL, got);
        bool more = data[0] != 0;
        if (k < len - 13) { /* the buffer is full: the rest stays */
            memmove(data + 13, data + 13 + k, len - 13 - k);
            put32(s->in + 1, len - k);
            memmove(s->in + 5 + len - k, s->in + 5 + len, s->nin - 5 - len);
            s->nin -= k;
            wr8(pb + P_MORE, 1);
            return noErr;
        }
        drop_msg(s);
        if (!more) {
            s->in_block = false;
            wr8(pb + P_MORE, 0);
            return noErr;
        }
    }
    if (got == want && want) {
        wr8(pb + P_MORE, 1);
        return noErr;
    }
    return s->closed ? sessClosedErr : ioInProgress;
}

static int16_t try_wait(waiting *w)
{
    switch (w->kind) {
    case W_INFORM: return try_inform(w->pb);
    case W_START: return try_start(w->pb);
    default: return try_read(w->pb);
    }
}

/* Interrupt time: waiting calls that can finish, then their completions
 * (the block on the stack, for Pascal routines, and in A0). */
void ppc_toolbox_poll(void)
{
    for (int i = 0; i < nwaits;) {
        int16_t r = try_wait(&waits[i]);
        if (r == ioInProgress) {
            i++;
            continue;
        }
        waiting w = waits[i];
        waits[i] = waits[--nwaits];
        finish(w.pb, r, w.async);
    }
    while (ndone > 0) {
        gaddr pb = done[0].pb;
        int16_t r = done[0].result;
        memmove(done, done + 1, sizeof done[0] * (size_t)--ndone);
        cpu68k *c = g_cpu;
        uint32_t d[8], a[8];
        memcpy(d, c->d, sizeof d);
        memcpy(a, c->a, sizeof a);
        uint16_t sr = c->sr;
        c->a[0] = pb;
        c->d[0] = (uint32_t)(int32_t)r;
        cpu_push32(c, pb);
        guest_call_regs(rd32(pb + P_COMPLETION));
        memcpy(c->d, d, sizeof d);
        memcpy(c->a, a, sizeof a);
        c->sr = sr;
    }
}

/* A call that may wait: asynchronous, it goes on in the background (and
 * one already answered has its completion routine called all the same);
 * synchronous, it is waited for here. */
static int16_t waitable(gaddr pb, wait_kind kind, bool async)
{
    waiting w = { pb, kind, async };
    int16_t r = try_wait(&w);
    if (r != ioInProgress && async) {
        finish(pb, r, true);
        return ioInProgress;
    }
    if (r != ioInProgress)
        return r;
    if (async) {
        if (nwaits == MAX_WAIT)
            return badReqErr;
        waits[nwaits++] = w;
        return ioInProgress;
    }
    while ((r = try_wait(&w)) == ioInProgress && g_cpu && !g_cpu->halted) {
        usleep(1000);
        ppc_toolbox_poll(); /* this run's own other calls */
        machine_advance_ticks();
        time_pump();
    }
    return r == ioInProgress ? noResponseErr : r;
}

/* ---------------------------------------------------------------------- */
/* The calls */

static int16_t start(gaddr pb, bool async)
{
    if (!port_of(rd16(pb + P_PORT)))
        return noPortErr;
    gaddr namep = rd32(pb + P_NAME), loc = rd32(pb + P_LOCATION);
    if (!namep)
        return badPortNameErr;
    if (loc && (int16_t)rd16(loc) != ppcNoLocation)
        return noResponseErr; /* another machine: no network to reach it on */
    uint8_t name[NAME];
    gget(name, namep, NAME);
    static port_entry all[64];
    int n = list_ports(all, 64), k = 0;
    while (k < n && !same_name(all[k].name, name))
        k++;
    if (k == n)
        return destPortErr;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", all[k].path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return noResponseErr;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return destPortErr;
    }
    signal(SIGPIPE, SIG_IGN);
    uint8_t req[NAME + 4];
    memcpy(req, port_of(rd16(pb + P_PORT))->name, NAME);
    put32(req + NAME, rd32(pb + P_USERDATA));
    session *s = new_session(rd16(pb + P_PORT), fd);
    if (!s) {
        close(fd);
        return sessTableErr;
    }
    if (!send_msg(fd, 'S', req, sizeof req)) {
        end_session(s);
        return noResponseErr;
    }
    wr32(pb + P_SESS, s->refnum);
    return waitable(pb, W_START, async);
}

static int16_t answer(gaddr pb, bool accept)
{
    session *s = session_of(rd32(pb + P_SESS));
    if (!s || s->accepted)
        return noSessionErr;
    if (accept) {
        s->accepted = send_msg(s->fd, 'A', NULL, 0);
        return s->accepted ? noErr : sessClosedErr;
    }
    uint8_t why[4];
    put32(why, rd32(pb + P_REJECT));
    send_msg(s->fd, 'R', why, 4);
    end_session(s);
    return noErr;
}

static int16_t write_block(gaddr pb)
{
    session *s = session_of(rd32(pb + P_SESS));
    if (!s || !s->accepted)
        return noSessionErr;
    receive(s);
    if (s->closed)
        return sessClosedErr;
    uint32_t n = rd32(pb + P_LEN);
    uint8_t *msg = malloc(13 + (size_t)n);
    if (!msg)
        return memFullErr;
    msg[0] = rd8(pb + P_MORE) ? 1 : 0;
    put32(msg + 1, rd32(pb + P_USERDATA));
    put32(msg + 5, rd32(pb + P_CREATOR));
    put32(msg + 9, rd32(pb + P_TYPE));
    if (n)
        gget(msg + 13, rd32(pb + P_BUF), n);
    bool ok = send_msg(s->fd, 'D', msg, 13 + n);
    free(msg);
    if (!ok) {
        s->closed = true;
        return sessClosedErr;
    }
    wr32(pb + P_ACTUAL, n);
    return noErr;
}

static int16_t list(gaddr pb)
{
    gaddr pat = rd32(pb + P_NAME), loc = rd32(pb + P_LOCATION), out = rd32(pb + P_LISTBUF);
    wr16(pb + P_ACTCOUNT, 0);
    if (!pat || !out)
        return badPortNameErr;
    if (loc && (int16_t)rd16(loc) != ppcNoLocation)
        return noResponseErr;
    uint8_t want[NAME];
    gget(want, pat, NAME);
    static port_entry all[64];
    int n = list_ports(all, 64);
    uint16_t skip = rd16(pb + P_START), max = rd16(pb + P_REQCOUNT), got = 0, seen = 0;
    for (int i = 0; i < n && got < max; i++) {
        if (!matches(want, all[i].name))
            continue;
        if (seen++ < skip)
            continue;
        gaddr e = out + (uint32_t)got * (2 + NAME); /* PortInfoRec: filler, authRequired, name */
        wr8(e, 0);
        wr8(e + 1, 0); /* local: no authentication */
        gput(e + 2, all[i].name, NAME);
        got++;
    }
    wr16(pb + P_ACTCOUNT, got);
    return noErr;
}

/* The selector's call; ioInProgress for one going on in the background. */
static int16_t call(int sel, gaddr pb, bool async)
{
    if (sel == 0) /* PPCInit */
        return noErr;
    if (!pb)
        return badReqErr;
    wr16(pb + P_RESULT, ioInProgress);
    switch (sel) {
    case 1: return open_port(pb);
    case 2: return start(pb, async);
    case 3: return port_of(rd16(pb + P_PORT)) ? waitable(pb, W_INFORM, async) : noPortErr;
    case 4: return answer(pb, true);
    case 5: return answer(pb, false);
    case 6: return write_block(pb);
    case 7:
        wr32(pb + P_ACTUAL, 0);
        return waitable(pb, W_READ, async);
    case 8: { /* PPCEnd */
        session *s = session_of(rd32(pb + P_SESS));
        if (!s)
            return noSessionErr;
        fail_waits(0, s->refnum, sessClosedErr);
        end_session(s);
        return noErr;
    }
    case 9: return close_port(rd16(pb + P_PORT));
    case 10: return list(pb);
    case 11: return badReqErr; /* IPCKillListPorts: lists finish before they return */
    default: return badReqErr;
    }
}

/* The result for the caller, in ioResult too.  An asynchronous call that
 * fails before it starts (no such port to start a session with) says so
 * at once, and its completion routine isn't called: SpeedShare frees the
 * parameter block then, and a completion run later read the freed block.
 * One that finishes at once, with its answer or its result, is
 * completed as one that waited would be. */
static int16_t ppctb_call(int sel, gaddr pb, bool async)
{
    int16_t r = call(sel, pb, async);
    if (r == ioInProgress)
        return noErr;
    if (pb && sel)
        finish(pb, r, async && r == noErr);
    return r;
}

/* _PPC: D0 the selector, A0 the block; bit 10 of the trap word async. */
void tb_PPCDispatch(int32_t d0, int32_t a0)
{
    bool async = (trap_current_word() & 0x0400) != 0;
    g_cpu->d[0] = (uint32_t)(int32_t)ppctb_call((int)(d0 & 0xFF), (gaddr)a0, async);
}

/* PPCBrowser (PACK 9): there is no browser dialog to choose another
 * program's port with, so the user is taken to have cancelled it, which
 * programs handle (AgendaMaker's Send Message sends nothing). */
int16_t tb_PPCBrowser(gaddr prompt, gaddr appl_label, int8_t default_specified, gaddr location, gaddr port_info,
                      gaddr filter, gaddr nbp_type)
{
    (void)prompt, (void)appl_label, (void)default_specified, (void)location, (void)port_info, (void)filter,
        (void)nbp_type;
    return -128; /* userCanceledErr */
}

int16_t tb_PPCOpenSync(gaddr pb) { return ppctb_call(1, pb, false); }
int16_t tb_PPCOpenAsync(gaddr pb) { return ppctb_call(1, pb, true); }
int16_t tb_PPCStartSync(gaddr pb) { return ppctb_call(2, pb, false); }
int16_t tb_PPCStartAsync(gaddr pb) { return ppctb_call(2, pb, true); }
int16_t tb_PPCInformSync(gaddr pb) { return ppctb_call(3, pb, false); }
int16_t tb_PPCInformAsync(gaddr pb) { return ppctb_call(3, pb, true); }
int16_t tb_PPCAcceptSync(gaddr pb) { return ppctb_call(4, pb, false); }
int16_t tb_PPCAcceptAsync(gaddr pb) { return ppctb_call(4, pb, true); }
int16_t tb_PPCRejectSync(gaddr pb) { return ppctb_call(5, pb, false); }
int16_t tb_PPCRejectAsync(gaddr pb) { return ppctb_call(5, pb, true); }
int16_t tb_PPCWriteSync(gaddr pb) { return ppctb_call(6, pb, false); }
int16_t tb_PPCWriteAsync(gaddr pb) { return ppctb_call(6, pb, true); }
int16_t tb_PPCReadSync(gaddr pb) { return ppctb_call(7, pb, false); }
int16_t tb_PPCReadAsync(gaddr pb) { return ppctb_call(7, pb, true); }
int16_t tb_PPCEndSync(gaddr pb) { return ppctb_call(8, pb, false); }
int16_t tb_PPCEndAsync(gaddr pb) { return ppctb_call(8, pb, true); }
int16_t tb_PPCCloseSync(gaddr pb) { return ppctb_call(9, pb, false); }
int16_t tb_PPCCloseAsync(gaddr pb) { return ppctb_call(9, pb, true); }
int16_t tb_IPCListPortsSync(gaddr pb) { return ppctb_call(10, pb, false); }
int16_t tb_IPCListPortsAsync(gaddr pb) { return ppctb_call(10, pb, true); }
int16_t tb_IPCKillListPorts(gaddr pb) { return ppctb_call(11, pb, false); }

/* User identities: only the default user's (GetDefaultUser's 1) exists;
 * local sessions need no other. */
int16_t tb_DeleteUserIdentity(uint32_t userRef) { return userRef == 1 ? noErr : noUserRefErr; }

/* StartSecureSession: a session with a port on this machine needs no
 * authentication, so it is PPCStart's, no guest chosen. */
int16_t tb_StartSecureSession(gaddr pb, gaddr userName, int8_t useDefault, int8_t allowGuest, gaddr guestSelected,
                              gaddr prompt)
{
    if (guestSelected)
        wr8(guestSelected, 0);
    return ppctb_call(2, pb, false);
}
