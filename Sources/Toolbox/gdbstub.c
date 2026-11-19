/*
 * A gdb-remote stub over MacsBug (macsbug.c): lldb (or gdb) attaches to
 * the guest over TCP ("gdb-remote PORT" in lldb) and sees one thread, the
 * program's processor: a PowerPC program's (lldb's sysv-ppc ABI: r0-r31,
 * f0-f31, pc, msr, cr, lr, ctr, xer, fpscr) or a 68k program's (d0-d7,
 * a0-a7, ps, pc, as gdb's m68k target numbers them).
 *
 * The stub is MacsBug's console while it is attached: the machine stops
 * in the debugger as for any console, and the stub answers the client
 * from the stopped machine (registers, memory) and turns its requests to
 * go on into MacsBug's commands (c: g, s: s, a breakpoint: br, kill: es).
 * Breakpoints stay MacsBug's, kept host-side: nothing is written into the
 * program's code.  It runs on a thread of its own; the machine's thread
 * tells it of each stop.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "machine.h"
#include "ppc.h"
#include "tb_internal.h"

BB_HOST static int client = -1;
BB_HOST static bool ppc_target;    /* the client's view: PowerPC, else 68k */
BB_HOST static bool no_ack;
BB_HOST static atomic_bool stop_seen; /* the machine stopped since the client went on */
BB_HOST static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
BB_HOST static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;

static void stub_event(void *ctx, int event, const char *text)
{
    (void)ctx;
    (void)text;
    if (event != MACHINE_DEBUG_STOP)
        return;
    pthread_mutex_lock(&lock);
    atomic_store(&stop_seen, true);
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

/* ---- Packets ---- */

static const char hexd[] = "0123456789abcdef";

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static bool send_all(const char *p, size_t n)
{
    while (n) {
        ssize_t k = write(client, p, n);
        if (k <= 0) {
            if (k < 0 && errno == EINTR)
                continue;
            return false;
        }
        p += k;
        n -= (size_t)k;
    }
    return true;
}

static void reply(const char *data)
{
    size_t n = strlen(data);
    char *pkt = bb_host_malloc(n + 5);
    if (!pkt)
        return;
    uint8_t sum = 0;
    pkt[0] = '$';
    for (size_t i = 0; i < n; i++) {
        pkt[1 + i] = data[i];
        sum = (uint8_t)(sum + (uint8_t)data[i]);
    }
    pkt[n + 1] = '#';
    pkt[n + 2] = hexd[sum >> 4];
    pkt[n + 3] = hexd[sum & 15];
    send_all(pkt, n + 4);
    bb_host_free(pkt);
}

/* The next packet into buf (0x03, the client's interrupt, as "\x03");
 * false when the connection is gone.  With timeout_ms >= 0, also false
 * (and *timed_out) when nothing came in that time. */
static bool read_packet(char *buf, size_t n, int timeout_ms, bool *timed_out)
{
    *timed_out = false;
    size_t k = 0;
    bool in = false;
    for (;;) {
        if (timeout_ms >= 0) {
            struct pollfd p = { client, POLLIN, 0 };
            int r = poll(&p, 1, timeout_ms);
            if (r == 0) {
                *timed_out = true;
                return false;
            }
            if (r < 0 && errno != EINTR)
                return false;
        }
        char c;
        ssize_t r = read(client, &c, 1);
        if (r <= 0) {
            if (r < 0 && errno == EINTR)
                continue;
            return false;
        }
        if (!in) {
            if (c == 0x03) {
                snprintf(buf, n, "\x03");
                return true;
            }
            if (c == '$') {
                in = true;
                k = 0;
            }
            continue;
        }
        if (c == '#') {
            char cs[2];
            for (int i = 0; i < 2; i++)
                while (read(client, &cs[i], 1) != 1)
                    if (errno != EINTR)
                        return false;
            buf[k < n ? k : n - 1] = 0;
            if (!no_ack)
                send_all("+", 1);
            return true;
        }
        if (k + 1 < n)
            buf[k++] = c;
    }
}

/* ---- Registers ---- */

static void put32(char **o, uint32_t v)
{
    for (int i = 28; i >= 0; i -= 4)
        *(*o)++ = hexd[(v >> i) & 15];
}

static void put64(char **o, uint64_t v)
{
    for (int i = 60; i >= 0; i -= 4)
        *(*o)++ = hexd[(v >> i) & 15];
}

static uint64_t get_hex(const char **s, int digits)
{
    uint64_t v = 0;
    for (int i = 0; i < digits && hexval(**s) >= 0; i++, (*s)++)
        v = v << 4 | (uint64_t)hexval(**s);
    return v;
}

/* PowerPC: r0-r31 (0-31), f0-f31 (32-63), pc 64, msr 65, cr 66, lr 67,
 * ctr 68, xer 69, fpscr 70.  68k: d0-d7 (0-7), a0-a7 (8-15), ps 16, pc 17. */
static int nregs(void) { return ppc_target ? 71 : 18; }
static int reg_bits(int r) { return ppc_target && r >= 32 && r < 64 ? 64 : 32; }

static uint64_t reg_value(int r)
{
    if (ppc_target) {
        ppc_cpu *c = &g_ppc;
        if (r < 32)
            return c->gpr[r];
        if (r < 64)
            return c->fpr[r - 32];
        switch (r) {
        case 64: return c->pc;
        case 65: return ppc_msr(); /* user mode, as an application ran */
        case 66: return c->cr;
        case 67: return c->lr;
        case 68: return c->ctr;
        case 69: return c->xer;
        default: return c->fpscr;
        }
    }
    cpu68k *c = g_cpu;
    return r < 8 ? c->d[r] : r < 16 ? c->a[r - 8] : r == 16 ? c->sr : c->pc;
}

static void set_reg(int r, uint64_t v)
{
    if (ppc_target) {
        ppc_cpu *c = &g_ppc;
        if (r < 32)
            c->gpr[r] = (uint32_t)v;
        else if (r < 64)
            c->fpr[r - 32] = v;
        else if (r == 64)
            c->pc = (uint32_t)v;
        else if (r == 66)
            c->cr = (uint32_t)v;
        else if (r == 67)
            c->lr = (uint32_t)v;
        else if (r == 68)
            c->ctr = (uint32_t)v;
        else if (r == 69)
            c->xer = (uint32_t)v;
        else if (r == 70)
            c->fpscr = (uint32_t)v;
        return;
    }
    cpu68k *c = g_cpu;
    if (r < 8)
        c->d[r] = (uint32_t)v;
    else if (r < 16)
        c->a[r - 8] = (uint32_t)v;
    else if (r == 16)
        cpu_set_sr(c, (uint16_t)v);
    else if (r == 17)
        c->pc = (uint32_t)v;
}

static const char ppc_xml[] =
    "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\"><target version=\"1.0\">"
    "<architecture>powerpc:common</architecture><feature name=\"org.gnu.gdb.power.core\">"
    "%s</feature><feature name=\"org.gnu.gdb.power.fpu\">%s</feature></target>";

static char *target_xml(void)
{
    char *x = bb_host_malloc(16384);
    if (!x)
        return NULL;
    char core[8192] = "", fpu[4096] = "";
    size_t k = 0;
    if (ppc_target) {
        for (int i = 0; i < 32; i++)
            k += (size_t)snprintf(core + k, sizeof core - k,
                                  "<reg name=\"r%d\" bitsize=\"32\" type=\"uint32\" regnum=\"%d\" group=\"general\"%s/>", i,
                                  i, i == 1 ? " generic=\"sp\"" : "");
        const char *names[] = { "pc", "msr", "cr", "lr", "ctr", "xer" };
        for (int i = 0; i < 6; i++)
            k += (size_t)snprintf(core + k, sizeof core - k,
                                  "<reg name=\"%s\" bitsize=\"32\" type=\"%s\" regnum=\"%d\" group=\"general\"%s/>",
                                  names[i], i == 0 ? "code_ptr" : "uint32", 64 + i,
                                  i == 0 ? " generic=\"pc\"" : i == 3 ? " generic=\"ra\"" : "");
        size_t f = 0;
        for (int i = 0; i < 32; i++)
            f += (size_t)snprintf(fpu + f, sizeof fpu - f,
                                  "<reg name=\"f%d\" bitsize=\"64\" type=\"ieee_double\" regnum=\"%d\" group=\"float\"/>",
                                  i, 32 + i);
        snprintf(fpu + f, sizeof fpu - f, "<reg name=\"fpscr\" bitsize=\"32\" type=\"uint32\" regnum=\"70\" group=\"float\"/>");
        snprintf(x, 16384, ppc_xml, core, fpu);
        return x;
    }
    for (int i = 0; i < 16; i++) {
        const char *generic = i == 14 ? " generic=\"fp\"" : i == 15 ? " generic=\"sp\"" : "";
        k += (size_t)snprintf(core + k, sizeof core - k,
                              "<reg name=\"%s%d\" bitsize=\"32\" type=\"%s\" regnum=\"%d\" group=\"general\"%s/>",
                              i < 8 ? "d" : "a", i & 7, i < 8 ? "uint32" : "data_ptr", i, generic);
    }
    snprintf(core + k, sizeof core - k,
             "<reg name=\"ps\" bitsize=\"32\" type=\"uint32\" regnum=\"16\" group=\"general\" generic=\"flags\"/>"
             "<reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\" regnum=\"17\" group=\"general\" generic=\"pc\"/>");
    snprintf(x, 16384,
             "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\"><target version=\"1.0\">"
             "<architecture>m68k</architecture><feature name=\"org.gnu.gdb.m68k.core\">%s</feature></target>",
             core);
    return x;
}

/* lldb's own description of a register (qRegisterInfo), from the same table. */
static void register_info(int r, char *o, size_t n)
{
    if (r >= nregs()) {
        snprintf(o, n, "E45");
        return;
    }
    char name[16];
    const char *set = "General Purpose Registers", *generic = "", *fmt = "hex", *enc = "uint";
    int bits = reg_bits(r);
    if (ppc_target) {
        static const char *const spr[] = { "pc", "msr", "cr", "lr", "ctr", "xer", "fpscr" };
        if (r < 32)
            snprintf(name, sizeof name, "r%d", r);
        else if (r < 64) {
            snprintf(name, sizeof name, "f%d", r - 32);
            set = "Floating Point Registers";
            fmt = "float";
            enc = "ieee754";
        } else
            snprintf(name, sizeof name, "%s", spr[r - 64]);
        generic = r == 1 ? "generic:sp;" : r == 64 ? "generic:pc;" : r == 67 ? "generic:ra;" : r == 66 ? "generic:flags;" : "";
        if (r == 3)
            generic = "generic:arg1;";
    } else {
        if (r < 16)
            snprintf(name, sizeof name, "%c%d", r < 8 ? 'd' : 'a', r & 7);
        else
            snprintf(name, sizeof name, "%s", r == 16 ? "ps" : "pc");
        generic = r == 14 ? "generic:fp;" : r == 15 ? "generic:sp;" : r == 16 ? "generic:flags;" : r == 17 ? "generic:pc;" : "";
    }
    int offset = 0;
    for (int i = 0; i < r; i++)
        offset += reg_bits(i) / 8;
    snprintf(o, n, "name:%s;bitsize:%d;offset:%d;encoding:%s;format:%s;set:%s;%sdwarf:%d;", name, bits, offset, enc, fmt,
             set, generic, r);
}

/* ---- The session ---- */

static void stop_reply(void)
{
    char buf[256];
    char *o = buf;
    o += snprintf(buf, sizeof buf, "T05thread:1;");
    int pcreg = ppc_target ? 64 : 17, spreg = ppc_target ? 1 : 15;
    o += snprintf(o, 8, "%02x:", pcreg);
    put32(&o, (uint32_t)reg_value(pcreg));
    *o++ = ';';
    o += snprintf(o, 8, "%02x:", spreg);
    put32(&o, (uint32_t)reg_value(spreg));
    *o++ = ';';
    *o = 0;
    reply(buf);
}

/* Wait for the machine to stop, the client's interrupt passed on as the
 * programmer's switch; false if the connection went. */
static bool wait_stop(void)
{
    for (;;) {
        pthread_mutex_lock(&lock);
        if (!atomic_load(&stop_seen)) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 50000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&changed, &lock, &ts);
        }
        bool seen = atomic_exchange(&stop_seen, false);
        pthread_mutex_unlock(&lock);
        if (seen)
            return true;
        if (machine_halted()) {
            reply("W00"); /* the program ended */
            return false;
        }
        char buf[64];
        bool timed_out;
        if (read_packet(buf, sizeof buf, 0, &timed_out)) {
            if (buf[0] == 0x03)
                machine_debug_break();
        } else if (!timed_out) {
            return false;
        }
    }
}

static void go_on(const char *cmd)
{
    atomic_store(&stop_seen, false);
    machine_debug_post(cmd);
}

static void session(void)
{
    char *pkt = bb_host_malloc(0x10000);
    char *out = bb_host_malloc(0x10000);
    if (!pkt || !out)
        return;
    no_ack = false;
    ppc_target = g_ppc_mode;
    atomic_store(&stop_seen, false);
    machine_debug_attach(stub_event, NULL, true);
    machine_debug_break();
    if (!wait_stop())
        goto done;
    for (;;) {
        bool timed_out;
        if (!read_packet(pkt, 0x10000, -1, &timed_out))
            break;
        const char *p = pkt;
        if (p[0] == 0x03 || p[0] == '?') {
            stop_reply();
        } else if (!strncmp(p, "qSupported", 10)) {
            reply("PacketSize=fff0;qXfer:features:read+;QStartNoAckMode+;vContSupported+");
        } else if (!strcmp(p, "QStartNoAckMode")) {
            reply("OK");
            no_ack = true;
        } else if (!strncmp(p, "qXfer:features:read:target.xml:", 32)) {
            const char *a = p + 32;
            uint32_t off = (uint32_t)strtoul(a, (char **)&a, 16), len = a[0] == ',' ? (uint32_t)strtoul(a + 1, NULL, 16) : 0;
            char *x = target_xml();
            size_t n = x ? strlen(x) : 0;
            if (off >= n) {
                reply("l");
            } else {
                size_t k = n - off < len ? n - off : len;
                if (k > 0xFFF0 - 2)
                    k = 0xFFF0 - 2;
                out[0] = k < n - off ? 'm' : 'l';
                memcpy(out + 1, x + off, k);
                out[k + 1] = 0;
                reply(out);
            }
            bb_host_free(x);
        } else if (!strncmp(p, "qRegisterInfo", 13)) {
            register_info((int)strtol(p + 13, NULL, 16), out, 0x10000);
            reply(out);
        } else if (!strcmp(p, "qHostInfo") || !strcmp(p, "qProcessInfo")) {
            /* Big-endian, 32-bit pointers; the triple says the processor. */
            const char *triple = ppc_target ? "706f77657270632d6170706c652d6d61636f73" /* powerpc-apple-macos */
                                            : "6d36386b2d6170706c652d6d61636f73";      /* m68k-apple-macos */
            snprintf(out, 0x10000, "%striple:%s;endian:big;ptrsize:4;%s", !strcmp(p, "qProcessInfo") ? "pid:1;" : "",
                     triple, ppc_target ? "cputype:18;cpusubtype:0;" : "");
            reply(out);
        } else if (!strcmp(p, "qC")) {
            reply("QC1");
        } else if (!strcmp(p, "qfThreadInfo")) {
            reply("m1");
        } else if (!strcmp(p, "qsThreadInfo")) {
            reply("l");
        } else if (!strcmp(p, "qAttached")) {
            reply("1");
        } else if (p[0] == 'H' || !strncmp(p, "QThreadSuffixSupported", 22) || !strncmp(p, "QListThreadsInStopReply", 23)) {
            reply("OK");
        } else if (!strncmp(p, "qThreadStopInfo", 15)) {
            stop_reply();
        } else if (p[0] == 'g') {
            char *o = out;
            for (int r = 0; r < nregs(); r++)
                if (reg_bits(r) == 64)
                    put64(&o, reg_value(r));
                else
                    put32(&o, (uint32_t)reg_value(r));
            *o = 0;
            reply(out);
        } else if (p[0] == 'G') {
            const char *s = p + 1;
            for (int r = 0; r < nregs() && *s; r++)
                set_reg(r, get_hex(&s, reg_bits(r) / 4));
            reply("OK");
        } else if (p[0] == 'p') {
            int r = (int)strtol(p + 1, NULL, 16);
            if (r >= nregs()) {
                reply("E45");
            } else {
                char *o = out;
                if (reg_bits(r) == 64)
                    put64(&o, reg_value(r));
                else
                    put32(&o, (uint32_t)reg_value(r));
                *o = 0;
                reply(out);
            }
        } else if (p[0] == 'P') {
            const char *s = p + 1;
            int r = (int)strtol(s, (char **)&s, 16);
            if (*s == '=' && r < nregs()) {
                s++;
                set_reg(r, get_hex(&s, reg_bits(r) / 4));
                reply("OK");
            } else {
                reply("E45");
            }
        } else if (p[0] == 'm') {
            const char *s = p + 1;
            uint32_t a = (uint32_t)strtoul(s, (char **)&s, 16);
            uint32_t n = *s == ',' ? (uint32_t)strtoul(s + 1, NULL, 16) : 0;
            if (n > 0x7FF0)
                n = 0x7FF0;
            char *o = out;
            for (uint32_t i = 0; i < n; i++) {
                uint8_t b = rd8(a + i);
                *o++ = hexd[b >> 4];
                *o++ = hexd[b & 15];
            }
            *o = 0;
            reply(out);
        } else if (p[0] == 'M') {
            const char *s = p + 1;
            uint32_t a = (uint32_t)strtoul(s, (char **)&s, 16);
            uint32_t n = *s == ',' ? (uint32_t)strtoul(s + 1, (char **)&s, 16) : 0;
            if (*s == ':')
                s++;
            for (uint32_t i = 0; i < n && hexval(s[0]) >= 0 && hexval(s[1]) >= 0; i++, s += 2)
                wr8(a + i, (uint8_t)(hexval(s[0]) << 4 | hexval(s[1])));
            cpu_icache_flush(g_cpu, a, n);
            ppc_jit_flush();
            reply("OK");
        } else if ((p[0] == 'Z' || p[0] == 'z') && (p[1] == '0' || p[1] == '1') && p[2] == ',') {
            /* Breakpoints, kept by MacsBug, not written into the code. */
            uint32_t a = (uint32_t)strtoul(p + 3, NULL, 16);
            char cmd[48];
            snprintf(cmd, sizeof cmd, "%s %08X", p[0] == 'Z' ? "br" : "brc", a);
            machine_debug_command(cmd, out, 0x10000);
            reply("OK");
        } else if (!strcmp(p, "vCont?")) {
            reply("vCont;c;C;s;S");
        } else if (p[0] == 'c' || p[0] == 'C' || p[0] == 's' || p[0] == 'S' || !strncmp(p, "vCont;", 6)) {
            bool step = p[0] == 's' || p[0] == 'S' || !strncmp(p, "vCont;s", 7) || !strncmp(p, "vCont;S", 7);
            /* c ADDR / s ADDR: from there. */
            if ((p[0] == 'c' || p[0] == 's') && p[1]) {
                uint32_t a = (uint32_t)strtoul(p + 1, NULL, 16);
                set_reg(ppc_target ? 64 : 17, a);
            }
            go_on(step ? "s" : "g");
            if (!wait_stop())
                break;
            stop_reply();
        } else if (p[0] == 'k') {
            reply("X09"); /* killed: the program quits, as MacsBug's es */
            go_on("es");
            break;
        } else if (p[0] == 'D') {
            reply("OK");
            break;
        } else {
            reply("");
        }
    }
done:
    machine_debug_attach(NULL, NULL, false); /* a waiting stop goes on */
    bb_host_free(pkt);
    bb_host_free(out);
}

static void *serve(void *arg)
{
    int listener = (int)(intptr_t)arg;
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        client = fd;
        session();
        close(fd);
        client = -1;
        if (machine_halted())
            break;
    }
    close(listener);
    return NULL;
}

bool machine_gdb_listen(int port, bool wait, char *err, size_t errlen)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, errlen, "socket: %s", strerror(errno));
        return false;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* this Mac's own debuggers only */
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 1) < 0) {
        snprintf(err, errlen, "port %d: %s", port, strerror(errno));
        close(fd);
        return false;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, serve, (void *)(intptr_t)fd)) {
        snprintf(err, errlen, "no thread for the stub");
        close(fd);
        return false;
    }
    pthread_detach(t);
    /* The run starts once a client is there, stopped at its first
     * instruction for it. */
    while (wait && client < 0)
        usleep(20000);
    return true;
}
