/*
 * The Network Services Location manager (NSLCore.h and NSL.h): the client
 * side that programs call to find servers and zones, with the helpers for
 * its data: neighborhoods, service lists, typed request blocks and the
 * URLs and neighborhoods a search leaves in the client's buffer.
 *
 * NSL found things through plug-ins (AppleTalk, SLP, DNS); there are none
 * here, so a client can open the API, make requests and use every helper,
 * and a lookup says kNSLNoPluginsForSearch, as the manager did without a
 * plug-in for the protocol asked.  Registering a URL finds no plug-in
 * either (kNSLNoPluginsFound), and the standard dialog, with nothing to
 * choose from, is cancelled.
 *
 * A neighborhood is a Pascal string of its name followed by a Pascal string
 * of its protocols; the buffers searches fill hold neighborhoods one after
 * the other, or URLs separated by commas.  Parameter blocks are packed to
 * two bytes (NSLClientAsyncInfo is 58 bytes).  An NSLError, 8 bytes, comes
 * back through the pointer in r3 and goes in as two words.
 */
#include <ctype.h>
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>

#include "carbonrest.h"
#include "filemgr_internal.h"

enum {
    kNSLSchedulerError = -4171,
    kNSLBadURLSyntax = -4172,
    kNSLErrNullPtrError = -4176,
    kNSLNullNeighborhoodPtr = -4178,
    kNSLNoPluginsForSearch = -4179,
    kNSLSearchAlreadyInProgress = -4180,
    kNSLNoPluginsFound = -4181,
    kNSLBadProtocolTypeErr = -4183,
    kNSLNullListPtr = -4184,
    kNSLBadClientInfoPtr = -4185,
    kNSLCannotContinueLookup = -4186,
    kNSLBufferTooSmallForData = -4187,
    kNSLBadDataTypeErr = -4193,
    kNSLBadServiceTypeErr = -4194,
    kNSLBadReferenceErr = -4195,
    kNSLNoElementsInList = -4196,
    userCanceledErr = -128,
};
enum { kNSLSearchStateComplete = 3, kNSLWaitingForContinue = 5 };
enum { kNSLServicesLookupDataEvent = 6, kNSLNeighborhoodLookupDataEvent = 7, kNSLNewDataEvent = 8 };
enum { ASYNC_SIZE = 58, DIALOG_OPTIONS_SIZE = 1030 };
enum { CLIENT_TAG = 0x4E534C00, REQUEST_TAG = 0x4E534D00 };
enum { MAX_CLIENTS = 16, MAX_REQUESTS = 64 };

typedef struct {
    bool used;
    uint32_t client, notifier;
    gaddr context, buffer, info;
    uint32_t buflen;
    bool searching;
} request;
static bool clients[MAX_CLIENTS];
static request requests[MAX_REQUESTS];

void rest_nsl_reset(void)
{
    memset(clients, 0, sizeof clients);
    memset(requests, 0, sizeof requests);
}

/* An NSLError result: through the pointer in r3. */
#define ret_error(c, err)                                                                                              \
    do {                                                                                                               \
        int32_t e_ = (err);                                                                                            \
        gaddr out_ = A(0);                                                                                             \
        if (out_) {                                                                                                    \
            wr32(out_, (uint32_t)e_);                                                                                  \
            wr32(out_ + 4, 0);                                                                                         \
        }                                                                                                              \
        RET(out_);                                                                                                     \
    } while (0)

static bool client_ok(uint32_t ref) { return ref - CLIENT_TAG >= 1 && ref - CLIENT_TAG <= MAX_CLIENTS && clients[ref - CLIENT_TAG - 1]; }

static request *request_of(uint32_t ref)
{
    uint32_t i = ref - REQUEST_TAG;
    return i >= 1 && i <= MAX_REQUESTS && requests[i - 1].used ? &requests[i - 1] : NULL;
}

/* ---- Text: hex encoding ---- */

static bool needs_escape(uint8_t ch)
{
    return ch <= ' ' || ch >= 0x7F || strchr("\"%<>[\\]^`{|},", ch) != NULL;
}

/* NSLHexEncodeText(raw, rawLen, buffer, &bufferLen, &changed) */
static void nsl_HexEncodeText(ppc_cpu *c)
{
    gaddr in = A(0), out = A(2), lenp = A(3), changed = A(4);
    uint32_t n = U16(1), room = lenp ? rd16(lenp) : 0, k = 0;
    if ((!in && n) || !out || !lenp)
        RET(kNSLErrNullPtrError);
    bool ch = false;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b = rd8(in + i);
        uint32_t need = needs_escape(b) ? 3 : 1;
        if (k + need > room)
            RET(kNSLBufferTooSmallForData);
        if (need == 3) {
            static const char hex[] = "0123456789ABCDEF";
            wr8(out + k, '%');
            wr8(out + k + 1, (uint8_t)hex[b >> 4]);
            wr8(out + k + 2, (uint8_t)hex[b & 15]);
            ch = true;
        } else
            wr8(out + k, b);
        k += need;
    }
    wr16(lenp, (uint16_t)k);
    if (changed)
        wr8(changed, ch);
    RET(noErr);
}

/* NSLHexDecodeText(encoded, encodedLen, buffer, &bufferLen, &changed) */
static void nsl_HexDecodeText(ppc_cpu *c)
{
    gaddr in = A(0), out = A(2), lenp = A(3), changed = A(4);
    uint32_t n = U16(1), room = lenp ? rd16(lenp) : 0, k = 0;
    if ((!in && n) || !out || !lenp)
        RET(kNSLErrNullPtrError);
    bool ch = false;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b = rd8(in + i);
        if (b == '%' && i + 2 < n && isxdigit(rd8(in + i + 1)) && isxdigit(rd8(in + i + 2))) {
            char h[3] = { (char)rd8(in + i + 1), (char)rd8(in + i + 2), 0 };
            b = (uint8_t)strtol(h, NULL, 16);
            i += 2;
            ch = true;
        }
        if (k + 1 > room)
            RET(kNSLBufferTooSmallForData);
        wr8(out + k++, b);
    }
    wr16(lenp, (uint16_t)k);
    if (changed)
        wr8(changed, ch);
    RET(noErr);
}

/* ---- Services lists: a handle with a header (count, length) and the
 * service types, comma separated ---- */

enum { LIST_HEADER = 8 };

static bool list_ok(gaddr h) { return h && rd32(h) && tb_GetHandleSize(h) >= LIST_HEADER; }

static int32_t list_add(gaddr h, uint32_t sp, uint32_t n)
{
    int32_t size = tb_GetHandleSize(h);
    uint32_t count = rd32(rd32(h));
    uint32_t extra = n + (count ? 1 : 0);
    tb_SetHandleSize(h, size + (int32_t)extra);
    if (tb_GetHandleSize(h) != size + (int32_t)extra)
        return memFullErr;
    gaddr p = rd32(h);
    uint32_t at = (uint32_t)size;
    if (count)
        wr8(p + at++, ',');
    for (uint32_t i = 0; i < n; i++)
        wr8(p + at + i, rd8(sp + i));
    wr32(p, count + 1);
    wr32(p + 4, (uint32_t)size + extra);
    return noErr;
}

/* NSLMakeNewServicesList(initialServiceList): from "afp,ftp", or none. */
static void nsl_MakeNewServicesList(ppc_cpu *c)
{
    gaddr h = tb_NewHandle(LIST_HEADER, false, true);
    if (!h)
        RET(0);
    wr32(rd32(h) + 4, LIST_HEADER);
    gaddr s = A(0);
    for (uint32_t i = 0; s && rd8(s + i);) {
        uint32_t j = i;
        while (rd8(s + j) && rd8(s + j) != ',')
            j++;
        if (j > i && list_add(h, s + i, j - i)) {
            tb_DisposeHandle(h);
            RET(0);
        }
        i = rd8(s + j) ? j + 1 : j;
    }
    RET(h);
}

/* NSLAddServiceToServicesList(list, serviceType): NSLError */
static void nsl_AddServiceToServicesList(ppc_cpu *c)
{
    gaddr h = A(1), s = A(2);
    if (!h)
        ret_error(c, kNSLNullListPtr);
    if (!list_ok(h) || !s || !rd8(s))
        ret_error(c, s ? kNSLBadServiceTypeErr : kNSLErrNullPtrError);
    uint32_t n = 0;
    while (rd8(s + n) && rd8(s + n) != ',')
        n++;
    ret_error(c, list_add(h, s, n));
}

static void nsl_DisposeServicesList(ppc_cpu *c)
{
    if (A(0))
        tb_DisposeHandle(A(0));
    DONE();
}

/* NSLServiceIsInServiceList(list, svcToFind) */
static void nsl_ServiceIsInServiceList(ppc_cpu *c)
{
    gaddr h = A(0), s = A(1);
    if (!list_ok(h) || !s)
        RET(0);
    uint32_t total = rd32(rd32(h) + 4), want = 0;
    while (rd8(s + want))
        want++;
    gaddr p = rd32(h);
    for (uint32_t i = LIST_HEADER; i < total;) {
        uint32_t j = i;
        while (j < total && rd8(p + j) != ',')
            j++;
        if (j - i == want) {
            uint32_t k = 0;
            while (k < want && tolower(rd8(p + i + k)) == tolower(rd8(s + k)))
                k++;
            if (k == want)
                RET(1);
        }
        i = j + 1;
    }
    RET(0);
}

/* ---- Typed request blocks: NSLTypedData (type, length) and the data ---- */

/* NSLMakeServicesRequestPB(list, &newDataPtr) */
static void nsl_MakeServicesRequestPB(ppc_cpu *c)
{
    gaddr h = A(0), out = A(1);
    if (!out)
        RET(kNSLErrNullPtrError);
    wr32(out, 0);
    if (!list_ok(h))
        RET(kNSLNullListPtr);
    uint32_t n = rd32(rd32(h) + 4) - LIST_HEADER;
    gaddr p = tb_NewPtr((int32_t)(8 + n), false, true);
    if (!p)
        RET(memFullErr);
    wr32(p, kNSLServicesLookupDataEvent);
    wr32(p + 4, n);
    for (uint32_t i = 0; i < n; i++)
        wr8(p + 8 + i, rd8(rd32(h) + LIST_HEADER + i));
    wr32(out, p);
    RET(noErr);
}

static void nsl_FreeTypedDataPtr(ppc_cpu *c)
{
    if (A(0))
        tb_DisposePtr(A(0));
    RET(0);
}

/* NSLParseServicesRequestPB(data, &serviceListPtr, &serviceListLen) */
static void nsl_ParseServicesRequestPB(ppc_cpu *c)
{
    gaddr d = A(0);
    if (!d || !A(1) || !A(2))
        RET(kNSLErrNullPtrError);
    if (rd32(d) != kNSLServicesLookupDataEvent)
        RET(kNSLBadDataTypeErr);
    wr32(A(1), d + 8);
    wr16(A(2), (uint16_t)rd32(d + 4));
    RET(noErr);
}

/* ---- Neighborhoods ---- */

static uint32_t neighborhood_length(gaddr n)
{
    if (!n)
        return 0;
    return 2u + rd8(n) + rd8(n + 1 + rd8(n));
}

static void nsl_MakeNewNeighborhood(ppc_cpu *c)
{
    gaddr name = A(0), protocols = A(1);
    uint32_t nl = 0, pl = 0;
    while (name && rd8(name + nl))
        nl++;
    while (protocols && rd8(protocols + pl))
        pl++;
    if (nl > 255 || pl > 255)
        RET(0);
    gaddr p = tb_NewPtr((int32_t)(2 + nl + pl), false, true);
    if (!p)
        RET(0);
    wr8(p, (uint8_t)nl);
    for (uint32_t i = 0; i < nl; i++)
        wr8(p + 1 + i, rd8(name + i));
    wr8(p + 1 + nl, (uint8_t)pl);
    for (uint32_t i = 0; i < pl; i++)
        wr8(p + 2 + nl + i, rd8(protocols + i));
    RET(p);
}

static void nsl_CopyNeighborhood(ppc_cpu *c)
{
    gaddr n = A(0);
    uint32_t len = neighborhood_length(n);
    gaddr p = len ? tb_NewPtr((int32_t)len, false, true) : 0;
    for (uint32_t i = 0; p && i < len; i++)
        wr8(p + i, rd8(n + i));
    RET(p);
}

static void nsl_FreeNeighborhood(ppc_cpu *c)
{
    if (A(0))
        tb_DisposePtr(A(0));
    RET(0);
}

static void nsl_GetNameFromNeighborhood(ppc_cpu *c)
{
    gaddr n = A(0);
    if (A(1))
        wr32(A(1), n ? n + 1 : 0);
    if (A(2))
        wr32(A(2), n ? rd8(n) : 0);
    DONE();
}

static void nsl_GetNeighborhoodLength(ppc_cpu *c) { RET(neighborhood_length(A(0))); }

/* NSLParseServiceRegistrationPB(data, &neighborhood, &neighborhoodLen, &url, &urlLen):
 * a registration's data is a neighborhood then the URL's text. */
static void nsl_ParseServiceRegistrationPB(ppc_cpu *c)
{
    gaddr d = A(0);
    if (!d || !A(1) || !A(2) || !A(3) || !A(4))
        RET(kNSLErrNullPtrError);
    if (rd32(d) != kNSLNewDataEvent)
        RET(kNSLBadDataTypeErr);
    uint32_t nl = neighborhood_length(d + 8), total = rd32(d + 4);
    if (nl > total)
        RET(kNSLBadDataTypeErr);
    wr32(A(1), d + 8);
    wr16(A(2), (uint16_t)nl);
    wr32(A(3), d + 8 + nl);
    wr16(A(4), (uint16_t)(total - nl));
    RET(noErr);
}

/* ---- URLs ---- */

/* NSLGetServiceFromURL(url, &svcString, &svcLen): "afp" of "afp://..." */
static void nsl_GetServiceFromURL(ppc_cpu *c)
{
    gaddr u = A(0);
    if (!u || !A(1) || !A(2))
        RET(kNSLErrNullPtrError);
    uint32_t n = 0;
    while (rd8(u + n) && rd8(u + n) != ':')
        n++;
    if (!rd8(u + n) || n == 0)
        RET(kNSLBadURLSyntax);
    wr32(A(1), u);
    wr16(A(2), (uint16_t)n);
    RET(noErr);
}

/* ---- Searches: no plug-ins ---- */

static void nsl_LibraryVersion(ppc_cpu *c) { RET(0x00000110); }

static void nsl_OpenNavigationAPI(ppc_cpu *c)
{
    if (!A(0))
        RET(kNSLErrNullPtrError);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (!clients[i]) {
            clients[i] = true;
            wr32(A(0), CLIENT_TAG + (uint32_t)i + 1);
            RET(noErr);
        }
    wr32(A(0), 0);
    RET(memFullErr);
}

static void drop_request(request *r)
{
    if (r->info)
        tb_DisposePtr(r->info);
    memset(r, 0, sizeof *r);
}

static void nsl_CloseNavigationAPI(ppc_cpu *c)
{
    uint32_t ref = A(0);
    if (!client_ok(ref))
        DONE();
    for (int i = 0; i < MAX_REQUESTS; i++)
        if (requests[i].used && requests[i].client == ref)
            drop_request(&requests[i]);
    clients[ref - CLIENT_TAG - 1] = false;
    DONE();
}

/* NSLPrepareRequest(notifier, contextPtr, client, &ref, bufPtr, bufLen, &infoPtr): NSLError */
static void nsl_PrepareRequest(ppc_cpu *c)
{
    uint32_t notifier = A(1), client = A(3);
    gaddr context = A(2), refp = A(4), buf = A(5), infop = A(7);
    uint32_t len = A(6);
    if (!refp || !infop || !buf)
        ret_error(c, kNSLErrNullPtrError);
    if (!client_ok(client))
        ret_error(c, kNSLBadReferenceErr);
    for (int i = 0; i < MAX_REQUESTS; i++)
        if (!requests[i].used) {
            gaddr info = tb_NewPtr(ASYNC_SIZE, false, true);
            if (!info)
                ret_error(c, memFullErr);
            requests[i] = (request){ true, client, notifier, context, buf, info, len, false };
            wr32(info, context);
            wr32(info + 8, buf);
            wr32(info + 16, len);
            wr16(info + 44, kNSLSearchStateComplete);
            wr32(refp, REQUEST_TAG + (uint32_t)i + 1);
            wr32(infop, info);
            ret_error(c, noErr);
        }
    ret_error(c, memFullErr);
}

/* The search that finds nothing, because no plug-in serves it. */
static int32_t no_plugins(request *r, gaddr info)
{
    if (info) {
        wr32(info + 12, 0);
        wr16(info + 44, kNSLSearchStateComplete);
        wr32(info + 46, (uint32_t)kNSLNoPluginsForSearch);
        wr32(info + 50, 0);
        wr32(info + 54, 0);
    }
    return kNSLNoPluginsForSearch;
}

/* NSLStartNeighborhoodLookup(ref, neighborhood, asyncInfo): NSLError */
static void nsl_StartNeighborhoodLookup(ppc_cpu *c)
{
    request *r = request_of(A(1));
    if (!r)
        ret_error(c, kNSLBadReferenceErr);
    if (!A(3))
        ret_error(c, kNSLBadClientInfoPtr);
    ret_error(c, no_plugins(r, A(3)));
}

/* NSLStartServicesLookup(ref, neighborhood, requestData, asyncInfo): NSLError */
static void nsl_StartServicesLookup(ppc_cpu *c)
{
    request *r = request_of(A(1));
    if (!r)
        ret_error(c, kNSLBadReferenceErr);
    if (!A(2))
        ret_error(c, kNSLNullNeighborhoodPtr);
    if (!A(3) || rd32(A(3)) != kNSLServicesLookupDataEvent)
        ret_error(c, A(3) ? kNSLBadDataTypeErr : kNSLErrNullPtrError);
    if (!A(4))
        ret_error(c, kNSLBadClientInfoPtr);
    ret_error(c, no_plugins(r, A(4)));
}

/* NSLContinueLookup(asyncInfo): only a search waiting to continue can. */
static void nsl_ContinueLookup(ppc_cpu *c)
{
    gaddr info = A(1);
    if (!info)
        ret_error(c, kNSLBadClientInfoPtr);
    ret_error(c, rd16(info + 44) == kNSLWaitingForContinue ? noErr : kNSLCannotContinueLookup);
}

static void nsl_CancelRequest(ppc_cpu *c)
{
    request *r = request_of(A(1));
    if (r && r->info)
        wr16(r->info + 44, kNSLSearchStateComplete);
    ret_error(c, r ? noErr : kNSLBadReferenceErr);
}

static void nsl_DeleteRequest(ppc_cpu *c)
{
    request *r = request_of(A(1));
    if (r)
        drop_request(r);
    ret_error(c, r ? noErr : kNSLBadReferenceErr);
}

/* NSLGetNextUrl(infoPtr, &url, &length): the next of the comma-separated
 * URLs in the result buffer. */
static void nsl_GetNextUrl(ppc_cpu *c)
{
    gaddr info = A(0);
    if (!info || !A(1) || !A(2))
        RET(0);
    uint32_t at = rd32(info + 4), total = rd32(info + 12);
    gaddr buf = rd32(info + 8);
    if (!buf || at >= total)
        RET(0);
    uint32_t j = at;
    while (j < total && rd8(buf + j) != ',')
        j++;
    wr32(A(1), buf + at);
    wr32(A(2), j - at);
    wr32(info + 4, j < total ? j + 1 : total);
    RET(1);
}

/* NSLGetNextNeighborhood(infoPtr, &neighborhood, &length): the next one packed in the buffer. */
static void nsl_GetNextNeighborhood(ppc_cpu *c)
{
    gaddr info = A(0);
    if (!info || !A(1) || !A(2))
        RET(0);
    uint32_t at = rd32(info + 4), total = rd32(info + 12);
    gaddr buf = rd32(info + 8);
    if (!buf || at + 2 > total)
        RET(0);
    uint32_t len = neighborhood_length(buf + at);
    if (at + len > total)
        RET(0);
    wr32(A(1), buf + at);
    wr32(A(2), len);
    wr32(info + 4, at + len);
    RET(1);
}

/* ---- Errors ---- */

static const struct {
    int32_t err;
    const char *message, *solution;
} messages[] = {
    { kNSLBadURLSyntax, "The URL contains characters that are not allowed.", "Check the URL and try again." },
    { kNSLErrNullPtrError, "A required pointer was not supplied.", "Contact the program's developer." },
    { kNSLNullNeighborhoodPtr, "No neighborhood was given.", "Choose a neighborhood and try again." },
    { kNSLNoPluginsForSearch, "No network service plug-in can search for that.",
      "Install a plug-in for the protocol in the Network Services Location folder." },
    { kNSLSearchAlreadyInProgress, "A search is already in progress.", "Wait for it to finish, or cancel it." },
    { kNSLNoPluginsFound, "No network service plug-ins were found.",
      "Install plug-ins in the Network Services Location folder." },
    { kNSLBufferTooSmallForData, "The buffer is too small for the results.", "Contact the program's developer." },
    { kNSLBadReferenceErr, "The reference given is not valid.", "Contact the program's developer." },
    { kNSLCannotContinueLookup, "The search cannot be continued.", "Start the search again." },
    { userCanceledErr, "The operation was canceled.", "" },
};

/* NSLErrorToString(theErr (two words), errorString, solutionString): C strings. */
static void nsl_ErrorToString(ppc_cpu *c)
{
    int32_t err = (int32_t)A(0);
    gaddr es = A(2), ss = A(3);
    if (!es || !ss)
        RET(kNSLErrNullPtrError);
    const char *m = "An unknown network error occurred.", *s = "";
    for (size_t i = 0; i < sizeof messages / sizeof *messages; i++)
        if (messages[i].err == err) {
            m = messages[i].message;
            s = messages[i].solution;
        }
    for (size_t i = 0; i <= strlen(m); i++)
        wr8(es + (uint32_t)i, (uint8_t)m[i]);
    for (size_t i = 0; i <= strlen(s); i++)
        wr8(ss + (uint32_t)i, (uint8_t)s[i]);
    RET(noErr);
}

/* NSLGetErrorStringsFromResource(theErr, fileSpec, errorResID, errorString,
 * solutionString): items 1 and 2 of the file's STR# of that ID, as C strings. */
static void nsl_GetErrorStringsFromResource(ppc_cpu *c)
{
    gaddr spec = A(1), es = A(3), ss = A(4);
    int16_t id = S16(2);
    if (!spec || !es || !ss)
        RET(kNSLErrNullPtrError);
    int16_t before = tb_CurResFile();
    int16_t rn = tb_FSpOpenResFile(spec, 1);
    if (rn < 0) {
        int16_t e = (int16_t)rd16(LM_ResErr);
        RET(e ? e : fnfErr);
    }
    gaddr h = tb_Get1Resource((int32_t)fourcc("STR#"), id);
    int32_t err = noErr;
    if (h)
        tb_LoadResource(h);
    if (!h || !rd32(h) || tb_GetHandleSize(h) < 4) {
        err = resNotFound;
    } else {
        gaddr p = rd32(h);
        uint16_t count = rd16(p);
        uint32_t at = 2;
        for (uint16_t i = 1; i <= 2; i++) {
            gaddr dst = i == 1 ? es : ss;
            uint32_t n = 0;
            if (i <= count && at < (uint32_t)tb_GetHandleSize(h)) {
                n = rd8(p + at);
                for (uint32_t k = 0; k < n; k++)
                    wr8(dst + k, rd8(p + at + 1 + k));
                at += 1 + n;
            }
            wr8(dst + n, 0);
        }
    }
    tb_CloseResFile(rn);
    if (before)
        tb_UseResFile(before);
    set_res_err(noErr);
    RET(err);
}

/* NSLStandardRegisterURL(url, neighborhood), NSLStandardDeregisterURL: NSLError;
 * no plug-in takes a registration. */
static void nsl_StandardRegisterURL(ppc_cpu *c)
{
    if (!A(1))
        ret_error(c, kNSLErrNullPtrError);
    if (!A(2))
        ret_error(c, kNSLNullNeighborhoodPtr);
    ret_error(c, kNSLNoPluginsFound);
}

/* ---- Threads (the Thread Manager's, kept in the manager's list) ---- */

static void nsl_NewThread(ppc_cpu *c)
{
    uint32_t a[7] = { A(0), A(1), A(2), A(3), A(4), A(5), A(6) };
    RET(svc_call_export("NewThread", a, 7));
}

static void nsl_DisposeThread(ppc_cpu *c)
{
    uint32_t a[3] = { A(0), A(1), A(2) };
    RET(svc_call_export("DisposeThread", a, 3));
}

/* ---- NSL.h: the standard dialog ---- */

/* NSLGetDefaultDialogOptions(options): version 0, no flags, default (empty) strings. */
static void nsl_GetDefaultDialogOptions(ppc_cpu *c)
{
    if (!A(0))
        RET(kNSLErrNullPtrError);
    gmemset(A(0), 0, DIALOG_OPTIONS_SIZE);
    RET(noErr);
}

/* NSLStandardGetURL(options, eventProc, ctx, filterProc, serviceTypeList, &url):
 * no service is listed to choose, so the dialog ends as the user's cancel. */
static void nsl_StandardGetURL(ppc_cpu *c)
{
    if (!A(0) || !A(5))
        RET(kNSLErrNullPtrError);
    wr32(A(5), 0);
    RET(userCanceledErr);
}

static void nsl_FreeURL(ppc_cpu *c)
{
    if (A(0))
        tb_DisposePtr(A(0));
    RET(0);
}

/* NSLSaveURLAliasToFolder(folderSelectorType, url, userFriendlyName): an Internet location
 * file in a standard folder: its data fork the URL, its type by the kind of URL, 'MACS' its creator. */
static void nsl_SaveURLAliasToFolder(ppc_cpu *c)
{
    char url[1024], name[64];
    if (!A(1) || !A(2))
        RET(kNSLErrNullPtrError);
    rest_cstr(A(1), url, sizeof url);
    rest_cstr(A(2), name, sizeof name);
    const char *colon = strchr(url, ':');
    if (!colon || colon == url)
        RET(kNSLBadURLSyntax);
    if (!name[0] || strlen(name) > 31 || strchr(name, ':'))
        RET(kNSLBadURLSyntax);
    uint32_t type = fourcc("ilge");
    static const struct { const char *scheme; const char *type; } kinds[] = {
        { "http", "ilht" }, { "https", "ilht" }, { "ftp", "ilft" }, { "afp", "ilaf" }, { "file", "ilfi" },
        { "news", "ilnw" }, { "nntp", "ilnw" }, { "mailto", "ilmt" }, { "telnet", "iltn" }, { "gopher", "ilgo" },
    };
    for (unsigned i = 0; i < sizeof kinds / sizeof *kinds; i++)
        if (strlen(kinds[i].scheme) == (size_t)(colon - url) && !strncasecmp(url, kinds[i].scheme, (size_t)(colon - url)))
            type = fourcc(kinds[i].type);
    gaddr p = tb_NewPtr(8 + 64 + FSSpec_SIZEOF, false, true);
    if (!p)
        RET(rest_memFullErr);
    gaddr vref = p, dir = p + 2, pname = p + 8, spec = p + 8 + 64;
    int16_t e = tb_FindFolder(-1, (int32_t)A(0), 1, vref, dir);
    rest_pstr_put(pname, name, 31);
    if (!e)
        e = tb_FSMakeFSSpec((int16_t)rd16(vref), (int32_t)rd32(dir), pname, spec);
    if (e == noErr)
        e = rest_dupFNErr;
    else if (e == fnfErr)
        e = tb_FSpCreate(spec, (int32_t)fourcc("MACS"), (int32_t)type, 0);
    if (!e) {
        char path[1024];
        FILE *f = fm_resolve((int16_t)rd16(spec), (int32_t)rd32(spec + 2), spec + 6, path, sizeof path) == noErr
                      ? fopen(path, "wb")
                      : NULL;
        if (!f || fwrite(url, 1, strlen(url), f) != strlen(url))
            e = ioErr;
        if (f)
            fclose(f);
    }
    tb_DisposePtr(p);
    RET((int32_t)e);
}

const ppc_export rest_nsl_exports[] = {
    { "NSLSaveURLAliasToFolder", nsl_SaveURLAliasToFolder },
    { "NSLAddServiceToServicesList", nsl_AddServiceToServicesList },
    { "NSLCancelRequest", nsl_CancelRequest },
    { "NSLCloseNavigationAPI", nsl_CloseNavigationAPI },
    { "NSLContinueLookup", nsl_ContinueLookup },
    { "NSLCopyNeighborhood", nsl_CopyNeighborhood },
    { "NSLDeleteRequest", nsl_DeleteRequest },
    { "NSLDisposeServicesList", nsl_DisposeServicesList },
    { "NSLDisposeThread", nsl_DisposeThread },
    { "NSLErrorToString", nsl_ErrorToString },
    { "NSLFreeNeighborhood", nsl_FreeNeighborhood },
    { "NSLFreeTypedDataPtr", nsl_FreeTypedDataPtr },
    { "NSLFreeURL", nsl_FreeURL },
    { "NSLGetDefaultDialogOptions", nsl_GetDefaultDialogOptions },
    { "NSLGetErrorStringsFromResource", nsl_GetErrorStringsFromResource },
    { "NSLGetNameFromNeighborhood", nsl_GetNameFromNeighborhood },
    { "NSLGetNeighborhoodLength", nsl_GetNeighborhoodLength },
    { "NSLGetNextNeighborhood", nsl_GetNextNeighborhood },
    { "NSLGetNextUrl", nsl_GetNextUrl },
    { "NSLGetServiceFromURL", nsl_GetServiceFromURL },
    { "NSLHexDecodeText", nsl_HexDecodeText },
    { "NSLHexEncodeText", nsl_HexEncodeText },
    { "NSLLibraryVersion", nsl_LibraryVersion },
    { "NSLMakeNewNeighborhood", nsl_MakeNewNeighborhood },
    { "NSLMakeNewServicesList", nsl_MakeNewServicesList },
    { "NSLMakeServicesRequestPB", nsl_MakeServicesRequestPB },
    { "NSLNewThread", nsl_NewThread },
    { "NSLOpenNavigationAPI", nsl_OpenNavigationAPI },
    { "NSLParseServiceRegistrationPB", nsl_ParseServiceRegistrationPB },
    { "NSLParseServicesRequestPB", nsl_ParseServicesRequestPB },
    { "NSLPrepareRequest", nsl_PrepareRequest },
    { "NSLServiceIsInServiceList", nsl_ServiceIsInServiceList },
    { "NSLStandardDeregisterURL", nsl_StandardRegisterURL },
    { "NSLStandardGetURL", nsl_StandardGetURL },
    { "NSLStandardRegisterURL", nsl_StandardRegisterURL },
    { "NSLStartNeighborhoodLookup", nsl_StartNeighborhoodLookup },
    { "NSLStartServicesLookup", nsl_StartServicesLookup },
};
const unsigned rest_nsl_export_count = sizeof rest_nsl_exports / sizeof rest_nsl_exports[0];
