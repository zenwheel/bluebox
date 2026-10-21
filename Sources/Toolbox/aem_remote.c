/*
 * Apple Events between Blue Box processes, and from the host (`bluebox
 * send`).
 *
 * Each run that listens has a Unix socket named for its process ID and
 * its application's signature ("1234.54455854"), in a folder kept per user
 * and home folder ($TMPDIR/bluebox-ae-XXXXXXXX), so runs with another home
 * don't meet.  Events travel flattened (aem_flatten): a message is its
 * length (4 bytes, big-endian), the send mode (4 bytes) and the event; the
 * reply comes back on the same connection framed the same way, the
 * handler's result in place of the mode.
 */
#include <errno.h>
#include <dirent.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "forks.h"
#include "machine.h"
#include "tb_internal.h"

#define T(s) fourcc(s)

enum { kAENoReply = 1, kAEQueueReply = 2, kAEWaitReply = 3 };
enum { procNotFound = -600, errAETimeout = -1712, errAEEventNotHandled = -1708 };
enum { MAX_MESSAGE = 16 << 20 };

extern uint32_t g_app_creator;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

/* The folder of sockets, made if need be. */
static const char *folder(void)
{
    static char path[512];
    if (path[0])
        return path;
    const char *tmp = getenv("TMPDIR"), *home = getenv("HOME");
    if (!tmp || !*tmp)
        tmp = "/tmp/";
    uint32_t h = 2166136261u; /* FNV-1a of the home folder */
    for (const char *p = home ? home : ""; *p; p++)
        h = (h ^ (uint8_t)*p) * 16777619u;
    bool slash = tmp[strlen(tmp) - 1] == '/';
    snprintf(path, sizeof path, "%s%sbluebox-ae-%08x", tmp, slash ? "" : "/", h);
    mkdir(path, 0700);
    return path;
}

static bool write_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

/* Read n bytes, waiting at most timeout_ms in all (negative: for ever). */
static bool read_all(int fd, uint8_t *p, size_t n, int timeout_ms)
{
    while (n) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int r = poll(&pfd, 1, timeout_ms);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return false;
        ssize_t k = read(fd, p, n);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

static bool send_message(int fd, uint32_t word, const uint8_t *data, uint32_t len)
{
    uint8_t head[8];
    put32(head, len + 4);
    put32(head + 4, word);
    return write_all(fd, head, 8) && (!len || write_all(fd, data, len));
}

/* A message: its word, and its data (malloc'd). */
static bool read_message(int fd, int timeout_ms, uint32_t *word, uint8_t **data, uint32_t *len)
{
    uint8_t head[8];
    if (!read_all(fd, head, 8, timeout_ms))
        return false;
    uint32_t n = be32(head);
    if (n < 4 || n > MAX_MESSAGE)
        return false;
    *word = be32(head + 4);
    *len = n - 4;
    *data = malloc(*len ? *len : 1);
    if (*len && !read_all(fd, *data, *len, timeout_ms)) {
        free(*data);
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------------- */
/* Listening */

static char socket_path[600];

static void unlink_socket(void)
{
    if (socket_path[0])
        unlink(socket_path);
}

static void reply_to_connection(void *ctx, const uint8_t *data, uint32_t len, int16_t err)
{
    int fd = (int)(intptr_t)ctx;
    send_message(fd, (uint32_t)(int32_t)err, data, data ? len : 0);
    close(fd);
}

static void *listener(void *arg)
{
    int sock = (int)(intptr_t)arg;
    for (;;) {
        int fd = accept(sock, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            return NULL;
        }
        uint32_t mode, len;
        uint8_t *data;
        if (!read_message(fd, 5000, &mode, &data, &len)) {
            close(fd);
            continue;
        }
        if ((mode & 3) == kAENoReply) {
            aem_queue_flat(data, len, NULL, NULL);
            close(fd);
        } else
            aem_queue_flat(data, len, reply_to_connection, (void *)(intptr_t)fd);
        free(data);
    }
}

bool machine_listen_for_apple_events(void)
{
    if (socket_path[0])
        return true;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(socket_path, sizeof socket_path, "%s/%d.%08X", folder(), (int)getpid(), g_app_creator);
    if (strlen(socket_path) >= sizeof sa.sun_path) {
        socket_path[0] = 0;
        return false;
    }
    strcpy(sa.sun_path, socket_path);
    unlink(socket_path);
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0 || bind(sock, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(sock, 8) != 0) {
        if (sock >= 0)
            close(sock);
        socket_path[0] = 0;
        return false;
    }
    signal(SIGPIPE, SIG_IGN); /* a sender that went away */
    atexit(unlink_socket);
    pthread_t t;
    pthread_create(&t, NULL, listener, (void *)(intptr_t)sock);
    pthread_detach(t);
    return true;
}

/* ---------------------------------------------------------------------- */
/* Sending */

int machine_ae_processes(machine_ae_process *out, int max)
{
    DIR *d = opendir(folder());
    if (!d)
        return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        int pid;
        unsigned sig;
        char extra;
        if (sscanf(e->d_name, "%d.%8X%c", &pid, &sig, &extra) != 2)
            continue;
        if (kill(pid, 0) != 0 && errno == ESRCH) { /* left by a run that ended badly */
            char path[600];
            snprintf(path, sizeof path, "%s/%s", folder(), e->d_name);
            unlink(path);
            continue;
        }
        out[n++] = (machine_ae_process){ pid, sig };
    }
    closedir(d);
    return n;
}

static int connect_to(int pid, uint32_t sig)
{
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s/%d.%08X", folder(), pid, sig);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int16_t machine_ae_send(int pid, uint32_t sig, const uint8_t *event, uint32_t len, int mode, int timeout_ms,
                        uint8_t **reply, uint32_t *reply_len)
{
    *reply = NULL;
    *reply_len = 0;
    int fd = connect_to(pid, sig);
    if (fd < 0)
        return procNotFound;
    signal(SIGPIPE, SIG_IGN);
    if (!send_message(fd, (uint32_t)mode, event, len)) {
        close(fd);
        return procNotFound;
    }
    if ((mode & 3) == kAENoReply) {
        close(fd);
        return noErr;
    }
    uint32_t err;
    bool ok = read_message(fd, timeout_ms, &err, reply, reply_len);
    close(fd);
    if (!ok)
        return errAETimeout;
    if (!*reply_len) {
        free(*reply);
        *reply = NULL;
    }
    return (int16_t)err;
}

/* A queued reply: waited for on a thread of its own, then delivered to the
 * program as an Answer event. */
typedef struct {
    int pid;
    uint32_t sig;
    uint8_t *event;
    uint32_t len;
} queued_send;

static void *await_reply(void *arg)
{
    queued_send *q = arg;
    uint8_t *reply;
    uint32_t len;
    if (machine_ae_send(q->pid, q->sig, q->event, q->len, kAEQueueReply, -1, &reply, &len) == noErr && reply)
        aem_queue_flat(reply, len, NULL, NULL);
    free(reply);
    free(q->event);
    free(q);
    return NULL;
}

/* The process an address names: a signature ('sign'), a process ID
 * ('kpid') or a target ID ('targ', by its port's creator). */
static bool find_target(const uint8_t *addr, uint32_t type, uint32_t len, machine_ae_process *found)
{
    uint32_t sig = 0;
    int pid = 0;
    if (type == T("sign") && len >= 4)
        sig = be32(addr);
    else if (type == T("kpid") && len >= 4)
        pid = (int)be32(addr);
    else if (type == T("targ") && len >= TargetID_SIZEOF)
        sig = be32(addr + TargetID_name_off + PPCPortRec_u_port_creator_off);
    else
        return false;
    machine_ae_process procs[64];
    int n = machine_ae_processes(procs, 64);
    for (int i = 0; i < n; i++)
        if (procs[i].pid != getpid() && (pid ? procs[i].pid == pid : procs[i].signature == sig)) {
            *found = procs[i];
            return true;
        }
    return false;
}

/* For AESend: an event to another Blue Box process.  The timeout is in
 * ticks (kAEDefaultTimeout, -1, about a minute; kNoTimeOut, -2). */
int16_t aem_remote_send(const uint8_t *addr, uint32_t addr_type, uint32_t addr_len, const uint8_t *event,
                        uint32_t len, int mode, int32_t timeout, uint8_t **reply, uint32_t *reply_len)
{
    *reply = NULL;
    *reply_len = 0;
    machine_ae_process p;
    if (!find_target(addr, addr_type, addr_len, &p))
        return procNotFound;
    if (mode == kAEQueueReply) {
        queued_send *q = malloc(sizeof *q);
        *q = (queued_send){ p.pid, p.signature, malloc(len), len };
        memcpy(q->event, event, len);
        pthread_t t;
        pthread_create(&t, NULL, await_reply, q);
        pthread_detach(t);
        return noErr;
    }
    int ms = timeout == -2 ? -1 : timeout == -1 ? 60000 : timeout * 1000 / 60;
    int16_t err = machine_ae_send(p.pid, p.signature, event, len, mode, ms, reply, reply_len);
    /* Delivered: the handler's error is in the reply (keyErrorNumber). */
    return *reply || mode == kAENoReply ? noErr : err;
}

/* ---------------------------------------------------------------------- */
/* Replies, read on the host */

/* The parameter `key` of a flattened record: its type, data and length. */
static bool flat_param(const uint8_t *p, uint32_t len, uint32_t key, uint32_t *type, const uint8_t **data,
                       uint32_t *dlen)
{
    if (len < 12 || be32(p) != 0x42526563 /* 'BRec' */)
        return false;
    uint32_t na = be32(p + 4), np = be32(p + 8), off = 12;
    for (uint32_t i = 0; i < na + np && off + 12 <= len; i++) {
        uint32_t k = be32(p + off), t = be32(p + off + 4), n = be32(p + off + 8);
        if (off + 12 + n > len)
            break;
        if (i >= na && k == key) {
            *type = t;
            *data = p + off + 12;
            *dlen = n;
            return true;
        }
        off += 12 + ((n + 1) & ~1u);
    }
    return false;
}

static void render(uint32_t type, const uint8_t *d, uint32_t n, char *out, size_t size)
{
    if (type == T("TEXT") || type == T("cstr")) {
        size_t k = macroman_to_utf8(d, n, out, size);
        for (size_t i = 0; i < k; i++)
            if (out[i] == '\r')
                out[i] = '\n';
    } else if (type == T("long") && n == 4)
        snprintf(out, size, "%d", (int32_t)be32(d));
    else if (type == T("shor") && n == 2)
        snprintf(out, size, "%d", (int16_t)(d[0] << 8 | d[1]));
    else if (type == T("bool") && n >= 1)
        snprintf(out, size, "%s", d[0] ? "true" : "false");
    else if ((type == T("enum") || type == T("type")) && n == 4)
        snprintf(out, size, "%.4s", (const char *)d);
    else
        snprintf(out, size, "«%.4s» (%u bytes)", (const char *)&(uint32_t){ __builtin_bswap32(type) }, n);
}

bool machine_ae_reply_result(const uint8_t *reply, uint32_t len, char *text, size_t size, int32_t *errn)
{
    uint32_t type, n;
    const uint8_t *d;
    *errn = 0;
    if (size)
        text[0] = 0;
    if (flat_param(reply, len, T("errn"), &type, &d, &n) && (type == T("long") || type == T("shor")))
        *errn = type == T("long") ? (int32_t)be32(d) : (int16_t)(d[0] << 8 | d[1]);
    if (flat_param(reply, len, T("----"), &type, &d, &n)) {
        render(type, d, n, text, size);
        return true;
    }
    if (flat_param(reply, len, T("errs"), &type, &d, &n)) /* the error's message */
        render(type, d, n, text, size);
    return false;
}

int16_t machine_ae_send_to(int pid, uint32_t sig, uint32_t cls, uint32_t id, const char *const *files, int nfiles,
                           const char *text, bool wait, char *result, size_t size, int32_t *errn)
{
    uint32_t len, rlen;
    uint8_t *event = aem_build_flat(cls, id, files, nfiles, text, wait, &len), *reply;
    int16_t err = machine_ae_send(pid, sig, event, len, wait ? kAEWaitReply : kAENoReply, 120000, &reply, &rlen);
    free(event);
    *errn = 0;
    if (size)
        result[0] = 0;
    if (reply)
        machine_ae_reply_result(reply, rlen, result, size, errn);
    free(reply);
    return err;
}
