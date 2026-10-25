/*
 * The Data Access Manager (_Pack13, $A82F), as System 7 had it with no
 * database extension ('ddev') installed: nothing to open a session with.
 *
 * InitDBPack must come first (rcDBPackNotInited before it).  DBInit and
 * DBStartQuery find no ddev (rcDBBadDDEV); the calls on a session find
 * none open (rcDBBadSessID).  What needs no ddev works: result handlers
 * are installed, removed and found, DBResultsToText turns a results record
 * into text through them (or the built-in ones for the standard types),
 * and DBDisposeQuery disposes of a query record and its handles.
 * DBGetNewQuery reads the 'qrsc' resource it names.
 */
#include <stdio.h>
#include <string.h>

#include "legacy.h"
#include "tb_internal.h"

enum {
    rcDBError = -802, rcDBBadSessID = -806, rcDBBadSessNum = -807, rcDBBadDDEV = -808, rcDBBadAsyncPB = -810,
    rcDBNoHandler = -811, rcDBPackNotInited = -813,
};

static bool inited;

enum { MAX_HANDLERS = 32 };
static struct {
    uint32_t type;
    gaddr proc;
    bool sys;
} handlers[MAX_HANDLERS];
static int nhandlers;

void dbaccess_reset(void)
{
    inited = false;
    nhandlers = 0;
}

/* An asynchronous call's parameter block gets the result too, and its
 * completion routine (A1 the block) is called. */
static int16_t finish(gaddr asyncPB, int16_t r)
{
    if (asyncPB) {
        wr16(asyncPB + 4, (uint16_t)r);
        gaddr done = rd32(asyncPB);
        if (done) {
            cpu68k *c = g_cpu;
            uint32_t a1 = c->a[1];
            c->a[1] = asyncPB;
            guest_call(done);
            c->a[1] = a1;
        }
    }
    return r;
}

static int16_t no_session(gaddr asyncPB) { return finish(asyncPB, inited ? rcDBBadSessID : rcDBPackNotInited); }

int16_t tb_InitDBPack(int16_t version)
{
    inited = true;
    return noErr;
}

int16_t tb_DBInit(gaddr sessID, gaddr ddevName, gaddr host, gaddr user, gaddr passwd, gaddr connStr,
                  gaddr asyncPB)
{
    if (sessID)
        wr32(sessID, 0);
    return finish(asyncPB, inited ? rcDBBadDDEV : rcDBPackNotInited);
}

int16_t tb_DBEnd(int32_t sessID, gaddr asyncPB) { return no_session(asyncPB); }

int16_t tb_DBGetConnInfo(int32_t sessID, int16_t sessNum, gaddr returnedID, gaddr version, gaddr ddevName,
                         gaddr host, gaddr user, gaddr network, gaddr connStr, gaddr start, gaddr state,
                         gaddr asyncPB)
{
    if (!inited)
        return finish(asyncPB, rcDBPackNotInited);
    return finish(asyncPB, sessID ? rcDBBadSessID : rcDBBadSessNum);
}

int16_t tb_DBGetSessionNum(int32_t sessID, gaddr sessNum, gaddr asyncPB) { return no_session(asyncPB); }
int16_t tb_DBSend(int32_t sessID, gaddr text, int16_t len, gaddr asyncPB) { return no_session(asyncPB); }
int16_t tb_DBSendItem(int32_t sessID, int32_t dataType, int16_t len, int16_t places, int16_t flags, gaddr buffer,
                      gaddr asyncPB)
{
    return no_session(asyncPB);
}
int16_t tb_DBExec(int32_t sessID, gaddr asyncPB) { return no_session(asyncPB); }
int16_t tb_DBState(int32_t sessID, gaddr asyncPB) { return no_session(asyncPB); }
int16_t tb_DBGetErr(int32_t sessID, gaddr err1, gaddr err2, gaddr item1, gaddr item2, gaddr errorMsg,
                    gaddr asyncPB)
{
    return no_session(asyncPB);
}
int16_t tb_DBBreak(int32_t sessID, int8_t abort, gaddr asyncPB) { return no_session(asyncPB); }
int16_t tb_DBGetItem(int32_t sessID, int32_t timeout, gaddr dataType, gaddr len, gaddr places, gaddr flags,
                     gaddr buffer, gaddr asyncPB)
{
    return no_session(asyncPB);
}
int16_t tb_DBUnGetItem(int32_t sessID, gaddr asyncPB) { return no_session(asyncPB); }

/* DBKill: no asynchronous call is ever pending. */
int16_t tb_DBKill(gaddr asyncPB) { return inited ? rcDBBadAsyncPB : rcDBPackNotInited; }

int16_t tb_DBGetQueryResults(int32_t sessID, gaddr results, int32_t timeout, gaddr statusProc, gaddr asyncPB)
{
    return no_session(asyncPB);
}

/* ---- Query records ---- */

/* QueryRecord */
enum {
    qr_queryProc = 4, qr_ddevName = 8, qr_currQuery = 1096, qr_numQueries = 1098, qr_queryList = 1100,
    qr_numRes = 1104, qr_resList = 1106, qr_dataHandle = 1110, qr_size = 1118,
};

/* DBGetNewQuery: the 'qrsc' resource names the query; with no ddev to
 * run it on, one that is there still can't start a session. */
int16_t tb_DBGetNewQuery(int16_t queryID, gaddr query)
{
    if (!inited)
        return rcDBPackNotInited;
    if (query)
        wr32(query, 0);
    gaddr h = tb_GetResource(fourcc("qrsc"), queryID);
    return h ? rcDBError : resNotFound;
}

int16_t tb_DBDisposeQuery(gaddr query)
{
    if (!inited)
        return rcDBPackNotInited;
    if (!query || !rd32(query))
        return nilHandleErr;
    gaddr q = rd32(query);
    gaddr list = rd32(q + qr_queryList);
    if (list && rd32(list)) {
        int16_t n = (int16_t)rd16(q + qr_numQueries);
        for (int i = 0; i < n; i++)
            if (rd32(rd32(list) + 4 * (uint32_t)i))
                tb_DisposeHandle(rd32(rd32(list) + 4 * (uint32_t)i));
    }
    gaddr hs[] = { rd32(q + qr_queryProc), list, rd32(q + qr_resList), rd32(q + qr_dataHandle) };
    for (unsigned i = 0; i < sizeof hs / sizeof hs[0]; i++)
        if (hs[i] && (i != 0 || !(tb_HGetState(hs[i]) & 0x20))) /* a qdef from a resource stays */
            tb_DisposeHandle(hs[i]);
    tb_DisposeHandle(query);
    return noErr;
}

/* DBStartQuery: the query's ddev isn't there. */
int16_t tb_DBStartQuery(gaddr sessID, gaddr query, gaddr statusProc, gaddr asyncPB)
{
    if (sessID)
        wr32(sessID, 0);
    return finish(asyncPB, inited ? rcDBBadDDEV : rcDBPackNotInited);
}

/* ---- Result handlers ---- */

static int find_handler(uint32_t type, bool sys)
{
    for (int i = 0; i < nhandlers; i++)
        if (handlers[i].type == type && handlers[i].sys == sys)
            return i;
    return -1;
}

int16_t tb_DBInstallResultHandler(int32_t dataType, gaddr theHandler, int8_t isSysHandler)
{
    if (!inited)
        return rcDBPackNotInited;
    int i = find_handler(dataType, isSysHandler != 0);
    if (i < 0) {
        if (nhandlers == MAX_HANDLERS)
            return memFullErr;
        i = nhandlers++;
    }
    handlers[i].type = (uint32_t)dataType;
    handlers[i].proc = theHandler;
    handlers[i].sys = isSysHandler != 0;
    return noErr;
}

/* Removes the application's handler for the type (system ones stay). */
int16_t tb_DBRemoveResultHandler(int32_t dataType)
{
    if (!inited)
        return rcDBPackNotInited;
    int i = find_handler(dataType, false);
    if (i < 0)
        return rcDBNoHandler;
    handlers[i] = handlers[--nhandlers];
    return noErr;
}

int16_t tb_DBGetResultHandler(int32_t dataType, gaddr theHandler, int8_t getSysHandler)
{
    if (!inited)
        return rcDBPackNotInited;
    int i = find_handler(dataType, getSysHandler != 0);
    if (theHandler)
        wr32(theHandler, i < 0 ? 0 : handlers[i].proc);
    return i < 0 ? rcDBNoHandler : noErr;
}

/* The standard types' text, as the system's handlers made it; false for
 * a type with no built-in handler. */
static bool builtin_text(uint32_t type, gaddr data, int len, int places, char *out, size_t cap)
{
    out[0] = 0;
    if (type == fourcc("TEXT") || type == fourcc("vcha") || type == fourcc("lcha") || type == fourcc("deci") ||
        type == fourcc("mone")) {
        size_t n = (size_t)len < cap - 1 ? (size_t)len : cap - 1;
        gget(out, data, (uint32_t)n);
        out[n] = 0;
    } else if (type == fourcc("bool")) {
        snprintf(out, cap, "%s", rd8(data) ? "TRUE" : "FALSE");
    } else if (type == fourcc("shor")) {
        snprintf(out, cap, "%d", (int16_t)rd16(data));
    } else if (type == fourcc("long")) {
        snprintf(out, cap, "%d", (int32_t)rd32(data));
    } else if (type == fourcc("sing")) {
        uint32_t b = rd32(data);
        float f;
        memcpy(&f, &b, 4);
        snprintf(out, cap, "%.*f", places, (double)f);
    } else if (type == fourcc("doub")) {
        uint64_t b = (uint64_t)rd32(data) << 32 | rd32(data + 4);
        double d;
        memcpy(&d, &b, 8);
        snprintf(out, cap, "%.*f", places, d);
    } else if (type == fourcc("disc") || type == fourcc("none")) {
    } else {
        return false;
    }
    return true;
}

static void append(gaddr text, const char *s, uint32_t n)
{
    uint32_t at = (uint32_t)tb_GetHandleSize(text);
    tb_SetHandleSize(text, (int32_t)(at + n));
    gput(rd32(text) + at, s, n);
}

/* ResultsRecord: numRows, numCols, colTypes, colData, colInfo.  Columns
 * are separated by tabs, rows end in returns; each item's data is padded
 * to an even length. */
int16_t tb_DBResultsToText(gaddr results, gaddr theText)
{
    if (!inited)
        return rcDBPackNotInited;
    if (!results || !theText)
        return paramErr;
    int rows = (int16_t)rd16(results), cols = (int16_t)rd16(results + 2);
    gaddr types = rd32(results + 4), data = rd32(results + 8), info = rd32(results + 12);
    gaddr text = tb_NewHandle(0, false, false);
    if (!text)
        return memFullErr;
    wr32(theText, text);
    if (rows <= 0 || cols <= 0 || !types || !data || !info)
        return noErr;
    uint32_t at = 0;
    for (int r = 0; r < rows; r++)
        for (int k = 0; k < cols; k++) {
            int item = r * cols + k;
            uint32_t type = rd32(rd32(types) + 4 * (uint32_t)k);
            gaddr ci = rd32(info) + 6 * (uint32_t)item;
            int len = (int16_t)rd16(ci), places = (int16_t)rd16(ci + 2), flags = (int16_t)rd16(ci + 4);
            gaddr p = rd32(data) + at;
            int h = find_handler(type, false);
            if (h < 0)
                h = find_handler(type, true);
            if (h >= 0) {
                /* pascal OSErr (type, len, places, flags, data, text) */
                cpu68k *c = g_cpu;
                cpu_push16(c, 0);
                cpu_push32(c, type);
                cpu_push16(c, (uint16_t)len);
                cpu_push16(c, (uint16_t)places);
                cpu_push16(c, (uint16_t)flags);
                cpu_push32(c, p);
                cpu_push32(c, text);
                guest_call(handlers[h].proc);
                int16_t err = (int16_t)cpu_pop16(c);
                if (err)
                    return err;
            } else {
                char s[256];
                if (!(flags & 4) && !builtin_text(type, p, len, places, s, sizeof s)) /* kDBNullFlag: empty */
                    return rcDBNoHandler;
                if (flags & 4)
                    s[0] = 0;
                append(text, s, (uint32_t)strlen(s));
            }
            append(text, k == cols - 1 ? "\r" : "\t", 1);
            at += (uint32_t)(len + (len & 1));
        }
    return noErr;
}
