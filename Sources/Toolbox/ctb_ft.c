/*
 * The File Transfer Manager, with two tools of our own:
 *
 *   Text         a text file sent as it is; received, what comes is written
 *                until the transfer is stopped (or Timeout seconds pass
 *                with nothing)
 *   XMODEM Tool  XMODEM: checksum, CRC or 1K blocks (Method), sent and
 *                received, with retries and timeouts as Ward Christensen's
 *                protocol has them; the padding of the last block received
 *                is taken off
 *
 * The program calls FTExec while the transfer goes on (ftIsFTMode set);
 * the bytes go through its sendProc and come through its recvProc.  Files
 * are read and written by the File Manager, or by the program's readProc
 * and writeProc when it gives them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctb.h"

enum { ftNoErr = 0, ftRejected = 1, ftFailed = 2, ftTimeOut = 3, ftTooManyRetry = 4, ftNotEnoughDSpace = 5,
       ftRemoteCancel = 6, ftWrongFormat = 7, ftUserCancel = 9 };
enum { ftIsFTMode = 1 << 0, ftSucc = 1 << 7 };
enum { ftSameCircuit = 1 << 0, ftTextOnly = 1 << 3 };
enum { ftReceiving = 0, ftTransmitting = 1 };
enum { ftReadOpenFile = 0, ftReadDataFork = 1, ftReadAbort = 3, ftReadComplete = 4 };
enum { ftWriteOpenFile = 0, ftWriteDataFork = 1, ftWriteAbort = 3, ftWriteComplete = 4 };
enum { ftOpenDataFork = 1 };
enum { cmData = 1 };
enum { fsRdPerm = 1, fsRdWrPerm = 3, eofErr = -39, dupFNErr = -48 };
enum { T_TEXT = 1, T_XMODEM };
enum { MAX_FTS = 4, IOSIZE = 2048, MAX_SPECS = 32 };
/* XMODEM */
enum { SOH = 1, STX = 2, EOT = 4, ACK = 6, NAK = 0x15, CAN = 0x18, SUB = 0x1A, CRC_START = 'C' };
enum { X_START, X_BLOCK, X_ACK, X_EOT, X_DONE };
/* sendProc (thePtr, theSize, refCon, channel, flags): Size */
#define PI_SEND 0x0000AFF0u
/* recvProc (thePtr, theSize, refCon, channel, &flags): Size */
#define PI_RECV 0x0000EFF0u
/* readProc and writeProc (&count, pData, refCon, fileMsg): OSErr */
#define PI_FILE 0x00002FE0u
/* notifyProc (hFT, pFSSpec) */
#define PI_NOTIFY 0x000003C0u

/* ---------------------------------------------------------------------- */
/* The tools' configurations */

static const char *const methods[] = { "Checksum", "CRC", "1K", NULL };

enum { C_TIMEOUT = 4, TEXT_SIZE = 8 };
enum { C_METHOD = 4, C_RETRIES = 8, C_BLOCKWAIT = 12, C_TYPE = 16, C_CREATOR = 22, XMODEM_SIZE = 28 };

static const ctb_field text_fields[] = {
    { "Timeout", CF_INT, C_TIMEOUT, 0, 3600, NULL, "0" },
};
static const ctb_field xmodem_fields[] = {
    { "Method", CF_ENUM, C_METHOD, 0, 0, methods, "CRC" },
    { "Retries", CF_INT, C_RETRIES, 1, 99, NULL, "10" },
    { "Timeout", CF_INT, C_BLOCKWAIT, 1, 120, NULL, "10" },
    { "FileType", CF_STR, C_TYPE, 0, 4, NULL, "BINA" },
    { "Creator", CF_STR, C_CREATOR, 0, 4, NULL, "????" },
};

#define COUNT(a) (int)(sizeof a / sizeof a[0])
static const ctb_tool text_tool = { "Text", 0x74657874 /* 'text' */, TEXT_SIZE, text_fields, COUNT(text_fields) };
static const ctb_tool xmodem_tool = { "XMODEM Tool", 0x786D646D /* 'xmdm' */, XMODEM_SIZE, xmodem_fields,
                                      COUNT(xmodem_fields) };
static const ctb_tool *const ft_tools[] = { &text_tool, &xmodem_tool };

/* ---------------------------------------------------------------------- */
/* Transfers */

typedef struct {
    bool used;
    gaddr h;
    int tool;
    bool active, sending;
    /* the file */
    bool use_procs, file_open;
    int16_t refnum;
    gaddr spec;    /* the file's FSSpec (ours) */
    gaddr io;      /* guest buffer: the bytes sent and received, and the File Manager's */
    gaddr pb;
    uint64_t last_rx;
    /* bytes for the other end not yet taken by the sendProc */
    uint8_t out[1100];
    int outlen;
    /* XMODEM */
    int xstate;
    bool crc, eof, got_block;
    int block, retries, starts, cans;
    uint64_t deadline;
    uint8_t pkt[1029];
    int pktlen, pktneed;
    uint8_t data[1024]; /* the block being sent, or held back to take off its padding */
    int datalen;
    uint8_t ahead[1024]; /* the file read ahead of the blocks sent */
    int aheadlen;
    bool file_end;
    bool held;
    /* FTSend's files, FTReceive's */
    gaddr notify, specs;
    int nspecs, spec_i;
    int16_t result;
} ftx;

static ftx fts[MAX_FTS];

static gaddr rec(ftx *f) { return rd32(f->h); }
static gaddr cfg(ftx *f) { return rd32(rec(f) + FTRecord_config_off); }
static int32_t refcon(ftx *f) { return (int32_t)rd32(rec(f) + FTRecord_refCon_off); }

static ftx *ft_of(gaddr h)
{
    if (!h || !rd32(h))
        return NULL;
    uint32_t i = rd32(rd32(h) + FTRecord_ftPrivate_off);
    if (i < 1 || i > MAX_FTS || !fts[i - 1].used || fts[i - 1].h != h)
        return NULL;
    return &fts[i - 1];
}

static uint32_t ostype(gaddr block, uint16_t off, const char *def)
{
    char s[8];
    ctb_config_str(block, off, s, sizeof s);
    if (!s[0])
        snprintf(s, sizeof s, "%s", def);
    while (strlen(s) < 4)
        strcat(s, " ");
    return fourcc(s);
}

/* ---------------------------------------------------------------------- */
/* The file */

static int16_t file_proc(ftx *f, uint32_t *count, gaddr data, int msg)
{
    gaddr r = rec(f);
    gaddr proc = rd32(r + (f->sending ? FTRecord_readProc_off : FTRecord_writeProc_off));
    gaddr cp = f->pb + 80;
    wr32(cp, *count);
    uint32_t args[4] = { cp, data, (uint32_t)refcon(f), (uint32_t)msg };
    int16_t e = (int16_t)ctb_callback(proc, PI_FILE, args, 4);
    *count = rd32(cp);
    return e;
}

static int16_t open_file(ftx *f)
{
    f->use_procs = rd32(rec(f) + (f->sending ? FTRecord_readProc_off : FTRecord_writeProc_off)) != 0;
    if (f->use_procs) {
        /* The program's own: told the file's name and volume. */
        gaddr p = f->pb;
        gmemset(p, 0, 80);
        wr32(p + IOParam_ioNamePtr_off, f->spec + FSSpec_name_off);
        wr16(p + IOParam_ioVRefNum_off, rd16(f->spec + FSSpec_vRefNum_off));
        uint32_t count = ftOpenDataFork;
        int16_t e = file_proc(f, &count, p, ftReadOpenFile);
        f->file_open = e == noErr;
        return e;
    }
    if (!f->sending) {
        gaddr k = cfg(f);
        bool text = f->tool == T_TEXT;
        uint32_t type = text ? fourcc("TEXT") : ostype(k, C_TYPE, "BINA");
        uint32_t creator = text ? fourcc("ttxt") : ostype(k, C_CREATOR, "????");
        int16_t e = tb_FSpCreate(f->spec, (int32_t)creator, (int32_t)type, 0);
        if (e && e != dupFNErr)
            return e;
    }
    gaddr rn = f->pb + 84;
    int16_t e = tb_FSpOpenDF(f->spec, f->sending ? fsRdPerm : fsRdWrPerm, rn);
    if (e)
        return e;
    f->refnum = (int16_t)rd16(rn);
    f->file_open = true;
    if (!f->sending) {
        gmemset(f->pb, 0, 80);
        wr16(f->pb + IOParam_ioRefNum_off, (uint16_t)f->refnum);
        wr32(f->pb + IOParam_ioMisc_off, 0);
        tb_PBSetEOF(f->pb, false);
    }
    return noErr;
}

/* Up to n bytes of the file into the io buffer; how many (0 at its end). */
static int read_file(ftx *f, int n)
{
    if (!f->file_open)
        return 0;
    if (f->use_procs) {
        uint32_t count = (uint32_t)n;
        int16_t e = file_proc(f, &count, f->io, ftReadDataFork);
        return e && e != eofErr ? 0 : (int)count;
    }
    gaddr p = f->pb;
    gmemset(p, 0, 80);
    wr16(p + IOParam_ioRefNum_off, (uint16_t)f->refnum);
    wr32(p + IOParam_ioBuffer_off, f->io);
    wr32(p + IOParam_ioReqCount_off, (uint32_t)n);
    tb_PBRead(p, false);
    return (int)rd32(p + IOParam_ioActCount_off);
}

static bool write_file(ftx *f, const uint8_t *b, int n)
{
    if (!f->file_open || n <= 0)
        return f->file_open;
    gaddr buf = f->io + IOSIZE / 2;
    gput(buf, b, (uint32_t)n);
    if (f->use_procs) {
        uint32_t count = (uint32_t)n;
        return file_proc(f, &count, buf, ftWriteDataFork) == noErr;
    }
    gaddr p = f->pb;
    gmemset(p, 0, 80);
    wr16(p + IOParam_ioRefNum_off, (uint16_t)f->refnum);
    wr32(p + IOParam_ioBuffer_off, buf);
    wr32(p + IOParam_ioReqCount_off, (uint32_t)n);
    return tb_PBWrite(p, false) == noErr;
}

static void close_file(ftx *f, bool ok)
{
    if (!f->file_open)
        return;
    f->file_open = false;
    if (f->use_procs) {
        uint32_t count = 0;
        file_proc(f, &count, 0, ok ? ftReadComplete : ftReadAbort);
        return;
    }
    gmemset(f->pb, 0, 80);
    wr16(f->pb + IOParam_ioRefNum_off, (uint16_t)f->refnum);
    tb_PBClose(f->pb, false);
}

/* ---------------------------------------------------------------------- */
/* The other end */

/* What the sendProc takes of what is waiting to go. */
static void flush_out(ftx *f)
{
    if (!f->outlen)
        return;
    gaddr buf = f->io;
    gput(buf, f->out, (uint32_t)f->outlen);
    uint32_t args[5] = { buf, (uint32_t)f->outlen, (uint32_t)refcon(f), cmData, 0 };
    int32_t n = (int32_t)ctb_callback(rd32(rec(f) + FTRecord_sendProc_off), PI_SEND, args, 5);
    if (n <= 0)
        return;
    if (n > f->outlen)
        n = f->outlen;
    memmove(f->out, f->out + n, (size_t)(f->outlen - n));
    f->outlen -= n;
}

static void put_out(ftx *f, const uint8_t *b, int n)
{
    if (n > (int)sizeof f->out - f->outlen)
        n = (int)sizeof f->out - f->outlen;
    memcpy(f->out + f->outlen, b, (size_t)n);
    f->outlen += n;
    flush_out(f);
}

static void put_byte(ftx *f, uint8_t b) { put_out(f, &b, 1); }

/* What has come, up to n bytes, into b. */
static int receive(ftx *f, uint8_t *b, int n)
{
    gaddr buf = f->io, flags = f->pb + 88;
    wr16(flags, 0);
    uint32_t args[5] = { buf, (uint32_t)n, (uint32_t)refcon(f), cmData, flags };
    int32_t got = (int32_t)ctb_callback(rd32(rec(f) + FTRecord_recvProc_off), PI_RECV, args, 5);
    if (got <= 0)
        return 0;
    if (got > n)
        got = n;
    gget(b, buf, (uint32_t)got);
    return got;
}

/* ---------------------------------------------------------------------- */
/* Starting and finishing */

static void begin_file(ftx *f);

static void finish(ftx *f, int16_t err)
{
    close_file(f, err == ftNoErr);
    gaddr r = rec(f);
    if (f->notify && err == ftNoErr) {
        uint32_t args[2] = { f->h, f->spec };
        ctb_callback(f->notify, PI_NOTIFY, args, 2);
        f = ft_of(args[0]);
        if (!f)
            return;
        r = rec(f);
        if (f->sending && ++f->spec_i < f->nspecs) { /* FTSend's next file */
            gmemcpy(f->spec, f->specs + 70 * (uint32_t)f->spec_i, 70);
            begin_file(f);
            if (f->active)
                return;
            err = f->result;
        }
    }
    f->active = false;
    f->xstate = X_DONE;
    f->result = err;
    uint32_t flags = rd32(r + FTRecord_flags_off) & ~(uint32_t)ftIsFTMode;
    if (err == ftNoErr)
        flags |= ftSucc;
    wr32(r + FTRecord_flags_off, flags);
    wr16(r + FTRecord_errCode_off, (uint16_t)err);
}

static void begin_file(ftx *f)
{
    f->outlen = 0;
    f->pktlen = f->pktneed = 0;
    f->block = 1;
    f->retries = f->starts = f->cans = 0;
    f->eof = f->held = f->got_block = false;
    f->datalen = f->aheadlen = 0;
    f->file_end = false;
    f->last_rx = ctb_now_ms();
    f->active = true;
    f->result = ftNoErr;
    if (open_file(f) != noErr) {
        f->active = false;
        f->result = ftFailed;
        return;
    }
    f->xstate = X_START;
    f->deadline = 0; /* a receiver asks at once */
    if (f->sending)
        f->deadline = ctb_now_ms() + 60000; /* the receiver has a minute to ask */
}

static int16_t start(ftx *f, int direction)
{
    gaddr r = rec(f);
    f->sending = direction == ftTransmitting;
    wr16(r + FTRecord_direction_off, (uint16_t)direction);
    uint32_t flags = (rd32(r + FTRecord_flags_off) & ~(uint32_t)ftSucc) | ftIsFTMode;
    wr32(r + FTRecord_flags_off, flags);
    wr16(r + FTRecord_errCode_off, ftNoErr);
    begin_file(f);
    if (!f->active) {
        finish(f, ftFailed);
        return ftFailed;
    }
    return ftNoErr;
}

/* ---------------------------------------------------------------------- */
/* Text */

/* Each step says whether it got anywhere. */
static bool text_exec(ftx *f)
{
    if (f->sending) {
        flush_out(f);
        if (f->outlen)
            return false;
        int n = read_file(f, 512);
        if (n <= 0) {
            finish(f, ftNoErr);
            return false;
        }
        uint8_t b[512];
        gget(b, f->io, (uint32_t)n);
        put_out(f, b, n);
        return true;
    }
    uint8_t b[1024];
    int n = receive(f, b, sizeof b);
    uint64_t now = ctb_now_ms();
    if (n > 0) {
        f->last_rx = now;
        if (!write_file(f, b, n))
            finish(f, ftNotEnoughDSpace);
        return true;
    }
    int32_t quiet = (int32_t)rd32(cfg(f) + C_TIMEOUT);
    if (quiet > 0 && now - f->last_rx >= (uint64_t)quiet * 1000)
        finish(f, ftNoErr);
    return false;
}

/* ---------------------------------------------------------------------- */
/* XMODEM */

static uint16_t crc16(const uint8_t *b, int n)
{
    uint16_t crc = 0;
    for (int i = 0; i < n; i++) {
        crc ^= (uint16_t)(b[i] << 8);
        for (int k = 0; k < 8; k++)
            crc = crc & 0x8000 ? (uint16_t)(crc << 1 ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static uint8_t checksum(const uint8_t *b, int n)
{
    uint8_t s = 0;
    for (int i = 0; i < n; i++)
        s = (uint8_t)(s + b[i]);
    return s;
}

static void cancel(ftx *f, int16_t err)
{
    static const uint8_t cans[3] = { CAN, CAN, CAN };
    put_out(f, cans, 3);
    finish(f, err);
}

static uint64_t block_wait(ftx *f) { return (uint64_t)rd32(cfg(f) + C_BLOCKWAIT) * 1000; }
static int max_retries(ftx *f) { return (int)rd32(cfg(f) + C_RETRIES); }

/* The sender's next block, or EOT at the end of the file: 1K blocks while
 * more than 896 bytes are left (and the receiver asked for CRCs), 128-byte
 * blocks for the rest. */
static void send_block(ftx *f, bool again)
{
    uint64_t now = ctb_now_ms();
    if (!again) {
        while (f->aheadlen < 1024 && !f->file_end) {
            int n = read_file(f, 1024 - f->aheadlen);
            if (n <= 0)
                f->file_end = true;
            else {
                gget(f->ahead + f->aheadlen, f->io, (uint32_t)n);
                f->aheadlen += n;
            }
        }
        bool big = f->crc && rd16(cfg(f) + C_METHOD) == 2 && f->aheadlen > 896;
        int n = f->aheadlen < (big ? 1024 : 128) ? f->aheadlen : big ? 1024 : 128;
        f->eof = n == 0;
        memcpy(f->data, f->ahead, (size_t)n);
        memmove(f->ahead, f->ahead + n, (size_t)(f->aheadlen - n));
        f->aheadlen -= n;
        f->datalen = n;
    }
    if (f->eof) {
        put_byte(f, EOT);
        f->xstate = X_EOT;
        f->deadline = now + block_wait(f);
        return;
    }
    int size = f->datalen > 128 ? 1024 : 128;
    uint8_t pkt[1029];
    pkt[0] = size == 1024 ? STX : SOH;
    pkt[1] = (uint8_t)f->block;
    pkt[2] = (uint8_t)~f->block;
    memcpy(pkt + 3, f->data, (size_t)f->datalen);
    memset(pkt + 3 + f->datalen, SUB, (size_t)(size - f->datalen));
    int n = 3 + size;
    if (f->crc) {
        uint16_t c = crc16(pkt + 3, size);
        pkt[n++] = (uint8_t)(c >> 8);
        pkt[n++] = (uint8_t)c;
    } else {
        pkt[n++] = checksum(pkt + 3, size);
    }
    put_out(f, pkt, n);
    f->xstate = X_ACK;
    f->deadline = now + block_wait(f);
}

static bool xmodem_send(ftx *f)
{
    flush_out(f);
    uint8_t b[64];
    int n = receive(f, b, sizeof b);
    uint64_t now = ctb_now_ms();
    for (int i = 0; i < n && f->active; i++) {
        uint8_t c = b[i];
        if (c == CAN) {
            if (++f->cans >= 2)
                finish(f, ftRemoteCancel);
            continue;
        }
        f->cans = 0;
        switch (f->xstate) {
        case X_START:
            if (c == CRC_START || c == NAK) {
                f->crc = c == CRC_START && rd16(cfg(f) + C_METHOD) != 0;
                if (c == CRC_START && !f->crc)
                    break; /* checksum only: wait for a NAK */
                send_block(f, false);
            }
            break;
        case X_ACK:
            if (c == ACK) {
                f->block = (f->block + 1) & 0xFF;
                f->retries = 0;
                send_block(f, false);
            } else if (c == NAK) {
                if (++f->retries > max_retries(f))
                    cancel(f, ftTooManyRetry);
                else
                    send_block(f, true);
            }
            break;
        case X_EOT:
            if (c == ACK)
                finish(f, ftNoErr);
            else if (c == NAK)
                put_byte(f, EOT);
            break;
        }
    }
    if (!f->active || now < f->deadline)
        return n > 0;
    if (f->xstate == X_START) {
        cancel(f, ftTimeOut);
    } else if (++f->retries > max_retries(f)) {
        cancel(f, ftTimeOut);
    } else if (f->xstate == X_EOT) {
        put_byte(f, EOT);
        f->deadline = now + block_wait(f);
    } else {
        send_block(f, true);
    }
    return n > 0;
}

/* The block held back is written: all of it, or without its padding when
 * it was the last. */
static bool write_held(ftx *f, bool last)
{
    if (!f->held)
        return true;
    int n = f->datalen;
    if (last)
        while (n > 0 && f->data[n - 1] == SUB)
            n--;
    f->held = false;
    return write_file(f, f->data, n);
}

static void got_packet(ftx *f)
{
    uint8_t *p = f->pkt;
    int size = p[0] == STX ? 1024 : 128;
    bool ok = (uint8_t)(p[1] + p[2]) == 0xFF;
    if (ok && f->crc)
        ok = crc16(p + 3, size) == (uint16_t)(p[3 + size] << 8 | p[4 + size]);
    else if (ok)
        ok = checksum(p + 3, size) == p[3 + size];
    f->pktlen = f->pktneed = 0;
    uint64_t now = ctb_now_ms();
    f->deadline = now + block_wait(f);
    if (!ok) {
        if (++f->retries > max_retries(f))
            cancel(f, ftTooManyRetry);
        else
            put_byte(f, NAK);
        return;
    }
    f->retries = 0;
    if (p[1] == (uint8_t)(f->block - 1) && f->got_block) { /* our ACK was lost: again */
        put_byte(f, ACK);
        return;
    }
    if (p[1] != (uint8_t)f->block) {
        cancel(f, ftFailed);
        return;
    }
    if (!write_held(f, false)) {
        cancel(f, ftNotEnoughDSpace);
        return;
    }
    memcpy(f->data, p + 3, (size_t)size);
    f->datalen = size;
    f->held = true;
    f->got_block = true;
    f->block = (f->block + 1) & 0xFF;
    put_byte(f, ACK);
}

static bool xmodem_receive(ftx *f)
{
    flush_out(f);
    uint64_t now = ctb_now_ms();
    if (f->xstate == X_START && now >= f->deadline) {
        /* 'C' asks for CRC blocks; after three unanswered, NAK for checksums. */
        f->crc = rd16(cfg(f) + C_METHOD) != 0 && f->starts < 3;
        if (++f->starts > max_retries(f) + 3) {
            cancel(f, ftTimeOut);
            return false;
        }
        put_byte(f, f->crc ? CRC_START : NAK);
        f->deadline = now + 3000;
    }
    uint8_t b[1100];
    int n = receive(f, b, sizeof b);
    for (int i = 0; i < n && f->active; i++) {
        uint8_t c = b[i];
        if (f->pktneed) {
            f->pkt[f->pktlen++] = c;
            if (f->pktlen == f->pktneed)
                got_packet(f);
            continue;
        }
        switch (c) {
        case SOH:
        case STX:
            f->xstate = X_BLOCK;
            f->pkt[0] = c;
            f->pktlen = 1;
            f->pktneed = (c == STX ? 1024 : 128) + (f->crc ? 5 : 4);
            break;
        case EOT:
            if (write_held(f, true)) {
                put_byte(f, ACK);
                finish(f, ftNoErr);
            } else {
                cancel(f, ftNotEnoughDSpace);
            }
            break;
        case CAN:
            if (++f->cans >= 2)
                finish(f, ftRemoteCancel);
            break;
        default: f->cans = 0; break;
        }
    }
    if (!f->active || f->xstate == X_START)
        return n > 0;
    if (n > 0) {
        f->deadline = ctb_now_ms() + block_wait(f);
    } else if (ctb_now_ms() >= f->deadline) {
        if (++f->retries > max_retries(f)) {
            cancel(f, ftTimeOut);
            return false;
        }
        f->pktlen = f->pktneed = 0;
        put_byte(f, NAK);
        f->deadline = ctb_now_ms() + block_wait(f);
    }
    return n > 0;
}

/* ---------------------------------------------------------------------- */
/* The manager's calls */

static void ft_config_changed(gaddr h) {}

uint32_t ft_init(uint16_t sel, const uint32_t *a) { return ftNoErr; }

/* (procID, flags, sendProc, recvProc, readProc, writeProc, environsProc, owner, refCon, userData) */
uint32_t ft_new(uint16_t sel, const uint32_t *a)
{
    int16_t id = (int16_t)a[0];
    if (!ctb_tool_of(MGR_FT, id))
        return 0;
    int slot = -1;
    for (int i = 0; i < MAX_FTS && slot < 0; i++)
        if (!fts[i].used)
            slot = i;
    if (slot < 0)
        return 0;
    gaddr h = tb_NewHandle(FTRecord_SIZEOF, false, true);
    if (!h)
        return 0;
    ctb_record_init(h, MGR_FT, id, a[1] & ~(uint32_t)(ftIsFTMode | ftSucc), (int32_t)a[8], (int32_t)a[9]);
    ftx *f = &fts[slot];
    memset(f, 0, sizeof *f);
    f->used = true;
    f->h = h;
    f->tool = id;
    f->io = tb_NewPtr(IOSIZE, false, true);
    f->pb = tb_NewPtr(128, false, true);
    f->spec = tb_NewPtr(70, false, true);
    gaddr r = rd32(h);
    wr32(r + FTRecord_ftPrivate_off, (uint32_t)slot + 1);
    wr32(r + FTRecord_sendProc_off, a[2]);
    wr32(r + FTRecord_recvProc_off, a[3]);
    wr32(r + FTRecord_readProc_off, a[4]);
    wr32(r + FTRecord_writeProc_off, a[5]);
    wr32(r + FTRecord_environsProc_off, a[6]);
    wr32(r + FTRecord_owner_off, a[7]);
    wr32(r + FTRecord_theBuf_off, f->io);
    wr32(r + FTRecord_bufSize_off, IOSIZE);
    wr16(r + FTRecord_attributes_off, id == T_TEXT ? ftSameCircuit | ftTextOnly : ftSameCircuit);
    return h;
}

static void free_ft(ftx *f)
{
    if (f->active)
        close_file(f, false);
    tb_DisposePtr(f->io);
    tb_DisposePtr(f->pb);
    tb_DisposePtr(f->spec);
    if (f->specs)
        tb_DisposePtr(f->specs);
    f->used = false;
    ctb_record_free(f->h);
}

uint32_t ft_dispose(uint16_t sel, const uint32_t *a)
{
    ftx *f = ft_of(a[0]);
    if (f)
        free_ft(f);
    return 0;
}

static gaddr ft_renew(gaddr h, int16_t proc_id)
{
    gaddr r = rd32(h);
    uint32_t args[10] = { (uint32_t)proc_id, rd32(r + FTRecord_flags_off), rd32(r + FTRecord_sendProc_off),
                          rd32(r + FTRecord_recvProc_off), rd32(r + FTRecord_readProc_off),
                          rd32(r + FTRecord_writeProc_off), rd32(r + FTRecord_environsProc_off),
                          rd32(r + FTRecord_owner_off), rd32(r + FTRecord_refCon_off), rd32(r + FTRecord_userData_off) };
    gaddr nh = ft_new(0x0208, args);
    if (nh) {
        ftx *f = ft_of(h);
        if (f)
            free_ft(f);
    }
    return nh;
}

static void ft_dispose_record(gaddr h)
{
    ftx *f = ft_of(h);
    if (f)
        free_ft(f);
}

const ctb_manager ft_manager = { ft_tools, COUNT(ft_tools), ft_config_changed, ft_renew, ft_dispose_record };

/* The file an SFReply names, as an FSSpec of ours. */
static void spec_from_reply(ftx *f, gaddr reply)
{
    gmemset(f->spec, 0, 70);
    gaddr name = reply ? reply + SFReply_fName_off : 0;
    gaddr fallback = f->pb + 96;
    if (!name || !rd8(name)) {
        static const char *const untitled = "Received File";
        wr8(fallback, (uint8_t)strlen(untitled));
        gput(fallback + 1, untitled, (uint32_t)strlen(untitled));
        name = fallback;
    }
    tb_FSMakeFSSpec(reply ? (int16_t)rd16(reply + SFReply_vRefNum_off) : 0, 0, name, f->spec);
}

/* (hFT, direction, fileInfo) */
uint32_t ft_start(uint16_t sel, const uint32_t *a)
{
    ftx *f = ft_of(a[0]);
    if (!f)
        return (uint32_t)ftFailed;
    if (f->active)
        return (uint32_t)ftRejected;
    if ((int16_t)a[1] == ftTransmitting && !a[2])
        return (uint32_t)ftFailed; /* nothing to send */
    if (a[2])
        gmemcpy(rec(f) + FTRecord_theReply_off, a[2], SFReply_SIZEOF);
    spec_from_reply(f, a[2]);
    f->notify = 0;
    f->nspecs = 0;
    return (uint32_t)(int32_t)start(f, (int16_t)a[1]);
}

/* (hFT, numFiles, pFSSpec array, notifyProc) */
uint32_t ft_send(uint16_t sel, const uint32_t *a)
{
    ftx *f = ft_of(a[0]);
    int n = (int16_t)a[1];
    if (!f || !a[2] || n < 1)
        return (uint32_t)ftFailed;
    if (f->active)
        return (uint32_t)ftRejected;
    if (n > MAX_SPECS)
        n = MAX_SPECS;
    if (f->specs)
        tb_DisposePtr(f->specs);
    f->specs = tb_NewPtr(70 * n, false, false);
    gmemcpy(f->specs, a[2], 70 * (uint32_t)n);
    f->nspecs = n;
    f->spec_i = 0;
    f->notify = a[3];
    gmemcpy(f->spec, a[2], 70);
    return (uint32_t)(int32_t)start(f, ftTransmitting);
}

/* (hFT, pFSSpec, notifyProc) */
uint32_t ft_receive(uint16_t sel, const uint32_t *a)
{
    ftx *f = ft_of(a[0]);
    if (!f || !a[1])
        return (uint32_t)ftFailed;
    if (f->active)
        return (uint32_t)ftRejected;
    gmemcpy(f->spec, a[1], 70);
    f->notify = a[2];
    f->nspecs = 0;
    return (uint32_t)(int32_t)start(f, ftReceiving);
}

uint32_t ft_exec(uint16_t sel, const uint32_t *a)
{
    ftx *f = ft_of(a[0]);
    if (!f || !f->active)
        return 0;
    /* As long as there is something to do, but not for ever. */
    for (int i = 0; i < 32 && f && f->active; i++) {
        bool more = f->tool == T_TEXT ? text_exec(f) : f->sending ? xmodem_send(f) : xmodem_receive(f);
        f = ft_of(a[0]);
        if (!more)
            break;
    }
    return 0;
}

/* (hFT): the transfer stopped; a text capture ends as it should */
uint32_t ft_abort(uint16_t sel, const uint32_t *a)
{
    ftx *f = ft_of(a[0]);
    if (!f || !f->active)
        return ftNoErr;
    if (f->tool == T_TEXT && !f->sending)
        finish(f, ftNoErr);
    else if (f->tool == T_XMODEM)
        cancel(f, ftUserCancel);
    else
        finish(f, ftUserCancel);
    return ftNoErr;
}

void ft_reset_all(void) { memset(fts, 0, sizeof fts); }
