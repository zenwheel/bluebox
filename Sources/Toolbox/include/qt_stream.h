/* QuickTime streaming: QTStreamLib's presentations (qt_stream.c) over
 * RTSP, RTP and SDP of our own (qt_rtsp.c). */
#ifndef QT_STREAM_H
#define QT_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mem.h"
#include "ppc.h"

enum {
    qtsBadSelectorErr = -5400, qtsBadStateErr = -5401, qtsBadDataErr = -5402, qtsUnsupportedDataTypeErr = -5403,
    qtsUnsupportedRateErr = -5404, qtsUnsupportedFeatureErr = -5405, qtsTooMuchDataErr = -5406,
    qtsUnknownValueErr = -5407, qtsTimeoutErr = -5408, qtsConnectionFailedErr = -5420, qtsAddressBusyErr = -5421,
};

/* ---- SDP (RFC 2327) ---- */

#define QTS_MAX_MEDIA 8

typedef struct {
    uint32_t media_type;  /* 'vide', 'soun', 'text', or the m= name's first letters */
    int pt;               /* the first payload type listed */
    char encoding[32];    /* a=rtpmap's name ("" for a static type without one) */
    int clock, channels;
    char fmtp[256];
    char control[512];    /* absolute */
    char addr[64];        /* c= (the session's when the media has none) */
    int port;
    int width, height;    /* a=cliprect, a=x-dimensions, a=framesize; 0 when not said */
} qts_sdp_media;

typedef struct {
    char name[128];       /* s= */
    char control[512];    /* the session's aggregate control URL */
    double range_end;     /* a=range:npt=start-end in seconds; 0 open-ended */
    int nmedia;
    qts_sdp_media m[QTS_MAX_MEDIA];
} qts_sdp;

/* A session description; base is the URL it came from (for relative controls). */
bool qts_sdp_parse(const char *text, size_t len, const char *base, qts_sdp *out);

/* ---- The network: RTSP (RFC 2326) with RTP interleaved on its TCP
 * connection, or RTP over UDP to the ports an SDP names ---- */

typedef struct qts_net qts_net;

/* An RTSP session: connected and described (*sdp malloc'd, the
 * description's text).  NULL with *err on failure. */
qts_net *qts_net_rtsp(const char *url, char **sdp, int16_t *err);
/* RTP received on the ports a description names (multicast joined). */
qts_net *qts_net_udp(const qts_sdp *sdp, int16_t *err);
/* SETUP of each medium (RTSP; nothing for UDP). */
int16_t qts_net_setup(qts_net *n, const qts_sdp *sdp);
/* PLAY from npt seconds (negative: from where it is), PAUSE. */
int16_t qts_net_play(qts_net *n, double from);
int16_t qts_net_pause(qts_net *n);
/* TEARDOWN and close. */
void qts_net_close(qts_net *n);
/* The RTP packets that have come, each to fn with its medium's index. */
typedef void (*qts_packet_fn)(void *ctx, int media, const uint8_t *pkt, size_t len);
int qts_net_poll(qts_net *n, qts_packet_fn fn, void *ctx);
/* Received packets and bytes, for statistics. */
void qts_net_counts(const qts_net *n, uint32_t *packets, uint32_t *bytes);
/* Whether it is an RTSP session (else RTP to UDP ports). */
bool qts_net_rtsp_session(const qts_net *n);

/* ---- RTP payloads to sound and pictures ---- */

typedef struct qts_depay qts_depay;

/* What a packet completed: sound (16-bit PCM, interleaved) or a picture
 * (RGBA rows), with its RTP time. */
typedef struct {
    int16_t *pcm;         /* malloc'd */
    uint32_t frames;
    int channels;
    double rate;
    uint8_t *rgba;        /* malloc'd */
    int w, h;
    uint32_t time;
} qts_unit;

/* Whether a medium's payload is one we turn into sound or pictures:
 * L8, L16, PCMU, PCMA, DVI4, JPEG (RFC 2435) and QuickTime's own (X-QT,
 * samples of the classic codecs). */
bool qts_depay_supported(const qts_sdp_media *m);
qts_depay *qts_depay_new(const qts_sdp_media *m);
void qts_depay_free(qts_depay *d);
/* A packet in; 1 when it finished a unit (*out filled), 0 otherwise. */
int qts_depay_packet(qts_depay *d, const uint8_t *pkt, size_t len, qts_unit *out);
/* What kind of media the depacketizer has seen ('vide', 'soun', 0). */
uint32_t qts_depay_media(const qts_depay *d);
/* A JPEG interchange stream for an RFC 2435 frame's header fields and
 * scan data (malloc'd). */
uint8_t *qts_jpeg_frame(int type, int q, int w, int h, const uint8_t *qtables, int qlen, int dri,
                        const uint8_t *scan, size_t scan_len, size_t *out_len);

/* ---- QTStreamLib (qt_stream.c) ---- */

void (*qtstream_export_fn(const char *name))(ppc_cpu *);
/* A streaming movie for an rtsp:// URL (NewMovieFromDataRef with a 'url '
 * reference), or for an SDP file or QuickTime's RTSP text file
 * (noMovieFound for any other file). */
int16_t qt_stream_open_movie(const char *url, gaddr moviep, int16_t flags);
int16_t qt_stream_import_file(const char *path, gaddr moviep, int16_t flags);
/* MoviesTask's part: streaming movies' presentations kept going. */
void qt_stream_task(void);
/* A movie just opened: its streaming track's presentation made. */
void qt_stream_adopt(gaddr movie);
void qt_stream_reset(void);

/* The FireWire DV isochronous data handler ('ihlr', qt_idh.c). */
void qt_idh_register(void);

#endif
