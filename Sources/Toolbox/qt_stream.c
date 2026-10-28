/*
 * QTStreamLib: QuickTime 4 and 5's streaming presentations, as PowerPC
 * programs call them, and streaming movies.
 *
 * A presentation comes from a session description (SDP text, a file of
 * it, or QuickTime's "RTSPtext" files) or an rtsp:// URL, whose server
 * describes it.  Its streams are the description's media; prerolling
 * sets them up (RTSP SETUP, RTP interleaved on the connection), starting
 * plays them (PLAY), stopping pauses them, and QTSPresIdle takes what has
 * come in: pictures drawn into each stream's GWorld and sound played on
 * the host as packets complete them (no jitter buffer: they show when
 * they arrive).  Notifications go to the program's procedure as
 * QuickTime sent them ('stnw', 'pack', 'sack', 'xack', 'pdon', 'err ').
 * What we decode is qt_rtsp.c's list; other streams are set up and
 * received but show nothing.
 *
 * Not done, with the documented errors: sending (kQTSSendMediaFlag
 * presentations, sourcers: Blue Box broadcasts nothing), the settings
 * dialog, and previews.  Messages, buffers, memory, statistics, the
 * connection preferences and error strings are whole.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ppc_bridge.h"
#include "qd_internal.h"
#include "qt_atoms.h"
#include "qt_comp.h"
#include "qt_dref.h"
#include "qt_errors.h"
#include "qt_movie_internal.h"
#include "qt_stream.h"
#include "tb_internal.h"

#define ARG(k) ((k) < 8 ? c->gpr[3 + (k)] : rd32(c->gpr[1] + 24 + 4 * (uint32_t)(k)))
#define RET(v) (c->gpr[3] = (uint32_t)(v), c->pc = c->lr)

enum {
    paramErrQ = -50, memFullErrQ = -108, fnfErrQ = -43, kQTSAutoModeFlag = 1, kQTSSendMediaFlag = 0x10000,
    kPresTimeScale = 600,
};

/* ---- Presentations and streams ---- */

#define MAX_PRES 32

typedef struct pres pres;

typedef struct {
    gaddr token;
    pres *p;
    int media;              /* the description's medium, -1 for a stream added empty */
    uint32_t type;
    int32_t width, height;  /* Fixed */
    int32_t matrix[9];
    gaddr clip;
    int16_t mode;
    uint16_t op[3];
    gaddr gworld, gdh;
    bool enabled, presenting;
    int16_t vol_l, vol_r;
    int32_t hints;
    int64_t seg_start, seg_dur;
    gaddr settings;
    qts_depay *depay;
    uint8_t *frame;         /* the last picture, RGBA */
    int fw, fh;
    uint32_t units, packets;
    bool metering;
    int32_t buffer_time;    /* Fixed seconds */
} stream;

struct pres {
    gaddr token;
    int32_t scale, flags;
    gaddr notify, refcon;
    int32_t pref_rate;
    gaddr timebase;
    stream s[QTS_MAX_MEDIA];
    int ns;
    qts_sdp sdp;
    char url[512];
    qts_net *net;
    bool set_up, playing, done;
    double at;              /* npt where playing last started, seconds */
    int64_t started_ms;
    int64_t seg_start, seg_dur;
    int32_t hints;
    gaddr movie;            /* a streaming movie's, 0 for a program's presentation */
    int64_t movie_last;     /* the movie time last seen (ms) */
    gaddr settings;
};

static pres *press[MAX_PRES];

static int64_t now_ms(void) { return (int64_t)tb_TickCount() * 1000 / 60; }

static pres *pres_of(gaddr token)
{
    for (int i = 0; i < MAX_PRES; i++)
        if (press[i] && token && press[i]->token == token)
            return press[i];
    return NULL;
}

static stream *stream_of(gaddr token, pres **pp)
{
    for (int i = 0; i < MAX_PRES; i++)
        if (press[i])
            for (int k = 0; k < press[i]->ns; k++)
                if (token && press[i]->s[k].token == token) {
                    if (pp)
                        *pp = press[i];
                    return &press[i]->s[k];
                }
    return NULL;
}

/* A call naming a stream of the presentation, or all of them (0). */
static bool target(pres *p, gaddr stok, stream **st)
{
    *st = NULL;
    if (!stok)
        return true;
    pres *q;
    stream *s = stream_of(stok, &q);
    if (!s || q != p)
        return false;
    *st = s;
    return true;
}

static uint32_t notify(pres *p, int16_t err, const char *type, gaddr params)
{
    if (!p->notify)
        return 0;
    uint32_t args[4] = { (uint32_t)(int32_t)err, fourcc(type), params, p->refcon };
    return mm_call(p->notify, 0x3FF0, args, 4);
}

/* A notification whose parameter is one word (a stream): passed in a
 * block of its own, or the word itself for those QuickTime passed so. */
static void notify_stream(pres *p, const char *type, stream *s)
{
    gaddr b = tb_NewPtr(8, false, true);
    if (!b)
        return;
    wr32(b, s ? s->token : 0);
    notify(p, 0, type, !strcmp(type, "stnw") || !strcmp(type, "stgn") ? b : s ? s->token : 0);
    tb_DisposePtr(b);
}

static void notify_error(pres *p, int16_t err, const char *text)
{
    gaddr b = tb_NewPtr(8 + (int32_t)strlen(text) + 1, false, true);
    if (!b) {
        notify(p, err, "err ", 0);
        return;
    }
    gput(b + 8, text, (uint32_t)strlen(text) + 1);
    wr32(b, b + 8);
    wr32(b + 4, 1); /* kQTSFatalErrorFlag */
    notify(p, err, "err ", b);
    tb_DisposePtr(b);
}

/* The host sound channel a stream plays on. */
static int sound_id(stream *s) { return 0x51540000 + (int)(s - s->p->s) + 16 * (int)(s->p->token & 0xFFF); }

static void stream_init(stream *s, pres *p, int media)
{
    memset(s, 0, sizeof *s);
    s->token = tb_NewPtr(4, false, true);
    if (s->token)
        wr32(s->token, fourcc("qtss"));
    s->p = p;
    s->media = media;
    s->enabled = s->presenting = true;
    s->vol_l = s->vol_r = 0x100;
    s->matrix[0] = s->matrix[4] = 0x10000;
    s->matrix[8] = 0x40000000;
    s->seg_dur = -1;
    if (media >= 0) {
        const qts_sdp_media *m = &p->sdp.m[media];
        s->type = m->media_type;
        s->width = m->width << 16;
        s->height = m->height << 16;
        s->depay = qts_depay_new(m);
    }
}

static void stream_free(stream *s)
{
    if (s->clip)
        tb_DisposeRgn(s->clip);
    if (s->settings)
        tb_DisposeHandle(s->settings);
    if (s->token)
        tb_DisposePtr(s->token);
    qts_depay_free(s->depay);
    free(s->frame);
    sound_host_stop(sound_id(s));
}

static pres *pres_new(int32_t scale, int32_t flags, gaddr proc, gaddr refcon)
{
    int slot = -1;
    for (int i = 0; i < MAX_PRES && slot < 0; i++)
        if (!press[i])
            slot = i;
    if (slot < 0)
        return NULL;
    pres *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->token = tb_NewPtr(4, false, true);
    if (!p->token) {
        free(p);
        return NULL;
    }
    wr32(p->token, fourcc("qtsp"));
    p->scale = scale > 0 ? scale : kPresTimeScale;
    p->flags = flags;
    p->notify = proc;
    p->refcon = refcon;
    p->pref_rate = 0x10000;
    p->timebase = (gaddr)tb_NewTimeBase();
    p->seg_dur = -1;
    press[slot] = p;
    return p;
}

static void pres_free(pres *p)
{
    for (int i = 0; i < MAX_PRES; i++)
        if (press[i] == p)
            press[i] = NULL;
    qts_net_close(p->net);
    for (int k = 0; k < p->ns; k++)
        stream_free(&p->s[k]);
    if (p->timebase)
        tb_DisposeTimeBase((int32_t)p->timebase);
    if (p->settings)
        tb_DisposeHandle(p->settings);
    tb_DisposePtr(p->token);
    free(p);
}

/* The presentation's streams from its description (the program told of each). */
static void add_streams(pres *p, bool tell)
{
    for (int i = 0; i < p->sdp.nmedia && p->ns < QTS_MAX_MEDIA; i++) {
        stream *s = &p->s[p->ns++];
        stream_init(s, p, i);
        if (tell)
            notify_stream(p, "stnw", s);
    }
    if (tell)
        notify(p, 0, "prch", 0);
}

/* A presentation of a description's text (base: where it came from). */
static int16_t pres_from_sdp(pres *p, const char *text, size_t len, const char *base)
{
    if (!qts_sdp_parse(text, len, base, &p->sdp))
        return qtsBadDataErr;
    snprintf(p->url, sizeof p->url, "%s", base ? base : "");
    return 0;
}

static int16_t pres_from_url(pres *p, const char *url)
{
    int16_t err;
    char *sdp;
    p->net = qts_net_rtsp(url, &sdp, &err);
    if (!p->net)
        return err;
    snprintf(p->url, sizeof p->url, "%s", url);
    err = qts_sdp_parse(sdp, strlen(sdp), url, &p->sdp) ? 0 : qtsBadDataErr;
    free(sdp);
    return err;
}

/* An "RTSPtext" file's URL, or a description's text: which one the bytes are. */
static int16_t pres_from_bytes(pres *p, const uint8_t *b, size_t n, const char *base)
{
    if (n >= 8 && !memcmp(b, "RTSPtext", 8)) {
        char url[512];
        size_t k = 8;
        while (k < n && (b[k] == ' ' || b[k] == '\r' || b[k] == '\n' || b[k] == '\t'))
            k++;
        size_t e = k;
        while (e < n && b[e] > ' ' && e - k < sizeof url - 1)
            e++;
        memcpy(url, b + k, e - k);
        url[e - k] = 0;
        return pres_from_url(p, url);
    }
    if (n >= 4 && !memcmp(b, "rtsp", 4)) {
        char url[512];
        size_t e = 0;
        while (e < n && b[e] > ' ' && e < sizeof url - 1)
            e++;
        memcpy(url, b, e);
        url[e] = 0;
        return pres_from_url(p, url);
    }
    return pres_from_sdp(p, (const char *)b, n, base);
}

static uint8_t *file_bytes(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t *b = malloc(65537);
    *n = b ? fread(b, 1, 65536, f) : 0;
    fclose(f);
    return b;
}

static bool spec_path(gaddr spec, char *out, size_t n)
{
    return spec && fm_host_path((int16_t)rd16(spec), (int32_t)rd32(spec + 2), spec + 6, true, out, n);
}

/* A data reference (type, its bytes) as a presentation's source. */
static int16_t pres_from_dref(pres *p, uint32_t type, gaddr h)
{
    uint32_t n;
    uint8_t *b = qt_dref_bytes(h, &n);
    if (!b)
        return paramErrQ;
    int16_t err;
    char path[1024];
    if (type == fourcc("url ") && n >= 7 && !strncasecmp((char *)b, "rtsp://", 7)) {
        char url[512];
        uint32_t k = n < sizeof url - 1 ? n : (uint32_t)sizeof url - 1;
        memcpy(url, b, k);
        url[k] = 0;
        err = pres_from_url(p, url);
    } else if (qt_dref_path(type, b, n, NULL, path, sizeof path)) {
        size_t fl;
        uint8_t *fb = file_bytes(path, &fl);
        err = fb ? pres_from_bytes(p, fb, fl, NULL) : fnfErrQ;
        free(fb);
    } else if (type == fourcc("hndl") && n >= 4 && rd32((uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3])) {
        gaddr hh = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
        uint32_t hl = (uint32_t)tb_GetHandleSize(hh);
        uint8_t *hb = malloc(hl + 1);
        gget(hb, rd32(hh), hl);
        err = pres_from_bytes(p, hb, hl, NULL);
        free(hb);
    } else {
        err = qtsUnsupportedDataTypeErr;
    }
    free(b);
    return err;
}

/* An atom container's description: the text of an 'sdp ' atom anywhere in it. */
static int16_t pres_from_atoms(pres *p, gaddr container, uint32_t parent)
{
    if (!container || !rd32(container))
        return paramErrQ;
    const uint8_t *cb = g_mem + rd32(container);
    size_t cl = (size_t)tb_GetHandleSize(container);
    uint32_t start = parent ? parent : qta_root(cb, cl);
    /* breadth over the parent's children and theirs */
    uint32_t todo[64];
    int nt = 0;
    todo[nt++] = start;
    for (int i = 0; i < nt; i++) {
        uint32_t a = qta_find(cb, cl, todo[i], fourcc("sdp "), 0);
        if (!a)
            a = qta_index(cb, cl, todo[i], fourcc("sdp "), 1, NULL);
        if (a) {
            uint32_t n;
            const uint8_t *d = qta_data(cb, cl, a, &n);
            if (d)
                return pres_from_bytes(p, d, n, NULL);
        }
        int k = qta_count(cb, cl, todo[i], 0);
        for (int j = 1; j <= k && nt < 64; j++) {
            uint32_t ch = qta_index(cb, cl, todo[i], 0, j, NULL);
            if (ch && !qta_data(cb, cl, ch, &(uint32_t){ 0 }))
                todo[nt++] = ch;
        }
    }
    return qtsBadDataErr;
}

static void apply_media_params(pres *p, gaddr mp)
{
    if (!mp)
        return;
    for (int k = 0; k < p->ns; k++) {
        stream *s = &p->s[k];
        if (rd32(mp))
            s->width = (int32_t)rd32(mp);
        if (rd32(mp + 4))
            s->height = (int32_t)rd32(mp + 4);
        for (int i = 0; i < 9; i++)
            s->matrix[i] = (int32_t)rd32(mp + 8 + 4 * (uint32_t)i);
        if (!s->matrix[8])
            s->matrix[0] = s->matrix[4] = 0x10000, s->matrix[8] = 0x40000000;
        s->gworld = rd32(mp + 44);
        s->gdh = rd32(mp + 48);
        if (rd32(mp + 52)) {
            s->clip = tb_NewRgn();
            tb_CopyRgn(rd32(mp + 52), s->clip);
        }
        s->mode = (int16_t)rd16(mp + 56);
        for (int i = 0; i < 3; i++)
            s->op[i] = rd16(mp + 58 + 2 * (uint32_t)i);
        if (rd16(mp + 64) || rd16(mp + 66)) {
            s->vol_l = (int16_t)rd16(mp + 64);
            s->vol_r = (int16_t)rd16(mp + 66);
        }
        s->metering = rd8(mp + 78) != 0;
    }
}

/* ---- Playing ---- */

static int16_t set_up(pres *p)
{
    if (p->set_up)
        return 0;
    int16_t err = 0;
    if (!p->net)
        p->net = qts_net_udp(&p->sdp, &err);
    if (!p->net)
        return err;
    err = qts_net_setup(p->net, &p->sdp);
    if (!err)
        p->set_up = true;
    return err;
}

static double npt_now(pres *p) { return p->playing ? p->at + (double)(now_ms() - p->started_ms) / 1000.0 : p->at; }

static int16_t play_from(pres *p, double from)
{
    int16_t err = set_up(p);
    if (!err)
        err = qts_net_play(p->net, from);
    if (err)
        return err;
    if (from >= 0)
        p->at = from;
    p->playing = true;
    p->done = false;
    p->started_ms = now_ms();
    if (p->timebase) {
        tb_SetTimeBaseValue((int32_t)p->timebase, (int32_t)(p->at * p->scale), p->scale);
        tb_SetTimeBaseRate((int32_t)p->timebase, p->pref_rate ? p->pref_rate : 0x10000);
    }
    return 0;
}

static void stop_playing(pres *p)
{
    if (!p->playing)
        return;
    p->at = npt_now(p);
    p->playing = false;
    if (p->net)
        qts_net_pause(p->net);
    if (p->timebase)
        tb_SetTimeBaseRate((int32_t)p->timebase, 0);
    for (int k = 0; k < p->ns; k++)
        sound_host_stop(sound_id(&p->s[k]));
}

/* Where a stream's pictures go: its GWorld or the presentation's movie's,
 * in the box its dimensions and matrix give (or the movie's box). */
static bool picture_place(stream *s, gaddr *port, gaddr *gdh, qrect *r)
{
    pres *p = s->p;
    *port = s->gworld;
    *gdh = s->gdh;
    if (p->movie) {
        gaddr b = tb_NewPtr(8, false, true);
        tb_GetMovieGWorld((int32_t)p->movie, b, b + 4);
        *port = rd32(b);
        *gdh = rd32(b + 4);
        tb_GetMovieBox(p->movie, b);
        *r = qrect_get(b);
        tb_DisposePtr(b);
        return *port != 0;
    }
    if (!*port)
        return false;
    int w = s->width ? s->width >> 16 : s->fw, h = s->height ? s->height >> 16 : s->fh;
    double sx = s->matrix[0] / 65536.0, sy = s->matrix[4] / 65536.0;
    int tx = s->matrix[6] >> 16, ty = s->matrix[7] >> 16;
    *r = (qrect){ (int16_t)ty, (int16_t)tx, (int16_t)(ty + h * sy), (int16_t)(tx + w * sx) };
    return true;
}

static void draw_stream(stream *s)
{
    gaddr port, gdh;
    qrect r;
    if (!s->frame || !s->enabled || !s->presenting || !picture_place(s, &port, &gdh, &r))
        return;
    gaddr saved = tb_NewPtr(8, false, true);
    tb_GetGWorld(saved, saved + 4);
    tb_SetGWorld(port, gdh);
    gaddr old_clip = 0;
    if (s->clip) {
        old_clip = tb_NewRgn();
        tb_GetClip(old_clip);
        gaddr cr = tb_NewRgn();
        tb_SectRgn(old_clip, s->clip, cr);
        tb_SetClip(cr);
        tb_DisposeRgn(cr);
    }
    qd_draw_rgba(s->frame, s->fw, s->fh, r);
    if (old_clip) {
        tb_SetClip(old_clip);
        tb_DisposeRgn(old_clip);
    }
    tb_SetGWorld(rd32(saved), rd32(saved + 4));
    tb_DisposePtr(saved);
}

static void take_packet(void *ctx, int media, const uint8_t *pkt, size_t len)
{
    pres *p = ctx;
    stream *s = NULL;
    for (int k = 0; k < p->ns && !s; k++)
        if (p->s[k].media == media)
            s = &p->s[k];
    if (!s)
        return;
    s->packets++;
    qts_unit u;
    if (!s->depay || !qts_depay_packet(s->depay, pkt, len, &u))
        return;
    s->units++;
    if (u.rgba) {
        free(s->frame);
        s->frame = u.rgba;
        s->fw = u.w;
        s->fh = u.h;
        if (s->type != fourcc("vide"))
            s->type = fourcc("vide");
        draw_stream(s);
    }
    if (u.pcm) {
        pres *pp = s->p;
        int vol = (s->vol_l + s->vol_r) / 2;
        if (pp->movie)
            vol = vol * tb_GetMovieVolume((int32_t)pp->movie) / 0x100;
        if (s->enabled && s->presenting && vol > 0 && pp->playing)
            sound_host_pcm(sound_id(s), u.pcm, (int)u.frames, u.channels, u.rate, (float)vol / 256.0f);
        free(u.pcm);
        if (s->type != fourcc("soun"))
            s->type = fourcc("soun");
    }
}

static void idle(pres *p)
{
    if (!p->net)
        return;
    qts_net_poll(p->net, take_packet, p);
    if (p->playing && !p->done && p->sdp.range_end > 0 && npt_now(p) >= p->sdp.range_end) {
        p->done = true;
        notify(p, 0, "pdon", 0);
    }
}

/* ---- The calls ---- */

static void QTS_InitializeQTS(ppc_cpu *c) { RET(0); }
static void QTS_TerminateQTS(ppc_cpu *c) { RET(0); }

static int16_t new_presentation(pres **out, uint32_t type, gaddr data, uint32_t len, int32_t flags, int32_t scale,
                                gaddr media, gaddr proc, gaddr refcon)
{
    *out = NULL;
    if (flags & kQTSSendMediaFlag) /* broadcasting: Blue Box sends nothing */
        return qtsUnsupportedFeatureErr;
    pres *p = pres_new(scale, flags, proc, refcon);
    if (!p)
        return memFullErrQ;
    int16_t err;
    if (type == fourcc("sdp ")) {
        uint8_t *b = malloc(len + 1);
        gget(b, data, len);
        err = data ? pres_from_bytes(p, b, len, NULL) : paramErrQ;
        free(b);
    } else if (type == fourcc("fspc")) {
        char path[1024];
        size_t n;
        uint8_t *b = spec_path(data, path, sizeof path) ? file_bytes(path, &n) : NULL;
        err = b ? pres_from_bytes(p, b, n, NULL) : fnfErrQ;
        free(b);
    } else if (type == fourcc("hndl")) {
        gaddr h = data ? rd32(data) : 0;
        if (h && rd32(h)) {
            uint32_t hl = (uint32_t)tb_GetHandleSize(h);
            uint8_t *b = malloc(hl + 1);
            gget(b, rd32(h), hl);
            err = pres_from_bytes(p, b, hl, NULL);
            free(b);
        } else {
            err = paramErrQ;
        }
    } else if (type == fourcc("dref")) {
        err = data ? pres_from_dref(p, rd32(data), rd32(data + 4)) : paramErrQ;
    } else if (type == fourcc("qtac")) {
        err = pres_from_atoms(p, data, 0);
    } else if (type == fourcc("qtat")) {
        err = data ? pres_from_atoms(p, rd32(data), rd32(data + 4)) : paramErrQ;
    } else {
        err = qtsUnsupportedDataTypeErr;
    }
    if (err) {
        pres_free(p);
        return err;
    }
    add_streams(p, true);
    apply_media_params(p, media);
    if (flags & kQTSAutoModeFlag) { /* auto mode: it plays as soon as it can */
        err = play_from(p, 0);
        if (err) {
            notify_error(p, err, "The presentation could not be started.");
            err = 0;
        } else {
            notify_stream(p, "sack", NULL);
        }
    }
    *out = p;
    return 0;
}

static void QTS_QTSNewPresentation(ppc_cpu *c)
{
    gaddr prm = ARG(0), out = ARG(1);
    if (!prm || !out) {
        RET(paramErrQ);
        return;
    }
    pres *p;
    int16_t err = new_presentation(&p, rd32(prm), rd32(prm + 4), rd32(prm + 8), (int32_t)rd32(prm + 16),
                                   (int32_t)rd32(prm + 20), rd32(prm + 24), rd32(prm + 28), rd32(prm + 32));
    wr32(out, p ? p->token : 0);
    RET(err);
}

static void QTS_QTSNewPresentationFromData(ppc_cpu *c)
{
    gaddr len64 = ARG(2), prm = ARG(3), out = ARG(4);
    if (!out) {
        RET(paramErrQ);
        return;
    }
    uint32_t len = len64 ? rd32(len64 + 4) : 0;
    pres *p;
    int16_t err = new_presentation(&p, ARG(0), ARG(1), len, prm ? (int32_t)rd32(prm + 8) : 0,
                                   prm ? (int32_t)rd32(prm + 12) : 0, prm ? rd32(prm + 16) : 0,
                                   prm ? rd32(prm + 20) : 0, prm ? rd32(prm + 24) : 0);
    wr32(out, p ? p->token : 0);
    RET(err);
}

static void QTS_QTSNewPresentationFromFile(ppc_cpu *c)
{
    gaddr prm = ARG(1), out = ARG(2);
    if (!out) {
        RET(paramErrQ);
        return;
    }
    pres *p;
    int16_t err = new_presentation(&p, fourcc("fspc"), ARG(0), 0, prm ? (int32_t)rd32(prm + 8) : 0,
                                   prm ? (int32_t)rd32(prm + 12) : 0, prm ? rd32(prm + 16) : 0,
                                   prm ? rd32(prm + 20) : 0, prm ? rd32(prm + 24) : 0);
    wr32(out, p ? p->token : 0);
    RET(err);
}

static void QTS_QTSNewPresentationFromDataRef(ppc_cpu *c)
{
    gaddr prm = ARG(2), out = ARG(3);
    if (!out) {
        RET(paramErrQ);
        return;
    }
    gaddr rec = tb_NewPtr(8, false, true);
    wr32(rec, ARG(1));
    wr32(rec + 4, ARG(0));
    pres *p;
    int16_t err = new_presentation(&p, fourcc("dref"), rec, 0, prm ? (int32_t)rd32(prm + 8) : 0,
                                   prm ? (int32_t)rd32(prm + 12) : 0, prm ? rd32(prm + 16) : 0,
                                   prm ? rd32(prm + 20) : 0, prm ? rd32(prm + 24) : 0);
    tb_DisposePtr(rec);
    wr32(out, p ? p->token : 0);
    RET(err);
}

static void QTS_QTSDisposePresentation(ppc_cpu *c)
{
    pres *p = pres_of(ARG(0));
    if (!p) {
        RET(paramErrQ);
        return;
    }
    stop_playing(p);
    notify(p, 0, "xprs", p->token);
    pres_free(p);
    RET(0);
}

#define PRES_OR_FAIL()          \
    pres *p = pres_of(ARG(0));  \
    if (!p) {                   \
        RET(paramErrQ);         \
        return;                 \
    }
#define STREAM_OR_FAIL(k)       \
    stream *s;                  \
    if (!target(p, ARG(k), &s)) { \
        RET(paramErrQ);         \
        return;                 \
    }

static void QTS_QTSPresGetFlags(ppc_cpu *c)
{
    PRES_OR_FAIL();
    if (ARG(1))
        wr32(ARG(1), (uint32_t)p->flags);
    RET(0);
}

static void QTS_QTSPresSetFlags(ppc_cpu *c)
{
    PRES_OR_FAIL();
    p->flags = (p->flags & ~(int32_t)ARG(2)) | ((int32_t)ARG(1) & (int32_t)ARG(2));
    RET(0);
}

static void QTS_QTSPresGetTimeBase(ppc_cpu *c)
{
    PRES_OR_FAIL();
    if (ARG(1))
        wr32(ARG(1), p->timebase);
    RET(0);
}

static void QTS_QTSPresGetTimeScale(ppc_cpu *c)
{
    PRES_OR_FAIL();
    if (ARG(1))
        wr32(ARG(1), (uint32_t)p->scale);
    RET(0);
}

static void QTS_QTSPresSetNotificationProc(ppc_cpu *c)
{
    PRES_OR_FAIL();
    p->notify = ARG(1);
    p->refcon = ARG(2);
    RET(0);
}

static void QTS_QTSPresGetNotificationProc(ppc_cpu *c)
{
    PRES_OR_FAIL();
    if (ARG(1))
        wr32(ARG(1), p->notify);
    if (ARG(2))
        wr32(ARG(2), p->refcon);
    RET(0);
}

static void QTS_QTSPresGetNumStreams(ppc_cpu *c)
{
    pres *p = pres_of(ARG(0));
    RET(p ? p->ns : 0);
}

static void QTS_QTSPresGetIndStream(ppc_cpu *c)
{
    pres *p = pres_of(ARG(0));
    uint32_t i = ARG(1);
    RET(p && i >= 1 && i <= (uint32_t)p->ns ? p->s[i - 1].token : 0);
}

static void QTS_QTSGetStreamPresentation(ppc_cpu *c)
{
    pres *p = NULL;
    RET(stream_of(ARG(0), &p) ? p->token : 0);
}

static void QTS_QTSPresNewStream(ppc_cpu *c)
{
    PRES_OR_FAIL();
    uint32_t type = ARG(1);
    gaddr data = ARG(2), out = ARG(5);
    uint32_t len = ARG(3);
    if (out)
        wr32(out, 0);
    if (type != fourcc("sdp ")) {
        RET(qtsUnsupportedDataTypeErr);
        return;
    }
    char *b = malloc(len + 1);
    gget(b, data, len);
    qts_sdp more;
    bool ok = qts_sdp_parse(b, len, p->url, &more);
    free(b);
    if (!ok || !more.nmedia || p->sdp.nmedia >= QTS_MAX_MEDIA) {
        RET(ok ? qtsTooMuchDataErr : qtsBadDataErr);
        return;
    }
    p->sdp.m[p->sdp.nmedia] = more.m[0];
    stream *s = &p->s[p->ns++];
    stream_init(s, p, p->sdp.nmedia++);
    p->set_up = false; /* the new one is set up when it next plays */
    if (out)
        wr32(out, s->token);
    notify_stream(p, "stnw", s);
    RET(0);
}

static void QTS_QTSDisposeStream(ppc_cpu *c)
{
    pres *p;
    stream *s = stream_of(ARG(0), &p);
    if (!s) {
        RET(paramErrQ);
        return;
    }
    notify_stream(p, "stgn", s);
    stream_free(s);
    int k = (int)(s - p->s);
    memmove(&p->s[k], &p->s[k + 1], (size_t)(p->ns - k - 1) * sizeof *s);
    p->ns--;
    RET(0);
}

static void QTS_QTSPresGetDimensions(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    int32_t w = 0, h = 0;
    for (int k = 0; k < p->ns; k++)
        if (!s || s == &p->s[k]) {
            int32_t sw = p->s[k].width ? p->s[k].width : p->s[k].fw << 16;
            int32_t sh = p->s[k].height ? p->s[k].height : p->s[k].fh << 16;
            w = sw > w ? sw : w;
            h = sh > h ? sh : h;
        }
    if (ARG(2))
        wr32(ARG(2), (uint32_t)w);
    if (ARG(3))
        wr32(ARG(3), (uint32_t)h);
    RET(0);
}

static void QTS_QTSPresSetDimensions(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    for (int k = 0; k < p->ns; k++)
        if (!s || s == &p->s[k]) {
            p->s[k].width = (int32_t)ARG(2);
            p->s[k].height = (int32_t)ARG(3);
        }
    RET(0);
}

static void QTS_QTSPresSetClip(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    for (int k = 0; k < p->ns; k++)
        if (!s || s == &p->s[k]) {
            stream *t = &p->s[k];
            if (t->clip)
                tb_DisposeRgn(t->clip);
            t->clip = 0;
            if (ARG(2)) {
                t->clip = tb_NewRgn();
                tb_CopyRgn(ARG(2), t->clip);
            }
        }
    RET(0);
}

static void QTS_QTSPresGetClip(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    stream *t = s ? s : p->ns ? &p->s[0] : NULL;
    gaddr r = 0;
    if (t && t->clip) {
        r = tb_NewRgn();
        tb_CopyRgn(t->clip, r);
    }
    if (ARG(2))
        wr32(ARG(2), r);
    RET(0);
}

static void QTS_QTSPresSetMatrix(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    gaddr m = ARG(2);
    for (int k = 0; k < p->ns; k++)
        if (!s || s == &p->s[k])
            for (int i = 0; i < 9; i++)
                p->s[k].matrix[i] = m ? (int32_t)rd32(m + 4 * (uint32_t)i)
                                      : i == 0 || i == 4 ? 0x10000 : i == 8 ? 0x40000000 : 0;
    RET(0);
}

static void QTS_QTSPresGetMatrix(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    stream *t = s ? s : p->ns ? &p->s[0] : NULL;
    for (int i = 0; i < 9 && ARG(2); i++)
        wr32(ARG(2) + 4 * (uint32_t)i, t ? (uint32_t)t->matrix[i] : i == 0 || i == 4 ? 0x10000u : i == 8 ? 0x40000000u : 0);
    RET(0);
}

static void QTS_QTSPresSetGWorld(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    for (int k = 0; k < p->ns; k++)
        if (!s || s == &p->s[k]) {
            p->s[k].gworld = ARG(2);
            p->s[k].gdh = ARG(3);
        }
    RET(0);
}

static void QTS_QTSPresGetGWorld(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    stream *t = s ? s : p->ns ? &p->s[0] : NULL;
    if (ARG(2))
        wr32(ARG(2), t ? t->gworld : 0);
    if (ARG(3))
        wr32(ARG(3), t ? t->gdh : 0);
    RET(0);
}

static void QTS_QTSPresSetGraphicsMode(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    for (int k = 0; k < p->ns; k++)
        if (!s || s == &p->s[k]) {
            p->s[k].mode = (int16_t)ARG(2);
            for (int i = 0; i < 3 && ARG(3); i++)
                p->s[k].op[i] = rd16(ARG(3) + 2 * (uint32_t)i);
        }
    RET(0);
}

static void QTS_QTSPresGetGraphicsMode(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    stream *t = s ? s : p->ns ? &p->s[0] : NULL;
    if (ARG(2))
        wr16(ARG(2), t ? (uint16_t)t->mode : 0);
    for (int i = 0; i < 3 && ARG(3); i++)
        wr16(ARG(3) + 2 * (uint32_t)i, t ? t->op[i] : 0);
    RET(0);
}

static void QTS_QTSPresGetPicture(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    stream *t = s;
    for (int k = 0; k < p->ns && !t; k++)
        if (p->s[k].frame)
            t = &p->s[k];
    gaddr pic = t && t->frame ? qt_picture_of_rgba(t->frame, t->fw, t->fh) : 0;
    if (ARG(2))
        wr32(ARG(2), pic);
    RET(0);
}

static void QTS_QTSPresInvalidateRegion(ppc_cpu *c)
{
    PRES_OR_FAIL();
    for (int k = 0; k < p->ns; k++)
        draw_stream(&p->s[k]);
    RET(0);
}

static void QTS_QTSPresSetVolumes(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    for (int k = 0; k < p->ns; k++)
        if (!s || s == &p->s[k]) {
            p->s[k].vol_l = (int16_t)ARG(2);
            p->s[k].vol_r = (int16_t)ARG(3);
        }
    RET(0);
}

static void QTS_QTSPresGetVolumes(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    stream *t = s ? s : p->ns ? &p->s[0] : NULL;
    if (ARG(2))
        wr16(ARG(2), t ? (uint16_t)t->vol_l : 0x100);
    if (ARG(3))
        wr16(ARG(3), t ? (uint16_t)t->vol_r : 0x100);
    RET(0);
}

static void QTS_QTSPresSetEnable(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    for (int k = 0; k < p->ns; k++)
        if (!s || s == &p->s[k]) {
            p->s[k].enabled = (ARG(2) & 0xFF) != 0;
            if (!p->s[k].enabled)
                sound_host_stop(sound_id(&p->s[k]));
        }
    RET(0);
}

static void QTS_QTSPresGetEnable(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    bool on = false;
    for (int k = 0; k < p->ns; k++)
        if (!s || s == &p->s[k])
            on |= p->s[k].enabled;
    if (ARG(2))
        wr8(ARG(2), on);
    RET(0);
}

static void QTS_QTSPresSetPresenting(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    for (int k = 0; k < p->ns; k++)
        if (!s || s == &p->s[k])
            p->s[k].presenting = (ARG(2) & 0xFF) != 0;
    RET(0);
}

static void QTS_QTSPresGetPresenting(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    bool on = false;
    for (int k = 0; k < p->ns; k++)
        if (!s || s == &p->s[k])
            on |= p->s[k].presenting;
    if (ARG(2))
        wr8(ARG(2), on);
    RET(0);
}

static void QTS_QTSPresSetPlayHints(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    int32_t f = (int32_t)ARG(2), m = (int32_t)ARG(3);
    if (s)
        s->hints = (s->hints & ~m) | (f & m);
    else
        p->hints = (p->hints & ~m) | (f & m);
    RET(0);
}

static void QTS_QTSPresGetPlayHints(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    if (ARG(2))
        wr32(ARG(2), (uint32_t)(s ? s->hints : p->hints));
    RET(0);
}

static void QTS_QTSPresSetPreferredRate(ppc_cpu *c)
{
    PRES_OR_FAIL();
    int32_t r = (int32_t)ARG(1);
    if (r != 0 && r != 0x10000) { /* a server sends at its own pace */
        RET(qtsUnsupportedRateErr);
        return;
    }
    p->pref_rate = r;
    RET(0);
}

static void QTS_QTSPresGetPreferredRate(ppc_cpu *c)
{
    PRES_OR_FAIL();
    if (ARG(1))
        wr32(ARG(1), (uint32_t)p->pref_rate);
    RET(0);
}

static uint64_t rd64(gaddr a) { return (uint64_t)rd32(a) << 32 | rd32(a + 4); }
static void wr64(gaddr a, uint64_t v)
{
    wr32(a, (uint32_t)(v >> 32));
    wr32(a + 4, (uint32_t)v);
}

static void QTS_QTSPresSetActiveSegment(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    int64_t st = ARG(2) ? (int64_t)rd64(ARG(2)) : 0, du = ARG(3) ? (int64_t)rd64(ARG(3)) : -1;
    if (s)
        s->seg_start = st, s->seg_dur = du;
    else
        p->seg_start = st, p->seg_dur = du;
    RET(0);
}

static void QTS_QTSPresGetActiveSegment(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    int64_t st = s ? s->seg_start : p->seg_start, du = s ? s->seg_dur : p->seg_dur;
    if (du < 0)
        du = p->sdp.range_end > 0 ? (int64_t)(p->sdp.range_end * p->scale) - st : 0x7FFFFFFF;
    if (ARG(2))
        wr64(ARG(2), (uint64_t)st);
    if (ARG(3))
        wr64(ARG(3), (uint64_t)du);
    RET(0);
}

static void QTS_QTSPresHasCharacteristic(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    uint32_t ch = ARG(2);
    bool has = false;
    for (int k = 0; k < p->ns; k++)
        if (!s || s == &p->s[k]) {
            uint32_t t = p->s[k].type;
            if ((ch == fourcc("eyes") && t == fourcc("vide")) || (ch == fourcc("ears") && t == fourcc("soun")))
                has = true;
        }
    if (ARG(3))
        wr8(ARG(3), has);
    RET(0);
}

static void QTS_QTSPresPreroll(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    int16_t err = set_up(p);
    if (!err)
        notify_stream(p, "pack", s);
    RET(err);
}

static void QTS_QTSPresPreroll64(ppc_cpu *c) { QTS_QTSPresPreroll(c); }

static void QTS_QTSPresStart(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    if (s && !s->enabled) {
        s->enabled = true;
        RET(0);
        return;
    }
    int16_t err = p->playing ? 0 : play_from(p, p->at > 0 ? p->at : 0);
    if (!err)
        notify_stream(p, "sack", s);
    RET(err);
}

static void QTS_QTSPresStop(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    if (s) { /* one stream: QuickTime's servers stop all or nothing, so it goes quiet */
        s->enabled = false;
        sound_host_stop(sound_id(s));
    } else {
        stop_playing(p);
    }
    notify_stream(p, "xack", s);
    RET(0);
}

static void skip_to(ppc_cpu *c, pres *p, int64_t t)
{
    double to = (double)t / p->scale;
    if (p->sdp.range_end > 0 && to > p->sdp.range_end) {
        RET(paramErrQ);
        return;
    }
    if (!p->playing) {
        p->at = to;
        RET(0);
        return;
    }
    RET(play_from(p, to));
}

static void QTS_QTSPresSkipTo(ppc_cpu *c)
{
    PRES_OR_FAIL();
    skip_to(c, p, (int64_t)ARG(1));
}

static void QTS_QTSPresSkipTo64(ppc_cpu *c)
{
    PRES_OR_FAIL();
    skip_to(c, p, ARG(1) ? (int64_t)rd64(ARG(1)) : 0);
}

static void QTS_QTSPresPreview(ppc_cpu *c)
{
    PRES_OR_FAIL();
    RET(qtsUnsupportedFeatureErr);
}

static void QTS_QTSPresIdle(ppc_cpu *c)
{
    pres *p = pres_of(ARG(0));
    gaddr io = ARG(1);
    if (p)
        idle(p);
    if (io)
        wr32(io + 16, 0);
    c->pc = c->lr;
}

/* QTSNameParams: maxNameLength, requestedLanguage, returnedActualLanguage, returnedName. */
static void put_name(gaddr np, const char *name)
{
    if (!np || !rd32(np + 12))
        return;
    size_t max = rd32(np) ? rd32(np) : 255, n = strlen(name);
    if (n > max - 1)
        n = max - 1;
    if (n > 255)
        n = 255;
    wr32(np + 8, 0);
    wr8(rd32(np + 12), (uint8_t)n);
    gput(rd32(np + 12) + 1, name, (uint32_t)n);
}

static gaddr stats_container(pres *p, stream *only, gaddr cont, uint32_t parent);

static void QTS_QTSPresGetInfo(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    uint32_t sel = ARG(2);
    gaddr io = ARG(3);
    if (!io) {
        RET(paramErrQ);
        return;
    }
    int32_t r = 0;
    if (sel == fourcc("bufr")) {
        wr32(io, (uint32_t)(s ? s->buffer_time : 3 << 16));
    } else if (sel == fourcc("dura")) { /* QTSDurationAtom: versionAndFlags, timeScale, duration */
        wr32(io, 0);
        wr32(io + 4, (uint32_t)p->scale);
        wr64(io + 8, p->sdp.range_end > 0 ? (uint64_t)(p->sdp.range_end * p->scale) : 0);
    } else if (sel == fourcc("mtyp")) {
        wr32(io, s ? s->type : 0);
    } else if (sel == fourcc("name")) {
        if (s && s->media >= 0) {
            char nm[64];
            snprintf(nm, sizeof nm, "%s", s->type == fourcc("vide") ? "Video" : s->type == fourcc("soun") ? "Sound"
                                                                                                         : "Stream");
            put_name(io, nm);
        } else {
            put_name(io, p->sdp.name[0] ? p->sdp.name : p->url);
        }
    } else if (sel == fourcc("drtt") || sel == fourcc("drti")) {
        uint32_t pk = 0, by = 0;
        if (p->net)
            qts_net_counts(p->net, &pk, &by);
        double secs = p->playing ? (double)(now_ms() - p->started_ms) / 1000.0 : 0;
        wr32(io, rd32(io) + (secs > 0 ? (uint32_t)(by * 8 / secs) : 0));
    } else if (sel == fourcc("drto")) {
        /* nothing sent */
    } else if (sel == fourcc("lpct")) { /* RTP over TCP loses nothing */
        uint32_t pk = 0;
        for (int k = 0; k < p->ns; k++)
            if (!s || s == &p->s[k])
                pk += p->s[k].packets;
        wr32(io, rd32(io) + pk);
        wr32(io + 8, 0);
    } else if (sel == fourcc("nviw")) {
        wr32(io, 1);
    } else if (sel == fourcc("btms")) {
        wr32(io, 0);
    } else if (sel == fourcc("mtrn")) {
        wr8(io, s ? s->metering : false);
    } else if (sel == fourcc("stat")) { /* QTSStatisticsParams: type, container, parentAtom, flags */
        stats_container(p, s, rd32(io + 4), rd32(io + 8));
    } else if (sel == fourcc("meta")) {
        gaddr cont = qt_atoms_new();
        int16_t e;
        if (p->sdp.name[0])
            qt_atoms_insert(cont, 0, fourcc("name"), 1, 0, p->sdp.name, (uint32_t)strlen(p->sdp.name), &e);
        wr32(io, cont);
    } else if (sel == fourcc("gull")) { /* QTSGetURLLinkRecord: no links in what we play */
        wr32(io + 4, 0);
    } else {
        r = qtsBadSelectorErr;
    }
    RET(r);
}

static void QTS_QTSPresSetInfo(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    uint32_t sel = ARG(2);
    gaddr io = ARG(3);
    int32_t r = 0;
    if (sel == fourcc("bufr") && io) {
        for (int k = 0; k < p->ns; k++)
            if (!s || s == &p->s[k])
                p->s[k].buffer_time = (int32_t)rd32(io);
    } else if (sel == fourcc("mtrn") && io) {
        for (int k = 0; k < p->ns; k++)
            if (!s || s == &p->s[k])
                p->s[k].metering = rd8(io) != 0;
    } else {
        r = qtsBadSelectorErr;
    }
    RET(r);
}

static gaddr copy_handle(gaddr h)
{
    if (!h || !rd32(h))
        return 0;
    int32_t n = tb_GetHandleSize(h);
    gaddr d = tb_NewHandle(n, false, false);
    if (d)
        ppc_copy(rd32(d), rd32(h), (uint32_t)n);
    return d;
}

static void QTS_QTSPresSetSettings(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    gaddr spec = ARG(2); /* QTAtomSpec: container, atom */
    gaddr *slot = s ? &s->settings : &p->settings;
    if (*slot)
        tb_DisposeHandle(*slot);
    *slot = spec ? copy_handle(rd32(spec)) : 0;
    RET(0);
}

static void QTS_QTSPresGetSettings(ppc_cpu *c)
{
    PRES_OR_FAIL();
    STREAM_OR_FAIL(1);
    gaddr have = s ? s->settings : p->settings;
    gaddr out = have ? copy_handle(have) : qt_atoms_new();
    if (ARG(2))
        wr32(ARG(2), out);
    RET(0);
}

static void QTS_QTSPresSettingsDialog(ppc_cpu *c)
{
    PRES_OR_FAIL();
    RET(qtsUnsupportedFeatureErr); /* transmission settings: Blue Box sends nothing */
}

static void QTS_QTSPresExport(ppc_cpu *c)
{
    PRES_OR_FAIL();
    gaddr prm = ARG(2);
    if (!prm) {
        RET(paramErrQ);
        return;
    }
    uint32_t type = rd32(prm + 4), dest = rd32(prm + 12);
    gaddr dst = rd32(prm + 16);
    wr32(prm + 28, 0);
    /* the session description, saved to a file */
    if ((type && type != fourcc("sdp ")) || dest != fourcc("fspc") || !dst) {
        RET(qtsUnsupportedFeatureErr);
        return;
    }
    char path[1024];
    tb_FSpCreate(dst, (int32_t)fourcc("TVOD"), (int32_t)fourcc("sdp "), 0);
    if (!spec_path(dst, path, sizeof path)) {
        RET(fnfErrQ);
        return;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        RET(-61); /* wrPermErr */
        return;
    }
    fprintf(f, "v=0\r\ns=%s\r\nt=0 0\r\n", p->sdp.name[0] ? p->sdp.name : "QuickTime");
    if (p->sdp.range_end > 0)
        fprintf(f, "a=range:npt=0-%.3f\r\n", p->sdp.range_end);
    if (p->url[0] && !strncmp(p->url, "rtsp", 4))
        fprintf(f, "a=control:%s\r\n", p->sdp.control);
    for (int i = 0; i < p->sdp.nmedia; i++) {
        const qts_sdp_media *m = &p->sdp.m[i];
        fprintf(f, "m=%s %d RTP/AVP %d\r\n",
                m->media_type == fourcc("vide") ? "video" : m->media_type == fourcc("soun") ? "audio" : "data", m->port,
                m->pt);
        if (m->addr[0])
            fprintf(f, "c=IN IP4 %s\r\n", m->addr);
        if (m->encoding[0])
            fprintf(f, m->channels > 1 ? "a=rtpmap:%d %s/%d/%d\r\n" : "a=rtpmap:%d %s/%d\r\n", m->pt, m->encoding,
                    m->clock, m->channels);
        if (m->fmtp[0])
            fprintf(f, "a=fmtp:%d %s\r\n", m->pt, m->fmtp);
        if (m->control[0])
            fprintf(f, "a=control:%s\r\n", m->control);
    }
    fclose(f);
    RET(0);
}

/* ---- Sourcers: for sending, which Blue Box doesn't ---- */

static void QTS_QTSNewSourcer(ppc_cpu *c)
{
    gaddr init = ARG(1), out = ARG(3);
    if (out)
        wr32(out, 0);
    if (!init || !out) {
        RET(paramErrQ);
        return;
    }
    uint32_t comp = components_find_next(0, fourcc("srcr"), rd32(init + 8), 0);
    if (!comp) {
        RET(cantFindHandler);
        return;
    }
    int16_t err;
    uint32_t ci = components_open_component(comp, &err);
    if (!ci) {
        RET(err ? err : -2047);
        return;
    }
    uint32_t args[1] = { init };
    int32_t r = qtc_call_named(ci, "QTSSourcerInitialize", args);
    if (r) {
        components_close(ci);
        RET(r);
        return;
    }
    wr32(out, ci);
    RET(0);
}

static void QTS_QTSPresAddSourcer(ppc_cpu *c)
{
    PRES_OR_FAIL();
    RET(qtsUnsupportedFeatureErr);
}

static void QTS_QTSPresRemoveSourcer(ppc_cpu *c)
{
    PRES_OR_FAIL();
    RET(paramErrQ); /* none was added */
}

static void QTS_QTSPresGetNumSourcers(ppc_cpu *c) { RET(0); }

static void QTS_QTSPresGetIndSourcer(ppc_cpu *c)
{
    PRES_OR_FAIL();
    if (ARG(3))
        wr32(ARG(3), 0);
    RET(paramErrQ);
}

/* ---- Memory ---- */

enum { kQTSMemAllocClearMem = 1, kQTSMemAllocTrySystemMemFirst = 0x10, kQTSMemAllocAllocatedInSystemMem = 2 };

static void QTS_QTSNewPtr(ppc_cpu *c)
{
    uint32_t n = ARG(0);
    int32_t flags = (int32_t)ARG(1);
    gaddr outFlags = ARG(2);
    bool sys = flags & kQTSMemAllocTrySystemMemFirst;
    gaddr p = tb_NewPtr((int32_t)n, sys, flags & kQTSMemAllocClearMem);
    if (!p && sys) {
        sys = false;
        p = tb_NewPtr((int32_t)n, false, flags & kQTSMemAllocClearMem);
    }
    if (outFlags)
        wr32(outFlags, p && sys ? kQTSMemAllocAllocatedInSystemMem : 0);
    RET(p);
}

static void QTS_QTSNewHandle(ppc_cpu *c)
{
    uint32_t n = ARG(0);
    int32_t flags = (int32_t)ARG(1);
    gaddr outFlags = ARG(2);
    bool sys = flags & kQTSMemAllocTrySystemMemFirst;
    gaddr h = tb_NewHandle((int32_t)n, sys, flags & kQTSMemAllocClearMem);
    if (!h && sys) {
        sys = false;
        h = tb_NewHandle((int32_t)n, false, flags & kQTSMemAllocClearMem);
    }
    if (outFlags)
        wr32(outFlags, h && sys ? kQTSMemAllocAllocatedInSystemMem : 0);
    RET(h);
}

static void QTS_QTSAllocMemPtr(ppc_cpu *c)
{
    RET(tb_NewPtr((int32_t)ARG(0), false, ARG(1) & kQTSMemAllocClearMem));
}

static void QTS_QTSReleaseMemPtr(ppc_cpu *c)
{
    if (ARG(0))
        tb_DisposePtr(ARG(0));
    c->pc = c->lr;
}

/* ---- Stream buffers and messages ----
 *
 * A QTSStreamBuffer is 88 bytes; its data is a block of its own, shared
 * by duplicates: a use count, its size, then the bytes.  The reserved
 * fields after flags hold the block (reserved3).
 */
enum { SB_SIZE = 88, SB_NEXT = 8, SB_RPTR = 12, SB_WPTR = 16, SB_VERSION = 20, SB_BLOCK = 44 };

static gaddr buffer_on(gaddr block)
{
    gaddr b = tb_NewPtr(SB_SIZE, false, true);
    if (!b)
        return 0;
    wr32(b + SB_RPTR, block + 8);
    wr32(b + SB_WPTR, block + 8);
    wr32(b + SB_VERSION, 1);
    wr32(b + SB_BLOCK, block);
    wr32(block, rd32(block) + 1);
    return b;
}

static gaddr new_buffer(uint32_t size, bool clear)
{
    gaddr block = tb_NewPtr((int32_t)(8 + size), false, clear);
    if (!block)
        return 0;
    wr32(block, 0);
    wr32(block + 4, size);
    gaddr b = buffer_on(block);
    if (!b)
        tb_DisposePtr(block);
    return b;
}

static void free_message(gaddr m)
{
    for (int guard = 0; m && guard < 100000; guard++) {
        gaddr next = rd32(m + SB_NEXT), block = rd32(m + SB_BLOCK);
        if (block) {
            uint32_t uses = rd32(block);
            if (uses <= 1)
                tb_DisposePtr(block);
            else
                wr32(block, uses - 1);
        }
        tb_DisposePtr(m);
        m = next;
    }
}

static uint32_t message_length(gaddr m)
{
    uint32_t n = 0;
    for (int guard = 0; m && guard < 100000; guard++, m = rd32(m + SB_NEXT))
        n += rd32(m + SB_WPTR) - rd32(m + SB_RPTR);
    return n;
}

/* A copy of the message: its bytes copied (copy) or shared, in one buffer (flat) or as many. */
static gaddr copy_message(gaddr m, bool copy, bool flat)
{
    if (!m)
        return 0;
    if (flat) {
        uint32_t n = message_length(m);
        gaddr b = new_buffer(n, false);
        if (!b)
            return 0;
        gaddr w = rd32(b + SB_WPTR);
        for (gaddr x = m; x; x = rd32(x + SB_NEXT)) {
            uint32_t k = rd32(x + SB_WPTR) - rd32(x + SB_RPTR);
            ppc_copy(w, rd32(x + SB_RPTR), k);
            w += k;
        }
        wr32(b + SB_WPTR, w);
        for (int i = 0; i < 4; i++)
            wr32(b + 24 + 4 * (uint32_t)i, rd32(m + 24 + 4 * (uint32_t)i));
        return b;
    }
    gaddr head = 0, tail = 0;
    for (gaddr x = m; x; x = rd32(x + SB_NEXT)) {
        gaddr b;
        if (copy || !rd32(x + SB_BLOCK)) {
            uint32_t k = rd32(x + SB_WPTR) - rd32(x + SB_RPTR);
            b = new_buffer(k, false);
            if (b) {
                ppc_copy(rd32(b + SB_RPTR), rd32(x + SB_RPTR), k);
                wr32(b + SB_WPTR, rd32(b + SB_RPTR) + k);
            }
        } else {
            b = buffer_on(rd32(x + SB_BLOCK));
            if (b) {
                wr32(b + SB_RPTR, rd32(x + SB_RPTR));
                wr32(b + SB_WPTR, rd32(x + SB_WPTR));
            }
        }
        if (!b) {
            free_message(head);
            return 0;
        }
        ppc_copy(b + 24, x + 24, 20);  /* metadata, flags */
        ppc_copy(b + 56, x + 56, 32);  /* moreMeta */
        if (tail)
            wr32(tail + SB_NEXT, b);
        else
            head = b;
        tail = b;
    }
    return head;
}

static void QTS_QTSNewStreamBuffer(ppc_cpu *c)
{
    gaddr out = ARG(2);
    if (!out) {
        RET(paramErrQ);
        return;
    }
    gaddr b = new_buffer(ARG(0), false);
    wr32(out, b);
    RET(b ? 0 : memFullErrQ);
}

static void QTS_QTSAllocBuffer(ppc_cpu *c) { RET(new_buffer(ARG(0), false)); }

static void QTS_QTSFreeMessage(ppc_cpu *c)
{
    free_message(ARG(0));
    c->pc = c->lr;
}

static void QTS_QTSMessageLength(ppc_cpu *c) { RET(message_length(ARG(0))); }
static void QTS_QTSDupMessage(ppc_cpu *c) { RET(copy_message(ARG(0), false, false)); }
static void QTS_QTSCopyMessage(ppc_cpu *c) { RET(copy_message(ARG(0), true, false)); }

/* A message in one buffer: itself when it is one already, else a flat
 * copy that replaces it (the original freed). */
static void QTS_QTSFlattenMessage(ppc_cpu *c)
{
    gaddr m = ARG(0);
    if (!m || !rd32(m + SB_NEXT)) {
        RET(m);
        return;
    }
    gaddr f = copy_message(m, true, true);
    if (f)
        free_message(m);
    RET(f);
}

static void QTS_QTSDuplicateMessage(ppc_cpu *c)
{
    gaddr m = ARG(0), out = ARG(2);
    int32_t flags = (int32_t)ARG(1);
    if (!out || !m) {
        RET(paramErrQ);
        return;
    }
    gaddr d = copy_message(m, flags & 1, flags & 2);
    wr32(out, d);
    RET(d ? 0 : memFullErrQ);
}

static void QTS_QTSStreamBufferDataInfo(ppc_cpu *c)
{
    gaddr b = ARG(0), start = ARG(1), max = ARG(2);
    gaddr block = b ? rd32(b + SB_BLOCK) : 0;
    if (start)
        wr32(start, block ? block + 8 : 0);
    if (max)
        wr32(max, block ? rd32(block + 4) : 0);
    c->pc = c->lr;
}

/* ---- Statistics ---- */

typedef struct {
    uint32_t type;
    gaddr stream;
    char name[48], value[48], unit[16];
} stat_item;

#define MAX_HELPERS 16
#define MAX_STATS 32

typedef struct {
    gaddr token;
    gaddr pres, stream;
    uint32_t type;
    stat_item items[MAX_STATS];
    int n, next;
} helper;

static helper helpers[MAX_HELPERS];

static int collect(pres *p, stream *only, stat_item *out, int max)
{
    int n = 0;
    uint32_t pk = 0, by = 0;
    if (p->net)
        qts_net_counts(p->net, &pk, &by);
#define ADD(t, st, nm, unit, ...)                                         \
    if (n < max) {                                                        \
        out[n] = (stat_item){ fourcc(t), st, nm, "", unit };              \
        snprintf(out[n].value, sizeof out[n].value, __VA_ARGS__);         \
        n++;                                                              \
    }
    if (!only) {
        static const char *const status[] = { "Stopped", "Playing" };
        ADD("stat", 0, "Status", "", "%s", status[p->playing]);
        ADD("rpkt", 0, "Packets received", "", "%u", pk);
        ADD("rbyt", 0, "Bytes received", "bytes", "%u", by);
        double secs = p->playing ? (double)(now_ms() - p->started_ms) / 1000.0 : 0;
        ADD("drti", 0, "Data rate", "bits/sec", "%u", secs > 0 ? (unsigned)(by * 8 / secs) : 0u);
    }
    for (int k = 0; k < p->ns; k++) {
        stream *s = &p->s[k];
        if (only && only != s)
            continue;
        const qts_sdp_media *m = s->media >= 0 ? &p->sdp.m[s->media] : NULL;
        ADD("rpnm", s->token, "Payload", "", "%s", m && m->encoding[0] ? m->encoding : "unknown");
        ADD("rpid", s->token, "Payload type", "", "%d", m ? m->pt : -1);
        ADD("rpkt", s->token, "Packets received", "", "%u", s->packets);
        if (s->type == fourcc("vide")) {
            double secs = p->playing ? (double)(now_ms() - p->started_ms) / 1000.0 : 0;
            ADD("fps ", s->token, "Frame rate", "fps", "%.2f", secs > 0 ? s->units / secs : 0.0);
        }
    }
#undef ADD
    return n;
}

/* Statistics as atoms: under the parent, one atom per statistic (its
 * type, its stream's place as ID for a stream's), holding 'name', 'frmt'
 * ('cstr'), 'unin' and the value as a C string in 'data'. */
static gaddr stats_container(pres *p, stream *only, gaddr cont, uint32_t parent)
{
    stat_item it[MAX_STATS];
    int n = collect(p, only, it, MAX_STATS);
    int16_t err;
    for (int i = 0; i < n && cont; i++) {
        uint32_t id = 1;
        for (int k = 0; k < p->ns; k++)
            if (p->s[k].token == it[i].stream)
                id = (uint32_t)k + 2;
        uint32_t a = qt_atoms_insert(cont, parent, it[i].type, id, 0, NULL, 0, &err);
        if (!a)
            continue;
        uint32_t fmt = fourcc("cstr");
        uint8_t fb[4] = { (uint8_t)(fmt >> 24), (uint8_t)(fmt >> 16), (uint8_t)(fmt >> 8), (uint8_t)fmt };
        qt_atoms_insert(cont, a, fourcc("name"), 1, 0, it[i].name, (uint32_t)strlen(it[i].name), &err);
        qt_atoms_insert(cont, a, fourcc("frmt"), 1, 0, fb, 4, &err);
        if (it[i].unit[0])
            qt_atoms_insert(cont, a, fourcc("unin"), 1, 0, it[i].unit, (uint32_t)strlen(it[i].unit), &err);
        qt_atoms_insert(cont, a, fourcc("data"), 1, 0, it[i].value, (uint32_t)strlen(it[i].value) + 1, &err);
    }
    return cont;
}

static helper *helper_of(gaddr t)
{
    for (int i = 0; i < MAX_HELPERS; i++)
        if (t && helpers[i].token == t)
            return &helpers[i];
    return NULL;
}

static void QTS_QTSNewStatHelper(ppc_cpu *c)
{
    gaddr out = ARG(4);
    pres *p = pres_of(ARG(0));
    stream *s;
    if (out)
        wr32(out, 0);
    if (!p || !out || !target(p, ARG(1), &s)) {
        RET(paramErrQ);
        return;
    }
    for (int i = 0; i < MAX_HELPERS; i++)
        if (!helpers[i].token) {
            helper *h = &helpers[i];
            memset(h, 0, sizeof *h);
            h->token = tb_NewPtr(4, false, true);
            h->pres = p->token;
            h->stream = ARG(1);
            h->type = ARG(2);
            wr32(out, h->token);
            RET(0);
            return;
        }
    RET(memFullErrQ);
}

static void QTS_QTSDisposeStatHelper(ppc_cpu *c)
{
    helper *h = helper_of(ARG(0));
    if (!h) {
        RET(paramErrQ);
        return;
    }
    tb_DisposePtr(h->token);
    memset(h, 0, sizeof *h);
    RET(0);
}

static void QTS_QTSStatHelperGetStats(ppc_cpu *c)
{
    helper *h = helper_of(ARG(0));
    pres *p = h ? pres_of(h->pres) : NULL;
    stream *s = NULL;
    if (!p || !target(p, h->stream, &s)) {
        RET(paramErrQ);
        return;
    }
    h->n = collect(p, s, h->items, MAX_STATS);
    h->next = 0;
    RET(0);
}

static void QTS_QTSStatHelperResetIter(ppc_cpu *c)
{
    helper *h = helper_of(ARG(0));
    if (h)
        h->next = 0;
    RET(h ? 0 : paramErrQ);
}

static void QTS_QTSStatHelperGetNumStats(ppc_cpu *c)
{
    helper *h = helper_of(ARG(0));
    RET(h ? h->n : 0);
}

static void put_text(gaddr dst, uint32_t max, const char *s, bool pascal)
{
    if (!dst || !max)
        return;
    size_t n = strlen(s);
    if (pascal) {
        if (n > max - 1)
            n = max - 1;
        if (n > 255)
            n = 255;
        wr8(dst, (uint8_t)n);
        gput(dst + 1, s, (uint32_t)n);
    } else {
        if (n > max - 1)
            n = max - 1;
        gput(dst, s, (uint32_t)n);
        wr8(dst + (uint32_t)n, 0);
    }
}

/* QTSStatHelperNextParams: flags, returnedStatisticsType, returnedStream,
 * maxStatNameLength, returnedStatName, maxStatStringLength,
 * returnedStatString, maxStatUnitLength, returnedStatUnit. */
static void QTS_QTSStatHelperNext(ppc_cpu *c)
{
    helper *h = helper_of(ARG(0));
    gaddr io = ARG(1);
    if (!h || !io || h->next >= h->n) {
        RET(0);
        return;
    }
    stat_item *it = &h->items[h->next++];
    bool pascal = rd32(io) & 1;
    wr32(io + 4, it->type);
    wr32(io + 8, it->stream);
    put_text(rd32(io + 16), rd32(io + 12), it->name, pascal);
    put_text(rd32(io + 24), rd32(io + 20), it->value, pascal);
    put_text(rd32(io + 32), rd32(io + 28), it->unit, pascal);
    RET(1);
}

static void QTS_QTSGetOrMakeStatAtomForStream(ppc_cpu *c)
{
    gaddr cont = ARG(0), out = ARG(2);
    if (!cont || !rd32(cont) || !out) {
        RET(paramErrQ);
        return;
    }
    uint32_t id = 1;
    pres *p;
    stream *s = stream_of(ARG(1), &p);
    if (s)
        id = (uint32_t)(s - p->s) + 2;
    uint32_t a = qta_find(g_mem + rd32(cont), (size_t)tb_GetHandleSize(cont), 0, fourcc("strm"), id);
    int16_t err = 0;
    if (!a)
        a = qt_atoms_insert(cont, 0, fourcc("strm"), id, 0, NULL, 0, &err);
    wr32(out, a);
    RET(a ? 0 : err ? err : memFullErrQ);
}

static void QTS_QTSInsertStatistic(ppc_cpu *c)
{
    gaddr cont = ARG(0);
    uint32_t parent = ARG(1), type = ARG(2), len = ARG(4), fmt = ARG(5);
    gaddr data = ARG(3);
    if (!cont || !rd32(cont)) {
        RET(paramErrQ);
        return;
    }
    const uint8_t *cb = g_mem + rd32(cont);
    size_t cl = (size_t)tb_GetHandleSize(cont);
    int16_t err = 0;
    uint32_t a = qta_find(cb, cl, parent, type, 1);
    if (!a)
        a = qt_atoms_insert(cont, parent, type, 1, 0, NULL, 0, &err);
    if (!a) {
        RET(err ? err : memFullErrQ);
        return;
    }
    uint8_t *v = malloc(len ? len : 1);
    if (len)
        gget(v, data, len);
    uint8_t fb[4] = { (uint8_t)(fmt >> 24), (uint8_t)(fmt >> 16), (uint8_t)(fmt >> 8), (uint8_t)fmt };
    qt_atoms_insert(cont, a, fourcc("frmt"), 1, 0, fb, 4, &err);
    qt_atoms_insert(cont, a, fourcc("data"), 1, 0, v, len, &err);
    free(v);
    RET(err);
}

static int16_t stat_child(gaddr cont, uint32_t parent, uint32_t type, uint32_t child, const void *d, uint32_t n)
{
    if (!cont || !rd32(cont))
        return paramErrQ;
    int16_t err = 0;
    uint32_t a = qta_find(g_mem + rd32(cont), (size_t)tb_GetHandleSize(cont), parent, type, 1);
    if (!a)
        a = qt_atoms_insert(cont, parent, type, 1, 0, NULL, 0, &err);
    if (!a)
        return err ? err : memFullErrQ;
    qt_atoms_insert(cont, a, child, 1, 0, d, n, &err);
    return err;
}

static void QTS_QTSInsertStatisticName(ppc_cpu *c)
{
    uint32_t n = ARG(4);
    char *b = malloc(n ? n : 1);
    if (n)
        gget(b, ARG(3), n);
    int16_t err = stat_child(ARG(0), ARG(1), ARG(2), fourcc("name"), b, n);
    free(b);
    RET(err);
}

static void QTS_QTSInsertStatisticUnits(ppc_cpu *c)
{
    uint32_t units = ARG(3), n = ARG(5);
    uint8_t ub[4] = { (uint8_t)(units >> 24), (uint8_t)(units >> 16), (uint8_t)(units >> 8), (uint8_t)units };
    int16_t err = stat_child(ARG(0), ARG(1), ARG(2), fourcc("unit"), ub, 4);
    if (!err && n && ARG(4)) {
        char *b = malloc(n);
        gget(b, ARG(4), n);
        err = stat_child(ARG(0), ARG(1), ARG(2), fourcc("unin"), b, n);
        free(b);
    }
    RET(err);
}

/* ---- Packetizers and reassemblers: the components there are ---- */

static const struct { const char *name; uint8_t id; uint32_t sub; } payloads[] = {
    { "PCMU", 0, 0x736F756E }, { "PCMA", 8, 0x736F756E }, { "L16", 10, 0x736F756E }, { "L16", 11, 0x736F756E },
    { "DVI4", 5, 0x736F756E }, { "MPA", 14, 0x6D703320 }, { "JPEG", 26, 0x6A706567 }, { "H261", 31, 0x68323631 },
    { "H263", 34, 0x68323633 }, { "H263-1998", 0, 0x3236332B }, { "X-QT", 0, 0x7174696D },
    { "X-QUICKTIME", 0, 0x7174696D }, { "X-QDM", 0, 0x51444D32 }, { "X-SV3V-ES", 0, 0x53565131 },
    { "X-PURE-VOICE", 0, 0x51636C70 },
};

/* The components of a type that take a payload (by its name or static ID;
 * a NULL name and id 0xFF: any), as an atom container of their
 * Components (atoms of the type, IDs from 1). */
static int16_t find_components(uint32_t type, const char *name, int id, uint32_t media_sub, gaddr out)
{
    if (!out)
        return paramErrQ;
    gaddr cont = qt_atoms_new();
    if (!cont)
        return memFullErrQ;
    uint32_t sub = media_sub;
    for (size_t i = 0; i < sizeof payloads / sizeof payloads[0] && !sub; i++)
        if ((name && !strcasecmp(name, payloads[i].name)) || (!name && id >= 0 && id < 96 && payloads[i].id == id &&
                                                              strcmp(payloads[i].name, "H263-1998")))
            sub = payloads[i].sub;
    int16_t err = 0;
    uint32_t n = 0;
    for (uint32_t comp = components_find_next(0, type, 0, 0); comp; comp = components_find_next(comp, type, 0, 0)) {
        uint32_t ctype, csub = 0, cmanu;
        components_describe(comp, &ctype, &csub, &cmanu);
        if (sub && csub != sub && csub != fourcc("gnrc"))
            continue;
        uint8_t cb[4] = { (uint8_t)(comp >> 24), (uint8_t)(comp >> 16), (uint8_t)(comp >> 8), (uint8_t)comp };
        qt_atoms_insert(cont, 0, type, ++n, 0, cb, 4, &err);
    }
    wr32(out, cont);
    return n ? 0 : cantFindHandler;
}

static char *guest_cstr(gaddr s)
{
    static char b[256];
    size_t i = 0;
    for (; s && i < sizeof b - 1 && rd8(s + (uint32_t)i); i++)
        b[i] = (char)rd8(s + (uint32_t)i);
    b[i] = 0;
    return b;
}

static void QTS_QTSFindReassemblerForPayloadID(ppc_cpu *c)
{
    RET(find_components(fourcc("rtpr"), NULL, (int)(ARG(0) & 0xFF), 0, ARG(2)));
}

static void QTS_QTSFindReassemblerForPayloadName(ppc_cpu *c)
{
    RET(find_components(fourcc("rtpr"), guest_cstr(ARG(0)), -1, 0, ARG(2)));
}

static void QTS_QTSFindMediaPacketizerForPayloadID(ppc_cpu *c)
{
    RET(find_components(fourcc("rtpm"), NULL, (int)ARG(0), 0, ARG(2)));
}

static void QTS_QTSFindMediaPacketizerForPayloadName(ppc_cpu *c)
{
    RET(find_components(fourcc("rtpm"), guest_cstr(ARG(0)), -1, 0, ARG(2)));
}

/* MediaPacketizerRequirements: mediaType, dataFormat, capabilityFlags, ... */
static void QTS_QTSFindMediaPacketizer(ppc_cpu *c)
{
    gaddr req = ARG(0);
    uint32_t fmt = req ? rd32(req + 4) : 0, media = req ? rd32(req) : 0;
    gaddr sd = ARG(1);
    if (!fmt && sd && rd32(sd))
        fmt = rd32(rd32(sd) + 4);
    uint32_t sub = fmt == fourcc("jpeg") ? fourcc("jpeg") : fmt == fourcc("h263") ? fourcc("h263")
                 : media == fourcc("soun") ? fourcc("soun") : 0;
    RET(find_components(fourcc("rtpm"), NULL, -1, sub, ARG(3)));
}

static void QTS_QTSFindMediaPacketizerForTrack(ppc_cpu *c)
{
    gaddr track = ARG(0);
    int32_t media = track ? tb_GetTrackMedia((int32_t)track) : 0;
    if (!media) {
        RET(invalidTrack);
        return;
    }
    gaddr desc = tb_NewHandle(0, false, false);
    tb_GetMediaSampleDescription(media, (int32_t)ARG(1), (int32_t)desc);
    uint32_t fmt = tb_GetHandleSize(desc) >= 8 ? rd32(rd32(desc) + 4) : 0;
    tb_DisposeHandle(desc);
    gaddr type = tb_NewPtr(4, false, true);
    tb_GetMediaHandlerDescription(media, type, 0, 0);
    uint32_t mt = rd32(type);
    tb_DisposePtr(type);
    uint32_t sub = fmt == fourcc("jpeg") ? fourcc("jpeg") : fmt == fourcc("h263") ? fourcc("h263")
                 : mt == fourcc("soun") ? fourcc("soun") : 0;
    RET(find_components(fourcc("rtpm"), NULL, -1, sub, ARG(3)));
}

/* ---- Preferences: the connection QuickTime would use, kept for the run ---- */

typedef struct {
    uint32_t protocol;
    int32_t port;
    uint32_t flags, seed;
} conn_pref;

typedef struct {
    uint32_t type;
    int32_t port;
    uint32_t flags, seed;
    uint8_t server[256];
} proxy_pref;

static conn_pref conns[8];
static int nconns;
static proxy_pref proxies[8];
static int nproxies;
static char *no_proxy;
static uint32_t no_proxy_flags, no_proxy_seed;
static char app_name[256];

static void default_prefs(void)
{
    nconns = 0;
    /* what Blue Box uses: RTP over the RTSP connection (TCP, port 554) */
    conns[nconns++] = (conn_pref){ fourcc("tcp "), 554, 0, 1 };
    conns[nconns++] = (conn_pref){ fourcc("udp "), 6970, 0, 1 };
    conns[nconns++] = (conn_pref){ fourcc("http"), 80, 0, 1 };
    nproxies = 0;
    free(no_proxy);
    no_proxy = NULL;
}

static void QTS_QTSPrefsAddConnectionSetting(ppc_cpu *c)
{
    uint32_t proto = ARG(0);
    for (int i = 0; i < nconns; i++)
        if (conns[i].protocol == proto) {
            conns[i] = (conn_pref){ proto, (int32_t)ARG(1), ARG(2), ARG(3) };
            RET(0);
            return;
        }
    if (nconns == 8) {
        RET(memFullErrQ);
        return;
    }
    conns[nconns++] = (conn_pref){ proto, (int32_t)ARG(1), ARG(2), ARG(3) };
    RET(0);
}

static void QTS_QTSPrefsAddProxySetting(ppc_cpu *c)
{
    uint32_t type = ARG(0);
    proxy_pref pp = { type, (int32_t)ARG(1), ARG(2), ARG(3), { 0 } };
    if (ARG(4))
        gget(pp.server, ARG(4), (uint32_t)rd8(ARG(4)) + 1);
    for (int i = 0; i < nproxies; i++)
        if (proxies[i].type == type) {
            proxies[i] = pp;
            RET(0);
            return;
        }
    if (nproxies == 8) {
        RET(memFullErrQ);
        return;
    }
    proxies[nproxies++] = pp;
    RET(0);
}

/* A handle of the matching QTSTransportPrefs (16 bytes each). */
static void QTS_QTSPrefsFindConnectionByType(ppc_cpu *c)
{
    uint32_t proto = ARG(0), flags = ARG(1), mask = ARG(2);
    gaddr outH = ARG(3), count = ARG(4);
    int n = 0;
    for (int i = 0; i < nconns; i++)
        if ((!proto || conns[i].protocol == proto) && (conns[i].flags & mask) == (flags & mask))
            n++;
    gaddr h = n ? tb_NewHandle(n * 16, false, true) : 0;
    int k = 0;
    for (int i = 0; i < nconns && h; i++)
        if ((!proto || conns[i].protocol == proto) && (conns[i].flags & mask) == (flags & mask)) {
            gaddr e = rd32(h) + 16 * (uint32_t)k++;
            wr32(e, conns[i].protocol);
            wr32(e + 4, (uint32_t)conns[i].port);
            wr32(e + 8, conns[i].flags);
            wr32(e + 12, conns[i].seed);
        }
    if (outH)
        wr32(outH, h);
    if (count)
        wr16(count, (uint16_t)n);
    RET(n ? 0 : qtsUnknownValueErr);
}

/* QTSProxyPref: flags, portID, seed, Str255 (268 bytes). */
static void QTS_QTSPrefsFindProxyByType(ppc_cpu *c)
{
    uint32_t type = ARG(0), flags = ARG(1), mask = ARG(2);
    gaddr outH = ARG(3), count = ARG(4);
    int n = 0;
    for (int i = 0; i < nproxies; i++)
        if ((!type || proxies[i].type == type) && (proxies[i].flags & mask) == (flags & mask))
            n++;
    gaddr h = n ? tb_NewHandle(n * 268, false, true) : 0;
    int k = 0;
    for (int i = 0; i < nproxies && h; i++)
        if ((!type || proxies[i].type == type) && (proxies[i].flags & mask) == (flags & mask)) {
            gaddr e = rd32(h) + 268 * (uint32_t)k++;
            wr32(e, proxies[i].flags);
            wr32(e + 4, (uint32_t)proxies[i].port);
            wr32(e + 8, proxies[i].seed);
            gput(e + 12, proxies[i].server, (uint32_t)proxies[i].server[0] + 1);
        }
    if (outH)
        wr32(outH, h);
    if (count)
        wr16(count, (uint16_t)n);
    RET(n ? 0 : qtsUnknownValueErr);
}

static void QTS_QTSPrefsGetActiveConnection(ppc_cpu *c)
{
    uint32_t proto = ARG(0);
    gaddr info = ARG(1);
    if (!info) {
        RET(paramErrQ);
        return;
    }
    /* RTSP and RTP go over TCP, interleaved */
    uint32_t want = proto == fourcc("rtsp") || proto == fourcc("rtp ") || !proto ? fourcc("tcp ") : proto;
    for (int i = 0; i < nconns; i++)
        if (conns[i].protocol == want) {
            wr32(info, conns[i].protocol);
            wr32(info + 4, (uint32_t)conns[i].port);
            wr32(info + 8, conns[i].flags);
            wr32(info + 12, conns[i].seed);
            RET(0);
            return;
        }
    RET(qtsUnknownValueErr);
}

/* QTSNoProxyPref: flags, seed, the list. */
static void QTS_QTSPrefsGetNoProxyURLs(ppc_cpu *c)
{
    gaddr outH = ARG(0);
    if (!outH) {
        RET(paramErrQ);
        return;
    }
    const char *s = no_proxy ? no_proxy : "";
    uint32_t n = (uint32_t)strlen(s) + 1;
    gaddr h = tb_NewHandle((int32_t)(8 + n), false, true);
    if (h) {
        wr32(rd32(h), no_proxy_flags);
        wr32(rd32(h) + 4, no_proxy_seed);
        gput(rd32(h) + 8, s, n);
    }
    wr32(outH, h);
    RET(h ? 0 : memFullErrQ);
}

static void QTS_QTSPrefsSetNoProxyURLs(ppc_cpu *c)
{
    free(no_proxy);
    no_proxy = ARG(0) ? strdup(guest_cstr(ARG(0))) : NULL;
    no_proxy_flags = ARG(1);
    no_proxy_seed = ARG(2);
    RET(0);
}

/* ---- Misc ---- */

static gaddr app_name_ptr;

static void QTS_QTSSetNetworkAppName(ppc_cpu *c)
{
    snprintf(app_name, sizeof app_name, "%s", ARG(0) ? guest_cstr(ARG(0)) : "");
    RET(0);
}

static void QTS_QTSGetNetworkAppName(ppc_cpu *c)
{
    gaddr out = ARG(1);
    if (!out) {
        RET(paramErrQ);
        return;
    }
    if (!app_name_ptr)
        app_name_ptr = tb_NewPtr(256, true, true);
    if (app_name_ptr)
        put_text(app_name_ptr, 256, app_name, false);
    wr32(out, app_name_ptr);
    RET(app_name_ptr ? 0 : memFullErrQ);
}

static const struct { int16_t code; const char *text; } errors[] = {
    { qtsBadSelectorErr, "The streaming component does not support this request." },
    { qtsBadStateErr, "The presentation is not in a state that allows this." },
    { qtsBadDataErr, "The streaming data is damaged or of an unknown kind." },
    { qtsUnsupportedDataTypeErr, "This kind of streaming data is not supported." },
    { qtsUnsupportedRateErr, "The stream cannot be played at that rate." },
    { qtsUnsupportedFeatureErr, "This streaming feature is not supported." },
    { qtsTooMuchDataErr, "There is too much data for the stream." },
    { qtsUnknownValueErr, "The value is not known." },
    { qtsTimeoutErr, "The server did not respond in time." },
    { qtsConnectionFailedErr, "The connection to the server failed." },
    { qtsAddressBusyErr, "The network address is already in use." },
    { -43, "The file or the presentation on the server was not found." },
    { -108, "There is not enough memory." },
};

static void QTS_QTSGetErrorString(ppc_cpu *c)
{
    int16_t code = (int16_t)ARG(0);
    for (size_t i = 0; i < sizeof errors / sizeof errors[0]; i++)
        if (errors[i].code == code) {
            put_text(ARG(2), ARG(1), errors[i].text, false);
            RET(1);
            return;
        }
    RET(0);
}

/* ---- Streaming movies ---- */

/* A movie whose one 'strm' track is the presentation: its sample holds
 * the URL; the presentation plays as the movie does (qt_stream_task). */
static int16_t movie_for(pres *p, gaddr moviep, int16_t flags, const char *url)
{
    add_streams(p, false);
    int w = 0, h = 0;
    for (int i = 0; i < p->sdp.nmedia; i++)
        if (p->sdp.m[i].media_type == fourcc("vide")) {
            w = p->sdp.m[i].width ? p->sdp.m[i].width : 320;
            h = p->sdp.m[i].height ? p->sdp.m[i].height : 240;
        }
    gaddr movie = (gaddr)tb_NewMovie(flags);
    if (!movie)
        return memFullErrQ;
    int32_t track = tb_NewMovieTrack((int32_t)movie, w << 16, h << 16, 0x100);
    int32_t media = track ? tb_NewTrackMedia(track, (int32_t)fourcc("strm"), kPresTimeScale, 0, 0) : 0;
    if (!media) {
        tb_DisposeMovie(movie);
        return memFullErrQ;
    }
    /* a day for presentations that don't say how long they are (live) */
    int32_t dur = p->sdp.range_end > 0 ? (int32_t)(p->sdp.range_end * kPresTimeScale) : 86400 * kPresTimeScale;
    uint32_t ulen = (uint32_t)strlen(url) + 1;
    gaddr desc = tb_NewHandle(16, false, true), data = tb_NewHandle((int32_t)ulen, false, false);
    wr32(rd32(desc), 16);
    wr32(rd32(desc) + 4, fourcc("rtsp"));
    wr16(rd32(desc) + 14, 1);
    gput(rd32(data), url, ulen);
    tb_AddMediaSample(media, (int32_t)data, 0, (int32_t)ulen, dur, (int32_t)desc, 1, 0, 0);
    tb_InsertMediaIntoTrack(track, 0, 0, dur, 0x10000);
    tb_DisposeHandle(desc);
    tb_DisposeHandle(data);
    p->movie = movie;
    p->movie_last = -1;
    wr32(moviep, movie);
    return 0;
}

int16_t qt_stream_open_movie(const char *url, gaddr moviep, int16_t flags)
{
    if (moviep)
        wr32(moviep, 0);
    if (!moviep)
        return paramErrQ;
    pres *p = pres_new(kPresTimeScale, 0, 0, 0);
    if (!p)
        return memFullErrQ;
    int16_t err = pres_from_url(p, url);
    if (!err)
        err = movie_for(p, moviep, flags, url);
    if (err)
        pres_free(p);
    return err;
}

int16_t qt_stream_import_file(const char *path, gaddr moviep, int16_t flags)
{
    size_t n;
    uint8_t *b = file_bytes(path, &n);
    if (!b)
        return noMovieFound;
    bool sdp = n >= 4 && !memcmp(b, "v=0", 3) && (b[3] == '\r' || b[3] == '\n');
    bool text = n >= 8 && !memcmp(b, "RTSPtext", 8);
    if (!sdp && !text) {
        free(b);
        return noMovieFound;
    }
    if (moviep)
        wr32(moviep, 0);
    pres *p = pres_new(kPresTimeScale, 0, 0, 0);
    int16_t err = p ? pres_from_bytes(p, b, n, NULL) : memFullErrQ;
    free(b);
    if (!err)
        err = movie_for(p, moviep, flags, p->url[0] ? p->url : path);
    if (err && p)
        pres_free(p);
    return err;
}

void qt_stream_task(void)
{
    for (int i = 0; i < MAX_PRES; i++) {
        pres *p = press[i];
        if (!p || !p->movie)
            continue;
        if (!qt_movie_model(p->movie)) { /* the movie went: so does its presentation */
            stop_playing(p);
            pres_free(p);
            continue;
        }
        int32_t rate = tb_GetMovieRate((int32_t)p->movie);
        int32_t scale = tb_GetMovieTimeScale((int32_t)p->movie);
        int64_t t = scale > 0 ? (int64_t)tb_GetMovieTime((int32_t)p->movie, 0) * 1000 / scale : 0;
        if (rate && !p->playing) {
            if (play_from(p, (double)t / 1000.0))
                p->movie_last = t; /* failed: tried again when the movie moves */
        } else if (!rate && p->playing) {
            stop_playing(p);
        } else if (p->playing && p->movie_last >= 0 && (t < p->movie_last - 500 || t > p->movie_last + 3000)) {
            play_from(p, (double)t / 1000.0); /* the movie was moved */
        }
        p->movie_last = t;
        idle(p);
    }
}

void qt_stream_reset(void)
{
    for (int i = 0; i < MAX_PRES; i++)
        if (press[i]) {
            qts_net_close(press[i]->net);
            free(press[i]);
            press[i] = NULL;
        }
    memset(helpers, 0, sizeof helpers);
    default_prefs();
    app_name[0] = 0;
    app_name_ptr = 0;
}

/* ---- The exports ---- */

static const ppc_export exports[] = {
#define E(name) { #name, QTS_##name }
    E(InitializeQTS), E(TerminateQTS), E(QTSAllocBuffer), E(QTSAllocMemPtr), E(QTSCopyMessage),
    E(QTSDisposePresentation), E(QTSDisposeStatHelper), E(QTSDisposeStream), E(QTSDupMessage),
    E(QTSDuplicateMessage), E(QTSFindMediaPacketizer), E(QTSFindMediaPacketizerForPayloadID),
    E(QTSFindMediaPacketizerForPayloadName), E(QTSFindMediaPacketizerForTrack), E(QTSFindReassemblerForPayloadID),
    E(QTSFindReassemblerForPayloadName), E(QTSFlattenMessage), E(QTSFreeMessage), E(QTSGetErrorString),
    E(QTSGetNetworkAppName), E(QTSGetOrMakeStatAtomForStream), E(QTSGetStreamPresentation), E(QTSInsertStatistic),
    E(QTSInsertStatisticName), E(QTSInsertStatisticUnits), E(QTSMessageLength), E(QTSNewHandle),
    E(QTSNewPresentation), E(QTSNewPresentationFromData), E(QTSNewPresentationFromDataRef),
    E(QTSNewPresentationFromFile), E(QTSNewPtr), E(QTSNewSourcer), E(QTSNewStatHelper), E(QTSNewStreamBuffer),
    E(QTSPrefsAddConnectionSetting), E(QTSPrefsAddProxySetting), E(QTSPrefsFindConnectionByType),
    E(QTSPrefsFindProxyByType), E(QTSPrefsGetActiveConnection), E(QTSPrefsGetNoProxyURLs), E(QTSPrefsSetNoProxyURLs),
    E(QTSPresAddSourcer), E(QTSPresExport), E(QTSPresGetActiveSegment), E(QTSPresGetClip), E(QTSPresGetDimensions),
    E(QTSPresGetEnable), E(QTSPresGetFlags), E(QTSPresGetGWorld), E(QTSPresGetGraphicsMode), E(QTSPresGetIndSourcer),
    E(QTSPresGetIndStream), E(QTSPresGetInfo), E(QTSPresGetMatrix), E(QTSPresGetNotificationProc),
    E(QTSPresGetNumSourcers), E(QTSPresGetNumStreams), E(QTSPresGetPicture), E(QTSPresGetPlayHints),
    E(QTSPresGetPreferredRate), E(QTSPresGetPresenting), E(QTSPresGetSettings), E(QTSPresGetTimeBase),
    E(QTSPresGetTimeScale), E(QTSPresGetVolumes), E(QTSPresHasCharacteristic), E(QTSPresIdle),
    E(QTSPresInvalidateRegion), E(QTSPresNewStream), E(QTSPresPreroll), E(QTSPresPreroll64), E(QTSPresPreview),
    E(QTSPresRemoveSourcer), E(QTSPresSetActiveSegment), E(QTSPresSetClip), E(QTSPresSetDimensions),
    E(QTSPresSetEnable), E(QTSPresSetFlags), E(QTSPresSetGWorld), E(QTSPresSetGraphicsMode), E(QTSPresSetInfo),
    E(QTSPresSetMatrix), E(QTSPresSetNotificationProc), E(QTSPresSetPlayHints), E(QTSPresSetPreferredRate),
    E(QTSPresSetPresenting), E(QTSPresSetSettings), E(QTSPresSetVolumes), E(QTSPresSettingsDialog), E(QTSPresSkipTo),
    E(QTSPresSkipTo64), E(QTSPresStart), E(QTSPresStop), E(QTSReleaseMemPtr), E(QTSSetNetworkAppName),
    E(QTSStatHelperGetNumStats), E(QTSStatHelperGetStats), E(QTSStatHelperNext), E(QTSStatHelperResetIter),
    E(QTSStreamBufferDataInfo),
#undef E
};

void (*qtstream_export_fn(const char *name))(ppc_cpu *)
{
    if (name[0] != 'Q' && name[0] != 'I' && name[0] != 'T')
        return NULL;
    for (size_t i = 0; i < sizeof exports / sizeof exports[0]; i++)
        if (!strcmp(exports[i].name, name))
            return exports[i].fn;
    return NULL;
}
