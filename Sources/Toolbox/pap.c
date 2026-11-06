/*
 * PAP, the Printer Access Protocol (Inside AppleTalk, chapter 10): a
 * print job sent to the LaserWriter the Chooser chose, as the PostScript
 * print_ps.c makes of its pages.
 *
 * Over ATP: the workstation opens a connection at the printer's session
 * listening socket (OpenConn, its own responding socket and flow quantum
 * in the data); the printer's reply says its responding socket and flow
 * quantum, or that it is busy (we wait and ask again).  The printer then
 * reads the job with SendData requests to us, each answered by up to its
 * flow quantum of 512-byte Data packets, the last with EOF; we read what
 * the printer says back (its status, PostScript errors) with SendData
 * requests of our own until its EOF says the job is done; both ends
 * tickle each other while it lasts; CloseConn ends it.  ATP's user bytes
 * are the connection ID, the PAP function, and the sequence number or
 * EOF flag.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "chooser.h"
#include "tb_internal.h"

enum { OpenConn = 1, OpenConnReply = 2, SendData = 3, Data = 4, Tickle = 5, CloseConn = 6, CloseConnReply = 7,
       SendStatus = 8, Status = 9 };
enum { QUANTUM = 8, PACKET = 512 };

size_t print_postscript(const machine_print_job *job, char **out);

typedef struct {
    uint8_t conn, node, srv_skt;
    int quantum;               /* the printer's: packets per SendData */
    const char *data;
    size_t len, sent;
    bool eof_sent;
    uint16_t last_seq;          /* the SendData answered last, again if asked again */
    int last_count;
    uint8_t last[8][578];
    int last_lens[8];
    uint32_t last_users[8];
    uint64_t heard;             /* when the printer was last heard from */
} pap_conn;

static uint64_t ms(void) { return host_clock_ms(); }

/* The printer's requests at our responding socket: SendData (the next
 * piece of the job), Tickle, CloseConn. */
static void request(void *ctx, uint8_t skt, uint8_t node, uint8_t rskt, uint16_t tid, uint32_t user,
                    const uint8_t *data, int len)
{
    (void)data;
    (void)len;
    pap_conn *c = ctx;
    if ((user >> 24) != c->conn || node != c->node)
        return;
    c->heard = ms();
    int fn = (int)(user >> 16) & 0xFF;
    if (fn == SendData) {
        uint16_t seq = (uint16_t)user;
        if (seq == 0 || seq != c->last_seq) { /* (seq 0: not sequenced) */
            int n = 0;
            while (n < c->quantum && n < 8 && c->sent < c->len) {
                size_t k = c->len - c->sent < PACKET ? c->len - c->sent : PACKET;
                memcpy(c->last[n], c->data + c->sent, k);
                c->last_lens[n] = (int)k;
                c->sent += k;
                n++;
            }
            bool eof = c->sent >= c->len;
            if (!n) { /* nothing left: an empty Data, EOF */
                c->last_lens[0] = 0;
                n = 1;
            }
            for (int i = 0; i < n; i++)
                c->last_users[i] = (uint32_t)c->conn << 24 | (uint32_t)Data << 16 | (eof ? 1u << 8 : 0);
            c->last_count = n;
            c->last_seq = seq;
            if (eof)
                c->eof_sent = true;
        }
        atp_host_respond(skt, node, rskt, tid, c->last_count, (const uint8_t(*)[578])c->last, c->last_lens,
                         c->last_users);
    } else if (fn == CloseConn) {
        static const uint8_t none[1][578];
        static const int zero[1] = { 0 };
        uint32_t u = (uint32_t)c->conn << 24 | (uint32_t)CloseConnReply << 16;
        atp_host_respond(skt, node, rskt, tid, 1, none, zero, &u);
        c->eof_sent = true;
        c->srv_skt = 0;
    }
}

static void split_nbp(const char *nbp, char *obj, char *type, char *zone)
{
    const char *colon = strchr(nbp, ':'), *at = strrchr(nbp, '@');
    snprintf(obj, 33, "%.*s", (int)(colon - nbp), nbp);
    snprintf(type, 33, "%.*s", (int)(at - colon - 1), colon + 1);
    snprintf(zone, 33, "%s", at + 1);
}

/* Send PostScript to a printer by NBP name; false with the reason. */
bool pap_send(const char *nbp, const char *ps, size_t len, char *err, size_t errlen)
{
    if (!appletalk_enabled() || at_open() != noErr) {
        snprintf(err, errlen, "AppleTalk is inactive");
        return false;
    }
    char obj[33], type[33], zone[33];
    split_nbp(nbp, obj, type, zone);
    uint16_t net;
    uint8_t node, sls;
    if (at_nbp_lookup(obj, type, zone, 4, 750, &net, &node, &sls) != noErr) {
        snprintf(err, errlen, "the printer \"%s\" isn't answering", obj);
        return false;
    }
    static pap_conn c;
    memset(&c, 0, sizeof c);
    c.node = node;
    c.data = ps;
    c.len = len;
    c.conn = (uint8_t)(1 + (ms() % 254));
    c.last_seq = 0xFFFF;
    int skt = atp_host_open(request, &c);
    if (!skt) {
        snprintf(err, errlen, "no AppleTalk socket");
        return false;
    }
    /* OpenConn, asked again while the printer is busy. */
    static atp_reply r;
    uint64_t start = ms();
    bool open = false;
    for (int wait = 0; !open && ms() - start < 120000; wait++) {
        uint8_t d[4] = { (uint8_t)skt, QUANTUM, (uint8_t)(wait >> 8), (uint8_t)wait };
        uint32_t user = (uint32_t)c.conn << 24 | (uint32_t)OpenConn << 16;
        int16_t e = atp_host_request(skt, node, sls, user, d, 4, true, 1, 2000, 4, &r);
        if (e != noErr || r.count < 1 || ((r.user[0] >> 16) & 0xFF) != OpenConnReply || r.len[0] < 4) {
            snprintf(err, errlen, "the printer \"%s\" didn't answer", obj);
            atp_host_close(skt);
            return false;
        }
        int result = (int16_t)(r.data[0][2] << 8 | r.data[0][3]);
        if (result == 0) {
            c.srv_skt = r.data[0][0];
            c.quantum = r.data[0][1] ? r.data[0][1] : 1;
            open = true;
        } else { /* busy: the status says why; again in a second or two */
            uint64_t until = ms() + 1500;
            while (ms() < until) {
                struct timespec ts = { 0, 5000000 };
                nanosleep(&ts, NULL);
                appletalk_poll();
            }
        }
    }
    if (!open) {
        snprintf(err, errlen, "the printer \"%s\" stayed busy", obj);
        atp_host_close(skt);
        return false;
    }
    LOGF(LOG_TRAPS, "  PAP: connected to %s (node %u socket %u, quantum %d), %zu bytes\n", nbp, node, c.srv_skt,
         c.quantum, len);
    /* The job goes as the printer asks for it; we read what it says back
     * until its EOF; tickles both ways. */
    static atp_reply rd;
    bool reading = false, printer_eof = false;
    uint16_t seq = 1;
    uint64_t last_tickle = ms();
    c.heard = ms();
    char said[512] = "";
    size_t nsaid = 0;
    while (c.srv_skt && !(c.eof_sent && printer_eof)) {
        if (!reading) {
            uint32_t user = (uint32_t)c.conn << 24 | (uint32_t)SendData << 16 | seq;
            if (atp_host_start(skt, node, c.srv_skt, user, NULL, 0, true, QUANTUM, 15000, -1, &rd) == noErr)
                reading = true;
        } else if (rd.result != ioInProgress) {
            reading = false;
            if (rd.result == noErr) {
                c.heard = ms();
                for (int i = 0; i < rd.count; i++) {
                    for (int k = 0; k < rd.len[i] && nsaid < sizeof said - 1; k++)
                        said[nsaid++] = rd.data[i][k] == '\r' ? '\n' : (char)rd.data[i][k];
                    if ((rd.user[i] >> 8) & 0xFF)
                        printer_eof = true;
                }
                seq = (uint16_t)(seq == 0xFFFF ? 1 : seq + 1);
            }
        }
        if (ms() - last_tickle > 60000) {
            atp_host_send_once(skt, node, c.srv_skt, (uint32_t)c.conn << 24 | (uint32_t)Tickle << 16, NULL, 0);
            last_tickle = ms();
        }
        if (ms() - c.heard > 120000) {
            snprintf(err, errlen, "the printer \"%s\" stopped answering", obj);
            break;
        }
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
        appletalk_poll();
    }
    if (reading)
        atp_host_cancel(&rd);
    said[nsaid] = 0;
    if (nsaid)
        LOGF(LOG_TRAPS, "  PAP: the printer said: %s\n", said);
    bool ok = c.eof_sent && printer_eof;
    if (c.srv_skt) {
        uint32_t user = (uint32_t)c.conn << 24 | (uint32_t)CloseConn << 16;
        atp_host_request(skt, node, c.srv_skt, user, NULL, 0, true, 1, 2000, 2, &r);
    }
    atp_host_close(skt);
    if (!ok && !err[0])
        snprintf(err, errlen, "the printer \"%s\" closed the connection", obj);
    return ok;
}

/* A job for the LaserWriter chosen: PostScript, over PAP. */
bool pap_print(const machine_print_job *job, const char *nbp)
{
    char *ps = NULL;
    size_t len = print_postscript(job, &ps);
    char err[256] = "";
    bool ok = len && pap_send(nbp, ps, len, err, sizeof err);
    bb_free(ps);
    if (!ok) {
        LOGF(LOG_UNIMPL, "PAP: %d page(s) not printed: %s\n", job->npages, err);
        char text[400];
        snprintf(text, sizeof text, "The document could not be printed on the LaserWriter: %s.", err);
        notify_host(0x5041, text);
    } else {
        LOGF(LOG_TRAPS, "  PAP: %d page(s) printed on %s\n", job->npages, nbp);
    }
    return ok;
}
