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

typedef enum { P_TCP, P_UDP, P_INET_SERVICES, P_DDP, P_ATP, P_ADSP, P_AT_SERVICES, P_NBP_MAPPER, P_PAP, P_ASP } provider_kind;
#define OT_ATALK(p) ((p)->kind >= P_DDP)

struct ot_atalk;   /* ot_appletalk.c's own state */
struct ot_session; /* and ot_session.c's, for PAP and ASP */

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
    bool told_connect, told_data, told_ordrel, told_disc, told_listen, flow, told_exdata;
    bool heard_ordrel; /* the notifier has had T_ORDREL */
    bool eof;            /* the other end released */
    bool disconnected;   /* reset or refused: T_DISCONNECT until OTRcvDisconnect */
    int disc_reason;
    struct ot_atalk *at;
    struct ot_session *ss;
    struct ot_optstore *opts; /* options negotiated (ot_options.c) */
    bool m68k;           /* opened by 68k code: its notifier is 68k code */
    int32_t cancel_err;  /* OTCancelSynchronousCalls: blocked calls return this */
    int32_t last_err;    /* XTI's t_errno for this endpoint's last call */
    bool owned_by_xti;   /* opened by t_open */
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
enum { kNetbufDataIsOTData = 0xFFFFFFFEu, kNetbufDataIsOTBufferStar = 0xFFFFFFFDu };
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
/* For ot_session.c: an address (DDP, or an NBP name looked up), a DDP
 * address into a TNetbuf, the endpoint's socket, and an endpoint bound
 * to any socket (OTAccept's worker). */
bool ota_address(provider *p, gaddr buf, uint32_t len, uint8_t *node, uint8_t *skt);
void ota_netbuf_ddp(gaddr nb, uint8_t node, uint8_t skt);
uint8_t ota_socket(provider *p);
int32_t ota_bind_any(provider *p);

/* ot_session.c: pap and asp endpoints */
int32_t ots_open(provider *p);
void ots_close(provider *p);
uint8_t ots_bind(provider *p, int qlen);
void ots_unbind(provider *p, uint8_t skt);
void ots_info(gaddr info, provider_kind kind);
void ots_poll(provider *p);
void ots_Connect(ppc_cpu *c, provider *p);
void ots_RcvConnect(ppc_cpu *c, provider *p);
void ots_Listen(ppc_cpu *c, provider *p);
void ots_Accept(ppc_cpu *c, provider *p, provider *w);
void ots_Snd(ppc_cpu *c, provider *p);
void ots_Rcv(ppc_cpu *c, provider *p);
void ots_Look(ppc_cpu *c, provider *p);
void ots_CountDataBytes(ppc_cpu *c, provider *p);
void ots_SndOrderlyDisconnect(ppc_cpu *c, provider *p);
void ots_RcvOrderlyDisconnect(ppc_cpu *c, provider *p);
void ots_SndDisconnect(ppc_cpu *c, provider *p);
void ots_RcvDisconnect(ppc_cpu *c, provider *p);
void ots_SndURequest(ppc_cpu *c, provider *p);
void ots_RcvUReply(ppc_cpu *c, provider *p);


/* ---- Shared with the other Open Transport files ---- */

/* Whether a notifier is running now (deferred task time: no synchronous
 * calls, as OTCanMakeSyncCall says). */
bool ot_in_notifier(void);
/* Whether the call being served came from 68k code (ot_68k.c sets it): the
 * program's callbacks and notifiers are then 68k routines. */
extern bool ot_from_68k;
/* Run the program's callback proc (PowerPC or 68k) with long arguments;
 * procinfo describes them as a Mixed Mode procInfo does. */
uint32_t ot_callproc(gaddr proc, uint32_t procinfo, const uint32_t *args, int n);
uint32_t ot_callproc_as(bool m68k, gaddr proc, uint32_t procinfo, const uint32_t *args, int n);

/* ot_util.c */
void ot_util_reset(void);
void ot_note_68k_proc(gaddr p);
bool ot_is_68k_proc(gaddr p);
uint32_t ot_make_port_ref(uint32_t bus, uint32_t dev, uint32_t slot, uint32_t other);
gaddr ot_buffer_new(const uint8_t *data, uint32_t len);

/* ot_hostnet.c: the host's network, as Open Transport reports it. */
typedef struct {
    char name[16];       /* the host's name for it (en0) */
    uint32_t addr, mask, bcast, gateway, dns, mtu;
    char domain[256];
    uint8_t mac[6];
    bool up, running, primary;
    int nsecondary;
    uint32_t secondary[16];
} ot_ifinfo;
int ot_host_interfaces(ot_ifinfo *out, int max);
bool ot_primary_interface(ot_ifinfo *out);
void ot_host_name(char *name, size_t size);

/* ot_ports.c: the port registry and registered clients. */
void ot_ports_reset(void);
void ot_ports_poll(void);
/* An event for every registered client, delivered at the next poll. */
void ot_clients_post(uint32_t code, int32_t result, gaddr cookie);
/* The port reference of the first (primary) Ethernet port. */
uint32_t ot_default_port_ref(void);
/* Open Transport's TCP/IP stack being used for the first time. */
void ot_stack_loading(void);

/* ot_options.c: what was negotiated stays with the endpoint. */
void ot_options_free(provider *p);
int32_t ot_option_mgmt(provider *p, gaddr req, gaddr ret);
/* Negotiate the options in a call's or datagram's option netbuf. */
void ot_options_apply(provider *p, gaddr nb);
/* Put the endpoint's options on a new host socket. */
void ot_options_reapply(provider *p);
/* AppleTalk's endpoints read the long or bytes negotiated for an option. */
bool ot_option_long(provider *p, uint32_t level, uint32_t name, uint32_t *out);
uint32_t ot_option_bytes(provider *p, uint32_t level, uint32_t name, uint8_t *out, uint32_t max);

/* ot_inet.c */
void ot_inet_reset(void);

/* Bring one provider's state up to date with its socket. */
void ot_poll_provider(provider *p);
void ot_put_info(gaddr info, provider_kind kind);

/* The tables of the files after opentransport.c's (ot_exports.c looks in
 * them all), and Open Transport's data exports. */
extern const ppc_export otu_exports[], otp_exports[], oto_exports[], oti_exports[], otc_exports[], otx_exports[];
extern const unsigned otu_export_count, otp_export_count, oto_export_count, oti_export_count, otc_export_count,
    otx_export_count;
gaddr ot_xti_errno_address(void);
void ot_xti_reset(void);

#endif
