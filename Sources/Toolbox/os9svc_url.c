/*
 * URLAccessLib (URLAccess.h): URL references, their properties and states
 * are kept, as on a Mac whose network service isn't there; opening,
 * downloading and uploading answer kURLAccessNotAvailableError, since Blue
 * Box's machine has no HTTP or FTP client to make a connection (the
 * properties of a transfer, its data and its size are then never known).
 * PowerPC only, as URLAccess was (kURL68kNotSupportedError).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "os9svc.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(int32_t)(v); c->pc = c->lr; return; } while (0)

enum {
    kURLInvalidURLReferenceError = -30770,
    kURLInvalidURLError = -30773,
    kURLUnsupportedSchemeError = -30774,
    kURLPropertyNotYetKnownError = -30777,
    kURLUnknownPropertyError = -30778,
    kURLPropertyBufferTooSmallError = -30779,
    kURLUnsettablePropertyError = -30780,
    kURLInvalidCallError = -30781,
    kURLAccessNotAvailableError = -30787,
};
enum { kURLNullState = 0, kURLErrorOccurredState = 7 };
enum { URL_VERSION = 0x02028000 }; /* URLAccess 2.0.2 */

#define MAX_REFS 32
#define MAX_URL 1024
#define REF_TAG 0x55524C00u /* 'URL', the index below */

typedef struct {
    bool used;
    char url[MAX_URL];
    uint32_t state;
    int32_t error;
    /* The properties a program sets */
    char user[256], password[256], method[32], user_agent[256], header[1024];
    uint8_t *body;
    uint32_t body_len;
    int32_t connect_timeout;
    bool have_user, have_password, have_method, have_agent, have_header, have_timeout;
} urlref;

static urlref refs[MAX_REFS];

void os9svc_url_reset(void)
{
    for (int i = 0; i < MAX_REFS; i++)
        free(refs[i].body);
    memset(refs, 0, sizeof refs);
}

static urlref *ref_of(uint32_t id)
{
    uint32_t i = id - REF_TAG - 1;
    return id > REF_TAG && i < MAX_REFS && refs[i].used ? &refs[i] : NULL;
}

static void get_cstr(gaddr p, char *out, size_t max)
{
    size_t n = 0;
    while (n + 1 < max && rd8(p + n)) {
        out[n] = (char)rd8(p + n);
        n++;
    }
    out[n] = 0;
}

/* A URL we can name the parts of: "scheme://[user[:password]@]host[:port]/path". */
static int url_check(const char *url)
{
    const char *colon = strstr(url, "://");
    if (!colon || colon == url)
        return kURLInvalidURLError;
    for (const char *p = url; p < colon; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '+' ||
              *p == '-' || *p == '.'))
            return kURLInvalidURLError;
    size_t n = (size_t)(colon - url);
    static const char *const schemes[] = { "http", "https", "ftp", "file" };
    for (unsigned i = 0; i < sizeof schemes / sizeof schemes[0]; i++)
        if (strlen(schemes[i]) == n && !strncasecmp(url, schemes[i], n))
            return noErr;
    return kURLUnsupportedSchemeError;
}

/* The host, the user and password, and the last path part of a URL. */
static void url_parts(const char *url, char *host, char *user, char *password, char *name)
{
    host[0] = user[0] = password[0] = name[0] = 0;
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *end = p + strcspn(p, "/?#");
    const char *at = memchr(p, '@', (size_t)(end - p));
    if (at) {
        const char *colon = memchr(p, ':', (size_t)(at - p));
        size_t ul = (size_t)((colon ? colon : at) - p);
        snprintf(user, 256, "%.*s", (int)ul, p);
        if (colon)
            snprintf(password, 256, "%.*s", (int)(at - colon - 1), colon + 1);
        p = at + 1;
    }
    const char *colon = memchr(p, ':', (size_t)(end - p));
    snprintf(host, 256, "%.*s", (int)((colon ? colon : end) - p), p);
    const char *path_end = end + strcspn(end, "?#");
    const char *slash = path_end;
    while (slash > end && slash[-1] != '/')
        slash--;
    snprintf(name, 256, "%.*s", (int)(path_end - slash), slash);
}

/* URLNewReference(url, *ref) */
static void url_NewReference(ppc_cpu *c)
{
    if (!R(0) || !R(1))
        RETURN(paramErr);
    char url[MAX_URL];
    get_cstr(R(0), url, sizeof url);
    int err = url_check(url);
    if (err)
        RETURN(err);
    for (int i = 0; i < MAX_REFS; i++)
        if (!refs[i].used) {
            memset(&refs[i], 0, sizeof refs[i]);
            refs[i].used = true;
            snprintf(refs[i].url, sizeof refs[i].url, "%s", url);
            wr32(R(1), REF_TAG + (uint32_t)i + 1);
            RETURN(noErr);
        }
    RETURN(memFullErr);
}

static void url_DisposeReference(ppc_cpu *c)
{
    urlref *r = ref_of(R(0));
    if (!r)
        RETURN(kURLInvalidURLReferenceError);
    free(r->body);
    r->used = false;
    RETURN(noErr);
}

/* A transfer that cannot be made: the reference ends in the error state. */
static int transfer_unavailable(urlref *r)
{
    int err = url_check(r->url);
    if (!err)
        err = kURLAccessNotAvailableError;
    r->state = kURLErrorOccurredState;
    r->error = err;
    return err;
}

static int simple_transfer(gaddr urlp)
{
    char url[MAX_URL];
    if (!urlp)
        return paramErr;
    get_cstr(urlp, url, sizeof url);
    int err = url_check(url);
    return err ? err : kURLAccessNotAvailableError;
}

/* URLSimpleDownload(url, destination, destinationHandle, openFlags, eventProc, userContext) */
static void url_SimpleDownload(ppc_cpu *c)
{
    if (!R(1) && !R(2))
        RETURN(kURLInvalidCallError);
    RETURN(simple_transfer(R(0)));
}

/* URLSimpleUpload(url, source, openFlags, eventProc, userContext) */
static void url_SimpleUpload(ppc_cpu *c)
{
    if (!R(1))
        RETURN(kURLInvalidCallError);
    RETURN(simple_transfer(R(0)));
}

/* URLDownload(ref, destination, destinationHandle, openFlags, eventProc, userContext)
 * and URLDownloadWithSigner, which has more after. */
static void url_Download(ppc_cpu *c)
{
    urlref *r = ref_of(R(0));
    if (!r)
        RETURN(kURLInvalidURLReferenceError);
    if (!R(1) && !R(2))
        RETURN(kURLInvalidCallError);
    RETURN(transfer_unavailable(r));
}

/* URLUpload(ref, source, openFlags, eventProc, userContext) */
static void url_Upload(ppc_cpu *c)
{
    urlref *r = ref_of(R(0));
    if (!r)
        RETURN(kURLInvalidURLReferenceError);
    if (!R(1))
        RETURN(kURLInvalidCallError);
    RETURN(transfer_unavailable(r));
}

/* URLOpen(ref, fileSpec, openFlags, notifyProc, eventRegister, userContext) */
static void url_Open(ppc_cpu *c)
{
    urlref *r = ref_of(R(0));
    if (!r)
        RETURN(kURLInvalidURLReferenceError);
    RETURN(transfer_unavailable(r));
}

static void url_Abort(ppc_cpu *c)
{
    urlref *r = ref_of(R(0));
    if (!r)
        RETURN(kURLInvalidURLReferenceError);
    r->state = kURLNullState;
    RETURN(noErr);
}

/* Nothing was ever opened, so there is no data to have. */
static void url_NoTransfer(ppc_cpu *c)
{
    if (!ref_of(R(0)))
        RETURN(kURLInvalidURLReferenceError);
    RETURN(kURLInvalidCallError);
}

static void url_GetCurrentState(ppc_cpu *c)
{
    urlref *r = ref_of(R(0));
    if (!r)
        RETURN(kURLInvalidURLReferenceError);
    if (R(1))
        wr32(R(1), r->state);
    RETURN(noErr);
}

static void url_GetError(ppc_cpu *c)
{
    urlref *r = ref_of(R(0));
    if (!r)
        RETURN(kURLInvalidURLReferenceError);
    if (R(1))
        wr32(R(1), (uint32_t)r->error);
    RETURN(noErr);
}

static void url_Idle(ppc_cpu *c) { RETURN(noErr); }

static void url_GetVersion(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    wr32(R(0), URL_VERSION);
    RETURN(noErr);
}

/* ---- Properties ---- */

/* The value of a property as bytes, where it is known: 0 and the data, or
 * the error. */
static int property_value(urlref *r, const char *name, const uint8_t **data, uint32_t *len, char *scratch)
{
    char host[256], user[256], password[256], file[256];
    url_parts(r->url, host, user, password, file);
    const char *s = NULL;
    if (!strcmp(name, "URLString"))
        s = r->url;
    else if (!strcmp(name, "URLHost"))
        s = host;
    else if (!strcmp(name, "URLResourceName"))
        s = file;
    else if (!strcmp(name, "URLUserName"))
        s = r->have_user ? r->user : user;
    else if (!strcmp(name, "URLPassword"))
        s = r->have_password ? r->password : password;
    else if (!strcmp(name, "URLHTTPRequestMethod"))
        s = r->have_method ? r->method : "GET";
    else if (!strcmp(name, "URLHTTPUserAgent"))
        s = r->have_agent ? r->user_agent : "";
    else if (!strcmp(name, "URLHTTPRequestHeader"))
        s = r->have_header ? r->header : "";
    else if (!strcmp(name, "URLHTTPRequestBody")) {
        *data = r->body;
        *len = r->body_len;
        return noErr;
    } else if (!strcmp(name, "URLConnectTimeout")) {
        uint32_t v = r->have_timeout ? (uint32_t)r->connect_timeout : 0;
        scratch[0] = (char)(v >> 24);
        scratch[1] = (char)(v >> 16);
        scratch[2] = (char)(v >> 8);
        scratch[3] = (char)v;
        *data = (const uint8_t *)scratch;
        *len = 4;
        return noErr;
    } else {
        static const char *const known[] = { "URLResourceSize", "URLLastModifiedTime", "URLMIMEType",
                                             "URLFileType", "URLFileCreator", "URLCharacterSet", "URLAuthType",
                                             "URLStatusString", "URLIsSecure", "URLCertificate", "URLTotalItems",
                                             "URLHTTPRespHeader", "URLHTTPRedirectedURL", "URLSSLCipherSuite" };
        for (unsigned i = 0; i < sizeof known / sizeof known[0]; i++)
            if (!strcmp(name, known[i]))
                return kURLPropertyNotYetKnownError; /* nothing has been fetched */
        return kURLUnknownPropertyError;
    }
    *data = (const uint8_t *)s;
    *len = (uint32_t)strlen(s) + 1; /* a C string, with its end */
    return noErr;
}

/* URLGetProperty(ref, property, buffer, bufferSize) */
static void url_GetProperty(ppc_cpu *c)
{
    urlref *r = ref_of(R(0));
    if (!r)
        RETURN(kURLInvalidURLReferenceError);
    if (!R(1) || !R(2))
        RETURN(paramErr);
    char name[64], scratch[4];
    const uint8_t *data;
    uint32_t len;
    get_cstr(R(1), name, sizeof name);
    int err = property_value(r, name, &data, &len, scratch);
    if (err)
        RETURN(err);
    if (R(3) < len)
        RETURN(kURLPropertyBufferTooSmallError);
    for (uint32_t i = 0; i < len; i++)
        wr8(R(2) + i, data[i]);
    RETURN(noErr);
}

/* URLGetPropertySize(ref, property, *size) */
static void url_GetPropertySize(ppc_cpu *c)
{
    urlref *r = ref_of(R(0));
    if (!r)
        RETURN(kURLInvalidURLReferenceError);
    if (!R(1) || !R(2))
        RETURN(paramErr);
    char name[64], scratch[4];
    const uint8_t *data;
    uint32_t len;
    get_cstr(R(1), name, sizeof name);
    int err = property_value(r, name, &data, &len, scratch);
    if (err)
        RETURN(err);
    wr32(R(2), len);
    RETURN(noErr);
}

static bool set_cstr(char *dst, size_t max, gaddr buf, uint32_t size)
{
    if (size >= max)
        return false;
    for (uint32_t i = 0; i < size; i++)
        dst[i] = (char)rd8(buf + i);
    dst[size] = 0;
    char *nul = memchr(dst, 0, size + 1);
    if (nul)
        *nul = 0;
    return true;
}

/* URLSetProperty(ref, property, buffer, bufferSize) */
static void url_SetProperty(ppc_cpu *c)
{
    urlref *r = ref_of(R(0));
    if (!r)
        RETURN(kURLInvalidURLReferenceError);
    if (!R(1) || (!R(2) && R(3)))
        RETURN(paramErr);
    char name[64];
    get_cstr(R(1), name, sizeof name);
    gaddr buf = R(2);
    uint32_t size = R(3);
    if (!strcmp(name, "URLUserName"))
        r->have_user = set_cstr(r->user, sizeof r->user, buf, size);
    else if (!strcmp(name, "URLPassword"))
        r->have_password = set_cstr(r->password, sizeof r->password, buf, size);
    else if (!strcmp(name, "URLHTTPRequestMethod"))
        r->have_method = set_cstr(r->method, sizeof r->method, buf, size);
    else if (!strcmp(name, "URLHTTPUserAgent"))
        r->have_agent = set_cstr(r->user_agent, sizeof r->user_agent, buf, size);
    else if (!strcmp(name, "URLHTTPRequestHeader"))
        r->have_header = set_cstr(r->header, sizeof r->header, buf, size);
    else if (!strcmp(name, "URLHTTPRequestBody")) {
        uint8_t *body = size ? malloc(size) : NULL;
        if (size && !body)
            RETURN(memFullErr);
        for (uint32_t i = 0; i < size; i++)
            body[i] = rd8(buf + i);
        free(r->body);
        r->body = body;
        r->body_len = size;
    } else if (!strcmp(name, "URLConnectTimeout")) {
        if (size != 4)
            RETURN(paramErr);
        r->connect_timeout = (int32_t)rd32(buf);
        r->have_timeout = true;
    } else {
        char scratch[4];
        const uint8_t *data;
        uint32_t len;
        int err = property_value(r, name, &data, &len, scratch);
        RETURN(err == kURLUnknownPropertyError ? err : kURLUnsettablePropertyError);
    }
    RETURN(noErr);
}

/* URLGetFileInfo(fName, *fType, *fCreator): the type and creator the
 * Internet Config mappings give a file of that name's extension, '????' for
 * both when there is none. */
static void url_GetFileInfo(ppc_cpu *c)
{
    gaddr entry = tb_NewPtr(2048, false, true);
    if (!R(0) || !entry)
        RETURN(R(0) ? memFullErr : paramErr);
    uint32_t type = 0x3F3F3F3F, creator = 0x3F3F3F3F;
    if (!ic_map_filename(R(0), entry)) {
        type = rd32(entry + 6);
        creator = rd32(entry + 10);
    }
    tb_DisposePtr(entry);
    if (R(1))
        wr32(R(1), type);
    if (R(2))
        wr32(R(2), creator);
    RETURN(noErr);
}

const ppc_export os9_url_exports[] = {
    { "URLGetURLAccessVersion", url_GetVersion }, { "URLNewReference", url_NewReference },
    { "URLDisposeReference", url_DisposeReference }, { "URLSimpleDownload", url_SimpleDownload },
    { "URLSimpleUpload", url_SimpleUpload }, { "URLDownload", url_Download },
    { "URLDownloadWithSigner", url_Download }, { "URLUpload", url_Upload }, { "URLOpen", url_Open },
    { "URLAbort", url_Abort }, { "URLGetDataAvailable", url_NoTransfer }, { "URLGetBuffer", url_NoTransfer },
    { "URLReleaseBuffer", url_NoTransfer }, { "URLGetProperty", url_GetProperty },
    { "URLGetPropertySize", url_GetPropertySize }, { "URLSetProperty", url_SetProperty },
    { "URLGetCurrentState", url_GetCurrentState }, { "URLGetError", url_GetError }, { "URLIdle", url_Idle },
    { "URLGetFileInfo", url_GetFileInfo },
};
const unsigned os9_url_export_count = sizeof os9_url_exports / sizeof os9_url_exports[0];
const os9_alias os9_url_aliases[] = { { "", "" } };
const unsigned os9_url_alias_count = 0;
