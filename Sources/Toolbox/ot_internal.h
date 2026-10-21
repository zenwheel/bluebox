/*
 * Open Transport's providers, shared by opentransport.c (the providers
 * themselves, TCP/IP over the host's sockets) and ot_appletalk.c
 * (AppleTalk's endpoints, services and NBP mapper, on appletalk.c).
 */
#ifndef OT_INTERNAL_H
#define OT_INTERNAL_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>

#include "ppc_bridge.h"

enum {
    kOTNoError = 0,
    kOTBadAddressErr = -3150, kOTBadOptionErr = -3151, kOTBadReferenceErr = -3153, kOTNoAddressErr = -3154,
    kOTOutStateErr = -3155, kOTBadSequenceErr = -3156, kOTLookErr = -3158, kOTBadDataErr = -3159,
    kOTBufferOverflowErr = -3160, kOTFlowErr = -3161, kOTNoDataErr = -3162, kOTNoDisconnectErr = -3163,
    kOTNoUDErrErr = -3164, kOTNoReleaseErr = -3166, kOTNotSupportedErr = -3167, kOTBadNameErr = -3170,
    kOTBadQLenErr = -3171, kOTAddressBusyErr = -3172, kOTCanceledErr = -3180, kOTNotFoundErr = -3201,
    kOTOutOfMemoryErr = -3211, kOTDuplicateFoundErr = -3216,
    kETIMEDOUTErr = -3259, kECONNREFUSEDErr = -3260, kEHOSTDOWNErr = -3263, kEHOSTUNREACHErr = -3264,
    kENETUNREACHErr = -3249, kECONNRESETErr = -3253,
};

enum {
    T_LISTEN = 0x0001, T_CONNECT = 0x0002, T_DATA = 0x0004, T_EXDATA = 0x0008, T_DISCONNECT = 0x0010,
    T_ERROR = 0x0020, T_UDERR = 0x0040, T_ORDREL = 0x0080, T_GODATA = 0x0100, T_GOEXDATA = 0x0200,
    T_REQUEST = 0x0400, T_REPLY = 0x0800, T_PASSCON = 0x1000,
};

enum {
    T_BINDCOMPLETE = 0x20000001, T_UNBINDCOMPLETE = 0x20000002, T_ACCEPTCOMPLETE = 0x20000003,
    T_REPLYCOMPLETE = 0x20000004, T_DISCONNECTCOMPLETE = 0x20000005, T_OPTMGMTCOMPLETE = 0x20000006,
    T_OPENCOMPLETE = 0x20000007, T_GETPROTADDRCOMPLETE = 0x20000008, T_RESOLVEADDRCOMPLETE = 0x20000009,
    T_REGNAMECOMPLETE = 0x2000000D, T_DELNAMECOMPLETE = 0x2000000E, T_LKUPNAMECOMPLETE = 0x2000000F,
    T_LKUPNAMERESULT = 0x20000010, kOTSyncIdleEvent = 0x20000011,
    T_DNRSTRINGTOADDRCOMPLETE = 0x10000001, T_DNRMAILEXCHANGECOMPLETE = 0x10000004,
    /* AppleTalk services' completions (kAppleTalkEvent + n) */
    T_GETMYZONECOMPLETE = 0x23010001, T_GETLOCALZONESCOMPLETE = 0x23010002, T_GETZONELISTCOMPLETE = 0x23010003,
    T_GETATALKINFOCOMPLETE = 0x23010004,
};

enum { T_UNINIT = 0, T_UNBND = 1, T_IDLE = 2, T_OUTCON = 3, T_INCON = 4, T_DATAXFER = 5, T_OUTREL = 6, T_INREL = 7 };

/* OTFlags */
enum { T_MORE = 0x0001, T_EXPEDITED = 0x0002, T_ACKNOWLEDGED = 0x0004, T_TIMEDOUT = 0x0020 };

#define MAX_PROVIDERS 64

typedef enum { P_TCP, P_UDP, P_INET_SERVICES, P_DDP, P_ATP, P_ADSP, P_AT_SERVICES, P_NBP_MAPPER } provider_kind;
#define OT_ATALK(p) ((p)->kind >= P_DDP)

struct ot_atalk; /* ot_appletalk.c's own state */

typedef struct {
    bool used;
    provider_kind kind;
    gaddr ref;           /* the program's ProviderRef: REF_MAGIC, index */
    int fd;              /* -1: none yet */
    bool async, blocking;
    gaddr notifier, context;
    int state;
    int qlen;            /* listening, with this queue */
    int pending_fd;      /* a connection waiting for OTAccept */
    struct sockaddr_in pending_peer;
    struct sockaddr_in peer;
    bool connecting;     /* a connect in progress */
    bool connected;      /* it finished: T_CONNECT until OTRcvConnect */
    bool told_connect, told_data, told_ordrel, told_disc, told_listen, flow;
    bool eof;            /* the other end released */
    bool disconnected;   /* reset or refused: T_DISCONNECT until OTRcvDisconnect */
    int disc_reason;
    struct ot_atalk *at;
} provider;

extern provider ot_providers[MAX_PROVIDERS];

int ot_new_provider(provider_kind kind);
provider *ot_provider_of(gaddr ref);
/* An event for the notifier, delivered at the next poll. */
void ot_post(provider *p, uint32_t code, int32_t result, gaddr cookie);
/* A call that completed now: in asynchronous mode it says so later. */
void ot_complete(ppc_cpu *c, provider *p, uint32_t event, int32_t result, gaddr cookie);
/* One slice of a blocking call's wait: the notifier's idle event, time
 * passing; false when the machine stopped or the provider closed. */
bool ot_wait_slice(provider *p);

/* Data given as a chain of OTData (fNext, fData, fLen) rather than one
 * buffer: a TNetbuf's len, or OTSnd's byte count, kNetbufDataIsOTData. */
enum { kNetbufDataIsOTData = 0xFFFFFFFEu };
/* The bytes of data at buf (len of them, or an OTData chain). */
uint32_t ot_data_len(gaddr buf, uint32_t len);
/* Up to max of them copied out: the number copied. */
uint32_t ot_data_get(uint8_t *out, gaddr buf, uint32_t len, uint32_t max);

/* ot_appletalk.c */
bool ota_kind(const char *config, provider_kind *kind);
/* config: the configuration's string ("adsp(EnableEOM=1)"), or NULL */
int32_t ota_open(provider *p, const char *config);
void ota_close(provider *p);
void ota_poll(provider *p);
void ota_info(gaddr info, provider_kind kind);
void ota_Bind(ppc_cpu *c, provider *p);
void ota_Unbind(ppc_cpu *c, provider *p);
void ota_Connect(ppc_cpu *c, provider *p);
void ota_RcvConnect(ppc_cpu *c, provider *p);
void ota_Listen(ppc_cpu *c, provider *p);
void ota_Accept(ppc_cpu *c, provider *p, provider *w);
void ota_Snd(ppc_cpu *c, provider *p);
void ota_Rcv(ppc_cpu *c, provider *p);
void ota_Look(ppc_cpu *c, provider *p);
void ota_CountDataBytes(ppc_cpu *c, provider *p);
void ota_SndOrderlyDisconnect(ppc_cpu *c, provider *p);
void ota_RcvOrderlyDisconnect(ppc_cpu *c, provider *p);
void ota_SndDisconnect(ppc_cpu *c, provider *p);
void ota_RcvDisconnect(ppc_cpu *c, provider *p);
void ota_GetProtAddress(ppc_cpu *c, provider *p);
void ota_SndUData(ppc_cpu *c, provider *p);
void ota_RcvUData(ppc_cpu *c, provider *p);
void ota_ResolveAddress(ppc_cpu *c, provider *p);

#endif
