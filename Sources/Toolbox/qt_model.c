/*
 * The Movie Toolbox's movies, tracks and media as they are kept: what a
 * 'moov' atom says, in records the calls can read and change, and written
 * back as a 'moov' atom for saving, flattening and playing.
 *
 * A movie (mvhd) has tracks (trak: tkhd, edts/elst, tref, load, udta),
 * each with a media (mdia: mdhd, hdlr, minf with its media header, data
 * references and sample table, udta).  The sample tables are kept as runs
 * of like samples (one per frame of video, one per chunk of sound) with
 * their data's place: stts, ctts, stss, stsz, stsc and stco/co64 are read
 * into them and made again from them.  Atoms with no call to change them
 * are kept as they were and written back.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qt_atoms.h"
#include "qt_model.h"
#include "tb_internal.h"

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static uint32_t four(const char *s) { return (uint32_t)(uint8_t)s[0] << 24 | (uint32_t)(uint8_t)s[1] << 16 | (uint32_t)(uint8_t)s[2] << 8 | (uint8_t)s[3]; }

/* ---- Growing buffers, for writing atoms ---- */

typedef struct {
    uint8_t *b;
    size_t n, cap;
} wbuf;

static void need(wbuf *w, size_t more)
{
    if (w->n + more <= w->cap)
        return;
    w->cap = (w->n + more) * 2 + 256;
    w->b = realloc(w->b, w->cap);
}

static void w8(wbuf *w, uint32_t v)
{
    need(w, 1);
    w->b[w->n++] = (uint8_t)v;
}
static void w16(wbuf *w, uint32_t v)
{
    w8(w, v >> 8);
    w8(w, v);
}
static void w32(wbuf *w, uint32_t v)
{
    w16(w, v >> 16);
    w16(w, v);
}
static void w64(wbuf *w, uint64_t v)
{
    w32(w, (uint32_t)(v >> 32));
    w32(w, (uint32_t)v);
}
static void wbytes(wbuf *w, const void *p, size_t n)
{
    need(w, n);
    if (n)
        memcpy(w->b + w->n, p, n);
    w->n += n;
}
static void wzero(wbuf *w, size_t n)
{
    need(w, n);
    memset(w->b + w->n, 0, n);
    w->n += n;
}

/* An atom begun: its size is filled in when it ends. */
static size_t begin(wbuf *w, const char *type)
{
    size_t at = w->n;
    w32(w, 0);
    wbytes(w, type, 4);
    return at;
}

static void end(wbuf *w, size_t at)
{
    uint32_t n = (uint32_t)(w->n - at);
    w->b[at] = (uint8_t)(n >> 24);
    w->b[at + 1] = (uint8_t)(n >> 16);
    w->b[at + 2] = (uint8_t)(n >> 8);
    w->b[at + 3] = (uint8_t)n;
}

/* ---- Reading atoms ---- */

/* Each child of [p, p + len): its type, contents and their length. */
typedef struct {
    const uint8_t *p, *end;
} walk;

static bool next_atom(walk *w, uint32_t *type, const uint8_t **body, uint32_t *n, const uint8_t **whole)
{
    if (w->p + 8 > w->end)
        return false;
    uint64_t size = be32(w->p), hdr = 8;
    if (size == 1 && w->p + 16 <= w->end) {
        size = be64(w->p + 8);
        hdr = 16;
    } else if (size == 0) {
        size = (uint64_t)(w->end - w->p);
    }
    if (size < hdr || size > (uint64_t)(w->end - w->p))
        return false;
    *type = be32(w->p + 4);
    *body = w->p + hdr;
    *n = (uint32_t)(size - hdr);
    if (whole)
        *whole = w->p;
    w->p += size;
    return true;
}

static void keep(uint8_t **extra, uint32_t *len, const uint8_t *whole, uint32_t n)
{
    *extra = realloc(*extra, *len + n);
    memcpy(*extra + *len, whole, n);
    *len += n;
}

static uint8_t *dup_bytes(const uint8_t *p, uint32_t n)
{
    uint8_t *d = malloc(n ? n : 1);
    if (n)
        memcpy(d, p, n);
    return d;
}

/* A new guest token (a 4-byte handle holding the kind) for a Track or Media. */
static gaddr token(uint32_t kind)
{
    gaddr h = tb_NewHandle(4, false, true);
    if (h && rd32(h))
        wr32(rd32(h), kind);
    return h;
}

void qtm_identity(int32_t m[9])
{
    memset(m, 0, 9 * sizeof *m);
    m[0] = m[4] = 0x10000;
    m[8] = 0x40000000;
}

static void read_matrix(const uint8_t *p, int32_t m[9])
{
    for (int i = 0; i < 9; i++)
        m[i] = (int32_t)be32(p + 4 * i);
    if (!m[0] && !m[1] && !m[3] && !m[4]) /* none at all: the identity */
        qtm_identity(m);
}

static void write_matrix(wbuf *w, const int32_t m[9])
{
    for (int i = 0; i < 9; i++)
        w32(w, (uint32_t)m[i]);
}

/* ---- Sound: a run's bytes from its frames and description ---- */

static uint64_t sound_bytes(const uint8_t *d, uint32_t len, uint32_t frames)
{
    if (!d || len < 36)
        return frames;
    uint32_t format = be32(d + 4);
    uint16_t version = (uint16_t)be16(d + 16);
    uint32_t channels = be16(d + 24), bits = be16(d + 26);
    if (!channels)
        channels = 1;
    if (version >= 1 && len >= 52) {
        uint32_t per_packet = be32(d + 36), bytes_per_frame = be32(d + 44);
        if (per_packet && bytes_per_frame)
            return (uint64_t)(frames + per_packet - 1) / per_packet * bytes_per_frame;
    }
    if (format == four("ima4"))
        return (uint64_t)(frames + 63) / 64 * 34 * channels;
    if (format == four("MAC3"))
        return (uint64_t)(frames + 5) / 6 * 2 * channels;
    if (format == four("MAC6"))
        return (uint64_t)(frames + 5) / 6 * channels;
    if (format == four("ulaw") || format == four("alaw"))
        return (uint64_t)frames * channels;
    return (uint64_t)frames * channels * (bits > 8 ? (bits + 7) / 8 : 1);
}

static uint64_t run_bytes(const qtm_media *md, uint32_t desc, uint32_t count, uint32_t size)
{
    if (md->type == four("soun") && desc >= 1 && (int)desc <= md->ndescs)
        return sound_bytes(md->descs[desc - 1], md->desc_len[desc - 1], count);
    return (uint64_t)count * size;
}

/* ---- Runs ---- */

static qtm_run *push_run(qtm_media *md)
{
    if (md->nruns == md->cap) {
        md->cap = md->cap ? md->cap * 2 : 64;
        md->runs = realloc(md->runs, (size_t)md->cap * sizeof *md->runs);
    }
    qtm_run *r = &md->runs[md->nruns++];
    memset(r, 0, sizeof *r);
    return r;
}

/* The sample table's runs, from its atoms. */
static void read_stbl(qtm_media *md, const uint8_t *p, uint32_t len)
{
    const uint8_t *stts = NULL, *ctts = NULL, *stss = NULL, *stsc = NULL, *stsz = NULL, *stco = NULL, *stsh = NULL;
    uint32_t ntts = 0, nctts = 0, nss = 0, nsc = 0, nsz = 0, nco = 0, nsh = 0;
    bool co64 = false;
    walk w = { p, p + len };
    uint32_t type, n;
    const uint8_t *b, *whole;
    while (next_atom(&w, &type, &b, &n, &whole)) {
        if (type == four("stsd") && n >= 8) {
            uint32_t count = be32(b + 4);
            const uint8_t *e = b + 8, *e_end = b + n;
            for (uint32_t i = 0; i < count && e + 4 <= e_end; i++) {
                uint32_t en = be32(e);
                if (en < 16 || e + en > e_end)
                    break;
                qtm_add_desc(md, e, en);
                e += en;
            }
        } else if (type == four("stts") && n >= 8) {
            stts = b + 8, ntts = be32(b + 4);
            if (8 + (uint64_t)ntts * 8 > n)
                ntts = (n - 8) / 8;
        } else if (type == four("ctts") && n >= 8) {
            ctts = b + 8, nctts = be32(b + 4);
            if (8 + (uint64_t)nctts * 8 > n)
                nctts = (n - 8) / 8;
        } else if (type == four("stss") && n >= 8) {
            stss = b + 8, nss = be32(b + 4);
            if (8 + (uint64_t)nss * 4 > n)
                nss = (n - 8) / 4;
        } else if (type == four("stsh") && n >= 8) {
            stsh = b + 8, nsh = be32(b + 4);
            if (8 + (uint64_t)nsh * 8 > n)
                nsh = (n - 8) / 8;
        } else if (type == four("stsc") && n >= 8) {
            stsc = b + 8, nsc = be32(b + 4);
            if (8 + (uint64_t)nsc * 12 > n)
                nsc = (n - 8) / 12;
        } else if (type == four("stsz") && n >= 12) {
            stsz = b + 4, nsz = be32(b + 8);
            if (!be32(b + 4) && 12 + (uint64_t)nsz * 4 > n)
                nsz = (n - 12) / 4;
        } else if ((type == four("stco") || type == four("co64")) && n >= 8) {
            co64 = type == four("co64");
            stco = b + 8, nco = be32(b + 4);
            if (8 + (uint64_t)nco * (co64 ? 8 : 4) > n)
                nco = (n - 8) / (co64 ? 8 : 4);
        } else {
            keep(&md->stbl_extra, &md->stbl_extra_len, whole, (uint32_t)(w.p - whole));
        }
    }
    if (stsh && nsh) {
        md->shadow = malloc((size_t)nsh * 2 * sizeof *md->shadow);
        for (uint32_t i = 0; i < nsh * 2; i++)
            md->shadow[i] = be32(stsh + 4 * i);
        md->nshadow = (int)nsh;
    }
    if (!stsc || !stco || !stsz)
        return;
    uint32_t constant = be32(stsz);
    const uint8_t *sizes = stsz + 8;
    /* walk the samples, chunk by chunk */
    uint32_t sample = 0, tts_i = 0, tts_left = ntts ? be32(stts) : 0, ss_i = 0;
    uint32_t ctts_i = 0, ctts_left = nctts ? be32(ctts) : 0;
    for (uint32_t c = 0, sc = 0; c < nco; c++) {
        while (sc + 1 < nsc && be32(stsc + (sc + 1) * 12) <= c + 1)
            sc++;
        uint32_t per = nsc ? be32(stsc + sc * 12 + 4) : 0, desc = nsc ? be32(stsc + sc * 12 + 8) : 1;
        uint64_t off = co64 ? be64(stco + (uint64_t)c * 8) : be32(stco + (uint64_t)c * 4);
        qtm_run *r = NULL;
        for (uint32_t k = 0; k < per && sample < nsz; k++, sample++) {
            uint32_t size = constant ? constant : be32(sizes + (uint64_t)sample * 4);
            while (tts_left == 0 && tts_i + 1 < ntts)
                tts_left = be32(stts + ++tts_i * 8);
            uint32_t dur = ntts ? be32(stts + tts_i * 8 + 4) : 1;
            if (tts_left)
                tts_left--;
            while (ctts_left == 0 && ctts_i + 1 < nctts)
                ctts_left = be32(ctts + ++ctts_i * 8);
            int32_t comp = nctts ? (int32_t)be32(ctts + ctts_i * 8 + 4) : 0;
            if (ctts_left)
                ctts_left--;
            bool sync = !stss;
            if (stss) {
                while (ss_i < nss && be32(stss + ss_i * 4) < sample + 1)
                    ss_i++;
                sync = ss_i < nss && be32(stss + ss_i * 4) == sample + 1;
            }
            if (r && r->size == size && r->duration == dur && r->sync == sync && r->comp_offset == comp) {
                r->count++;
            } else {
                uint64_t at = r ? r->offset + r->bytes : off;
                if (r)
                    off = at;
                r = push_run(md);
                *r = (qtm_run){ .offset = at, .count = 1, .size = size, .duration = dur, .desc = desc, .sync = sync,
                                .chunk = k == 0, .comp_offset = comp };
            }
            r->bytes = run_bytes(md, desc, r->count, r->size);
        }
    }
}

static void read_dref(qtm_media *md, const uint8_t *b, uint32_t n)
{
    if (n < 8)
        return;
    uint32_t count = be32(b + 4);
    const uint8_t *e = b + 8, *e_end = b + n;
    for (uint32_t i = 0; i < count && e + 12 <= e_end; i++) {
        uint32_t en = be32(e);
        if (en < 12 || e + en > e_end)
            break;
        qtm_add_dref(md, be32(e + 4), be32(e + 8) & 0xFFFFFF, e + 12, en - 12);
        e += en;
    }
}

static void read_minf(qtm_media *md, const uint8_t *p, uint32_t len)
{
    walk w = { p, p + len };
    uint32_t type, n;
    const uint8_t *b, *whole;
    while (next_atom(&w, &type, &b, &n, &whole)) {
        uint32_t wn = (uint32_t)(w.p - whole);
        if (type == four("vmhd") || type == four("smhd") || type == four("gmhd") || type == four("nmhd") ||
            type == four("hmhd")) {
            free(md->header);
            md->header = dup_bytes(whole, wn);
            md->header_len = wn;
        } else if (type == four("hdlr")) {
            /* the data handler's: made again */
        } else if (type == four("dinf")) {
            walk d = { b, b + n };
            uint32_t dt, dn;
            const uint8_t *db;
            while (next_atom(&d, &dt, &db, &dn, NULL))
                if (dt == four("dref"))
                    read_dref(md, db, dn);
        } else if (type == four("stbl")) {
            read_stbl(md, b, n);
        } else {
            keep(&md->extra, &md->extra_len, whole, wn);
        }
    }
}

static qtm_media *read_mdia(qtm_track *t, const uint8_t *p, uint32_t len)
{
    qtm_media *md = calloc(1, sizeof *md);
    md->scale = 600;
    md->default_dref = 1;
    md->quality = 0;
    walk w = { p, p + len };
    uint32_t type, n;
    const uint8_t *b, *whole;
    /* the handler first: sound runs need the type */
    walk h = w;
    while (next_atom(&h, &type, &b, &n, NULL))
        if (type == four("hdlr") && n >= 24) {
            md->handler_type = be32(b + 4);
            md->type = be32(b + 8);
            md->manufacturer = be32(b + 12);
            md->handler_flags = be32(b + 16);
            md->handler_mask = be32(b + 20);
            uint32_t sn = n > 24 ? b[24] : 0;
            if (sn && 25 + sn <= n && sn < sizeof md->handler_name) {
                memcpy(md->handler_name, b + 25, sn);
                md->handler_name[sn] = 0;
            }
        }
    while (next_atom(&w, &type, &b, &n, &whole)) {
        if (type == four("mdhd") && n >= 24) {
            if (b[0] == 1 && n >= 36) {
                md->created = (uint32_t)be64(b + 4);
                md->modified = (uint32_t)be64(b + 12);
                md->scale = (int32_t)be32(b + 20);
                md->duration = (int64_t)be64(b + 24);
                md->language = (int16_t)be16(b + 32);
                md->quality = (int16_t)be16(b + 34);
            } else {
                md->created = be32(b + 4);
                md->modified = be32(b + 8);
                md->scale = (int32_t)be32(b + 12);
                md->duration = be32(b + 16);
                md->language = (int16_t)be16(b + 20);
                md->quality = (int16_t)be16(b + 22);
            }
            if (md->scale <= 0)
                md->scale = 600;
        } else if (type == four("hdlr")) {
            /* read above */
        } else if (type == four("minf")) {
            read_minf(md, b, n);
        } else if (type == four("udta")) {
            if (md->udata)
                qt_udata_dispose(md->udata);
            md->udata = qt_udata_from(b, n);
        } else {
            keep(&md->extra, &md->extra_len, whole, (uint32_t)(w.p - whole));
        }
    }
    if (!md->ndrefs) /* none: the samples are in this file */
        qtm_add_dref(md, four("alis"), 1, NULL, 0);
    md->token = token(four("medi"));
    (void)t;
    return md;
}

static void read_trak(qtm_movie *m, const uint8_t *p, uint32_t len)
{
    qtm_track *t = calloc(1, sizeof *t);
    qtm_identity(t->matrix);
    t->flags = 0xF;
    int64_t header_duration = -1;
    walk w = { p, p + len };
    uint32_t type, n;
    const uint8_t *b, *whole;
    const uint8_t *elst = NULL;
    uint32_t elst_n = 0;
    while (next_atom(&w, &type, &b, &n, &whole)) {
        uint32_t wn = (uint32_t)(w.p - whole);
        if (type == four("tkhd") && n >= 84) {
            int v1 = b[0] == 1 && n >= 96, o = v1 ? 12 : 0;
            t->flags = be32(b) & 0xFFFFFF;
            t->created = v1 ? (uint32_t)be64(b + 4) : be32(b + 4);
            t->modified = v1 ? (uint32_t)be64(b + 12) : be32(b + 8);
            t->id = be32(b + 12 + (v1 ? 8 : 0));
            header_duration = v1 ? (int64_t)be64(b + 28) : be32(b + 20);
            t->layer = (int16_t)be16(b + 32 + o);
            t->alternate = (int16_t)be16(b + 34 + o);
            t->volume = (int16_t)be16(b + 36 + o);
            read_matrix(b + 40 + o, t->matrix);
            t->width = (int32_t)be32(b + 76 + o);
            t->height = (int32_t)be32(b + 80 + o);
        } else if (type == four("edts")) {
            walk e = { b, b + n };
            uint32_t et, en;
            const uint8_t *eb;
            while (next_atom(&e, &et, &eb, &en, NULL))
                if (et == four("elst"))
                    elst = eb, elst_n = en;
        } else if (type == four("mdia")) {
            if (t->media)
                qtm_free_media(t->media);
            t->media = read_mdia(t, b, n);
        } else if (type == four("udta")) {
            if (t->udata)
                qt_udata_dispose(t->udata);
            t->udata = qt_udata_from(b, n);
        } else if (type == four("tref")) {
            walk r = { b, b + n };
            uint32_t rt, rn;
            const uint8_t *rb;
            while (next_atom(&r, &rt, &rb, &rn, NULL)) {
                t->trefs = realloc(t->trefs, (size_t)(t->ntrefs + 1) * sizeof *t->trefs);
                qtm_tref *f = &t->trefs[t->ntrefs++];
                f->type = rt;
                f->n = (int)(rn / 4);
                f->ids = malloc((size_t)(f->n ? f->n : 1) * sizeof *f->ids);
                for (int i = 0; i < f->n; i++)
                    f->ids[i] = be32(rb + 4 * i);
            }
        } else if (type == four("load") && n >= 16) {
            t->load_time = (int32_t)be32(b);
            t->load_duration = (int32_t)be32(b + 4);
            t->load_flags = (int32_t)be32(b + 8);
            t->load_hints = (int32_t)be32(b + 12);
        } else if (type == four("clip")) {
            walk c = { b, b + n };
            uint32_t ct, cn;
            const uint8_t *cb;
            while (next_atom(&c, &ct, &cb, &cn, NULL))
                if (ct == four("crgn") && cn >= 10) {
                    if (t->clip)
                        tb_DisposeRgn(t->clip);
                    t->clip = tb_NewHandle((int32_t)cn, false, false);
                    if (t->clip && rd32(t->clip))
                        gput(rd32(t->clip), cb, cn);
                }
        } else if (type == four("matt")) {
            free(t->matte_atom);
            t->matte_atom = dup_bytes(whole, wn);
            t->matte_len = wn;
        } else {
            keep(&t->extra, &t->extra_len, whole, wn);
        }
    }
    if (elst && elst_n >= 8) {
        uint32_t count = be32(elst + 4);
        bool v1 = elst[0] == 1;
        uint32_t es = v1 ? 20 : 12;
        for (uint32_t i = 0; i < count && 8 + (uint64_t)(i + 1) * es <= elst_n; i++) {
            const uint8_t *e = elst + 8 + i * es;
            qtm_edit ed = v1 ? (qtm_edit){ (int64_t)be64(e), (int64_t)be64(e + 8), (int32_t)be32(e + 16) }
                             : (qtm_edit){ be32(e), (int32_t)be32(e + 4), (int32_t)be32(e + 8) };
            if (ed.duration <= 0)
                continue;
            t->edits = realloc(t->edits, (size_t)(t->nedits + 1) * sizeof *t->edits);
            t->edits[t->nedits++] = ed;
        }
    } else {
        /* no edit list: the whole media, from the start */
        int64_t d = header_duration;
        if (d <= 0 && t->media && t->media->scale > 0)
            d = t->media->duration * m->scale / t->media->scale;
        if (d > 0) {
            t->edits = malloc(sizeof *t->edits);
            t->edits[0] = (qtm_edit){ d, 0, 0x10000 };
            t->nedits = 1;
        }
    }
    if (t->id >= m->next_track_id)
        m->next_track_id = t->id + 1;
    t->token = token(four("trak"));
    m->tracks = realloc(m->tracks, (size_t)(m->ntracks + 1) * sizeof *m->tracks);
    m->tracks[m->ntracks++] = t;
}

qtm_movie *qtm_parse(const uint8_t *moov, uint32_t len)
{
    if (len < 8 || be32(moov + 4) != four("moov"))
        return NULL;
    qtm_movie *m = qtm_new(600, 0);
    bool have_mvhd = false;
    walk w = { moov + 8, moov + len };
    uint32_t type, n;
    const uint8_t *b, *whole;
    /* the movie header first: tracks need its time scale */
    walk h = w;
    while (next_atom(&h, &type, &b, &n, NULL))
        if (type == four("mvhd") && n >= 100) {
            int v1 = b[0] == 1 && n >= 112, o = v1 ? 12 : 0;
            m->created = v1 ? (uint32_t)be64(b + 4) : be32(b + 4);
            m->modified = v1 ? (uint32_t)be64(b + 12) : be32(b + 8);
            m->scale = (int32_t)be32(b + 12 + (v1 ? 8 : 0));
            m->duration = v1 ? (int64_t)be64(b + 24) : be32(b + 16);
            m->pref_rate = (int32_t)be32(b + 20 + o);
            m->pref_volume = (int16_t)be16(b + 24 + o);
            read_matrix(b + 36 + o, m->matrix);
            m->preview_time = (int32_t)be32(b + 72 + o);
            m->preview_duration = (int32_t)be32(b + 76 + o);
            m->poster_time = (int32_t)be32(b + 80 + o);
            m->sel_time = (int32_t)be32(b + 84 + o);
            m->sel_duration = (int32_t)be32(b + 88 + o);
            m->current_time = (int32_t)be32(b + 92 + o);
            m->next_track_id = be32(b + 96 + o);
            if (m->scale <= 0)
                m->scale = 600;
            have_mvhd = true;
        } else if (type == four("mvhd") && n >= 20) { /* short headers (some tests and old tools) */
            m->created = be32(b + 4);
            m->modified = be32(b + 8);
            m->scale = (int32_t)be32(b + 12);
            m->duration = be32(b + 16);
            if (m->scale <= 0)
                m->scale = 600;
            have_mvhd = true;
        }
    if (!have_mvhd) {
        qtm_free(m);
        return NULL;
    }
    while (next_atom(&w, &type, &b, &n, &whole)) {
        if (type == four("mvhd")) {
            /* read above */
        } else if (type == four("trak")) {
            read_trak(m, b, n);
        } else if (type == four("udta")) {
            if (m->udata)
                qt_udata_dispose(m->udata);
            m->udata = qt_udata_from(b, n);
        } else {
            keep(&m->extra, &m->extra_len, whole, (uint32_t)(w.p - whole));
        }
    }
    if (!m->udata)
        m->udata = qt_udata_new();
    for (int i = 0; i < m->ntracks; i++) {
        if (!m->tracks[i]->udata)
            m->tracks[i]->udata = qt_udata_new();
        if (m->tracks[i]->media && !m->tracks[i]->media->udata)
            m->tracks[i]->media->udata = qt_udata_new();
    }
    if (!m->next_track_id)
        m->next_track_id = 1;
    return m;
}

qtm_movie *qtm_new(int32_t scale, uint32_t now)
{
    qtm_movie *m = calloc(1, sizeof *m);
    m->scale = scale > 0 ? scale : 600;
    m->created = m->modified = now;
    m->pref_rate = 0x10000;
    m->pref_volume = 0x100;
    qtm_identity(m->matrix);
    m->next_track_id = 1;
    m->udata = qt_udata_new();
    m->active_start = m->active_duration = -1;
    return m;
}

void qtm_free_media(qtm_media *md)
{
    if (!md)
        return;
    for (int i = 0; i < md->ndrefs; i++)
        free(md->drefs[i].data);
    free(md->drefs);
    for (int i = 0; i < md->ndescs; i++)
        free(md->descs[i]);
    free(md->descs);
    free(md->desc_len);
    free(md->runs);
    free(md->header);
    free(md->extra);
    free(md->stbl_extra);
    free(md->shadow);
    if (md->udata)
        qt_udata_dispose(md->udata);
    if (md->property_atom)
        tb_DisposeHandle(md->property_atom);
    if (md->input_map)
        tb_DisposeHandle(md->input_map);
    if (md->token)
        tb_DisposeHandle(md->token);
    free(md);
}

static void free_track(qtm_track *t)
{
    qtm_free_media(t->media);
    free(t->edits);
    for (int i = 0; i < t->ntrefs; i++)
        free(t->trefs[i].ids);
    free(t->trefs);
    free(t->matte_atom);
    free(t->extra);
    if (t->udata)
        qt_udata_dispose(t->udata);
    if (t->clip)
        tb_DisposeRgn(t->clip);
    if (t->sound_localization)
        tb_DisposeHandle(t->sound_localization);
    if (t->token)
        tb_DisposeHandle(t->token);
    free(t);
}

void qtm_free(qtm_movie *m)
{
    if (!m)
        return;
    for (int i = 0; i < m->ntracks; i++)
        free_track(m->tracks[i]);
    free(m->tracks);
    if (m->udata)
        qt_udata_dispose(m->udata);
    if (m->ctab)
        tb_DisposeHandle(m->ctab);
    if (m->property_atom)
        tb_DisposeHandle(m->property_atom);
    free(m->extra);
    free(m->self_alias);
    free(m->default_dref);
    free(m->anchor_dref);
    free(m);
}

/* ---- Tracks and media ---- */

qtm_track *qtm_add_track(qtm_movie *m, int32_t width, int32_t height, int16_t volume, uint32_t now)
{
    qtm_track *t = calloc(1, sizeof *t);
    t->id = m->next_track_id++;
    t->flags = 0xF; /* enabled, in the movie, preview and poster */
    t->created = t->modified = now;
    t->volume = volume;
    t->width = width;
    t->height = height;
    qtm_identity(t->matrix);
    t->udata = qt_udata_new();
    t->token = token(four("trak"));
    m->tracks = realloc(m->tracks, (size_t)(m->ntracks + 1) * sizeof *m->tracks);
    m->tracks[m->ntracks++] = t;
    return t;
}

void qtm_remove_track(qtm_movie *m, qtm_track *t)
{
    for (int i = 0; i < m->ntracks; i++)
        if (m->tracks[i] == t) {
            memmove(&m->tracks[i], &m->tracks[i + 1], (size_t)(m->ntracks - i - 1) * sizeof *m->tracks);
            m->ntracks--;
            free_track(t);
            qtm_update_duration(m);
            return;
        }
}

qtm_track *qtm_track_by_id(const qtm_movie *m, uint32_t id)
{
    for (int i = 0; i < m->ntracks; i++)
        if (m->tracks[i]->id == id)
            return m->tracks[i];
    return NULL;
}

/* The media header and handler a new media of the type gets. */
static void default_header(qtm_media *md)
{
    wbuf w = { 0 };
    size_t a;
    if (md->type == four("vide")) {
        a = begin(&w, "vmhd");
        w32(&w, 1); /* flags: no lean ahead */
        w16(&w, 0x40); /* ditherCopy */
        wzero(&w, 6);
    } else if (md->type == four("soun")) {
        a = begin(&w, "smhd");
        w32(&w, 0);
        w16(&w, 0); /* balance */
        w16(&w, 0);
    } else {
        a = begin(&w, "gmhd");
        size_t g = begin(&w, "gmin");
        w32(&w, 0);
        w16(&w, 0x40);
        w16(&w, 0x8000);
        w16(&w, 0x8000);
        w16(&w, 0x8000);
        w16(&w, 0);
        w16(&w, 0);
        end(&w, g);
    }
    end(&w, a);
    md->header = w.b;
    md->header_len = (uint32_t)w.n;
}

qtm_media *qtm_new_media(qtm_track *t, uint32_t type, int32_t scale, uint32_t now)
{
    qtm_media *md = calloc(1, sizeof *md);
    md->type = type;
    md->handler_type = four("mhlr");
    md->manufacturer = four("appl");
    md->scale = scale > 0 ? scale : 600;
    md->created = md->modified = now;
    md->default_dref = 1;
    md->udata = qt_udata_new();
    md->token = token(four("medi"));
    const char *name = type == four("vide") ? "Apple Video Media Handler"
                     : type == four("soun") ? "Apple Sound Media Handler"
                     : type == four("text") ? "Apple Text Media Handler"
                                            : "Apple Media Handler";
    snprintf(md->handler_name, sizeof md->handler_name, "%s", name);
    default_header(md);
    if (t->media)
        qtm_free_media(t->media);
    t->media = md;
    return md;
}

/* ---- Durations ---- */

int64_t qtm_track_duration(const qtm_track *t)
{
    int64_t d = 0;
    for (int i = 0; i < t->nedits; i++)
        d += t->edits[i].duration;
    return d;
}

void qtm_update_duration(qtm_movie *m)
{
    int64_t d = 0;
    for (int i = 0; i < m->ntracks; i++) {
        int64_t t = qtm_track_duration(m->tracks[i]);
        if (t > d)
            d = t;
    }
    m->duration = d;
}

int64_t qtm_media_samples_duration(const qtm_media *md)
{
    int64_t d = 0;
    for (int i = 0; i < md->nruns; i++)
        d += (int64_t)md->runs[i].count * md->runs[i].duration;
    return d;
}

/* ---- Samples ---- */

uint32_t qtm_sample_count(const qtm_media *md)
{
    uint64_t n = 0;
    for (int i = 0; i < md->nruns; i++)
        n += md->runs[i].count;
    return (uint32_t)n;
}

uint32_t qtm_sync_count(const qtm_media *md)
{
    uint64_t n = 0;
    for (int i = 0; i < md->nruns; i++)
        if (md->runs[i].sync)
            n += md->runs[i].count;
    return (uint32_t)n;
}

uint32_t qtm_sample_at(const qtm_media *md, int64_t t, int64_t *start, int64_t *dur)
{
    int64_t at = 0;
    uint64_t n = 0;
    for (int i = 0; i < md->nruns; i++) {
        const qtm_run *r = &md->runs[i];
        int64_t span = (int64_t)r->count * r->duration;
        if (t < at + span && r->duration) {
            uint64_t k = (uint64_t)((t - at) / r->duration);
            if (start)
                *start = at + (int64_t)k * r->duration;
            if (dur)
                *dur = r->duration;
            return (uint32_t)(n + k + 1);
        }
        at += span;
        n += r->count;
    }
    return 0;
}

bool qtm_sample_time(const qtm_media *md, uint32_t s, int64_t *start, int64_t *dur)
{
    int64_t at = 0;
    uint64_t n = 0;
    for (int i = 0; s && i < md->nruns; i++) {
        const qtm_run *r = &md->runs[i];
        if (s <= n + r->count) {
            if (start)
                *start = at + (int64_t)(s - n - 1) * r->duration;
            if (dur)
                *dur = r->duration;
            return true;
        }
        at += (int64_t)r->count * r->duration;
        n += r->count;
    }
    return false;
}

bool qtm_sample_info(const qtm_media *md, uint32_t s, uint64_t *offset, uint64_t *bytes, uint32_t *desc, bool *sync,
                     int *run)
{
    uint64_t n = 0;
    for (int i = 0; s && i < md->nruns; i++) {
        const qtm_run *r = &md->runs[i];
        if (s <= n + r->count) {
            uint32_t k = (uint32_t)(s - n - 1);
            uint64_t each = r->count ? r->bytes / r->count : 0;
            if (md->type == four("soun")) /* frames: the run's bytes in proportion */
                each = r->count ? r->bytes * k / r->count : 0;
            if (offset)
                *offset = r->offset + (md->type == four("soun") ? each : each * k);
            if (bytes)
                *bytes = md->type == four("soun") ? (r->count ? r->bytes / r->count : 0) : each;
            if (desc)
                *desc = r->desc;
            if (sync)
                *sync = r->sync;
            if (run)
                *run = i;
            return true;
        }
        n += r->count;
    }
    return false;
}

void qtm_add_samples(qtm_media *md, uint64_t offset, uint64_t bytes, uint32_t count, uint32_t size, uint32_t duration,
                     uint32_t desc, bool sync, bool new_chunk)
{
    if (!count)
        return;
    qtm_run *last = md->nruns ? &md->runs[md->nruns - 1] : NULL;
    bool follows = last && last->desc == desc && last->offset + last->bytes == offset;
    if (follows && !new_chunk && last->size == size && last->duration == duration && last->sync == sync &&
        !last->comp_offset) {
        last->count += count;
        last->bytes = md->type == four("soun") ? run_bytes(md, desc, last->count, size) : last->bytes + bytes;
        return;
    }
    qtm_run *r = push_run(md);
    *r = (qtm_run){ .offset = offset, .bytes = bytes, .count = count, .size = size, .duration = duration,
                    .desc = desc, .sync = sync, .chunk = new_chunk || !follows };
    /* a chunk holds samples of one description, in one place */
    if (md->chunk_size > 0 && !r->chunk) {
        uint64_t in_chunk = 0;
        for (int i = md->nruns - 1; i >= 0; i--) {
            in_chunk += md->runs[i].bytes;
            if (md->runs[i].chunk)
                break;
        }
        if (in_chunk > (uint64_t)md->chunk_size)
            r->chunk = true;
    }
}

int qtm_find_desc(const qtm_media *md, const uint8_t *d, uint32_t len)
{
    for (int i = 0; i < md->ndescs; i++)
        if (md->desc_len[i] == len && !memcmp(md->descs[i], d, len))
            return i + 1;
    return 0;
}

int qtm_add_desc(qtm_media *md, const uint8_t *d, uint32_t len)
{
    md->descs = realloc(md->descs, (size_t)(md->ndescs + 1) * sizeof *md->descs);
    md->desc_len = realloc(md->desc_len, (size_t)(md->ndescs + 1) * sizeof *md->desc_len);
    md->descs[md->ndescs] = dup_bytes(d, len);
    md->desc_len[md->ndescs] = len;
    return ++md->ndescs;
}

int16_t qtm_desc_dref(const qtm_media *md, uint32_t desc)
{
    if (desc < 1 || (int)desc > md->ndescs || md->desc_len[desc - 1] < 16)
        return 1;
    int16_t i = (int16_t)be16(md->descs[desc - 1] + 14);
    return i >= 1 ? i : 1;
}

int qtm_add_dref(qtm_media *md, uint32_t type, uint32_t flags, const uint8_t *data, uint32_t len)
{
    md->drefs = realloc(md->drefs, (size_t)(md->ndrefs + 1) * sizeof *md->drefs);
    md->drefs[md->ndrefs] = (qtm_dref){ type, flags, dup_bytes(data, len), len, 0 };
    return ++md->ndrefs;
}

/* ---- Edits ---- */

int64_t qtm_track_to_media(const qtm_track *t, int64_t movie_time, int32_t movie_scale, int32_t *rate)
{
    int64_t at = 0;
    if (!t->media)
        return -1;
    for (int i = 0; i < t->nedits; i++) {
        const qtm_edit *e = &t->edits[i];
        if (movie_time < at + e->duration) {
            if (e->media_time < 0)
                return -1;
            if (rate)
                *rate = e->rate;
            double r = e->rate ? e->rate / 65536.0 : 1.0;
            return e->media_time +
                   (int64_t)((double)(movie_time - at) * t->media->scale / movie_scale * r);
        }
        at += e->duration;
    }
    return -1;
}

void qtm_set_edits(qtm_track *t, const qtm_edit *e, int n)
{
    free(t->edits);
    t->edits = n ? malloc((size_t)n * sizeof *e) : NULL;
    if (n)
        memcpy(t->edits, e, (size_t)n * sizeof *e);
    t->nedits = n;
}

/* A track's box in the movie: its size through its matrix. */
void qtm_track_bounds(const qtm_track *t, int16_t *top, int16_t *left, int16_t *bottom, int16_t *right)
{
    double w = t->width / 65536.0, h = t->height / 65536.0;
    double a = t->matrix[0] / 65536.0, b = t->matrix[1] / 65536.0, c = t->matrix[3] / 65536.0,
           d = t->matrix[4] / 65536.0, tx = t->matrix[6] / 65536.0, ty = t->matrix[7] / 65536.0;
    double xs[4] = { 0, w, 0, w }, ys[4] = { 0, 0, h, h }, x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (int i = 0; i < 4; i++) {
        double x = xs[i] * a + ys[i] * c + tx, y = xs[i] * b + ys[i] * d + ty;
        x0 = x < x0 ? x : x0;
        y0 = y < y0 ? y : y0;
        x1 = x > x1 ? x : x1;
        y1 = y > y1 ? y : y1;
    }
    *left = (int16_t)(x0 < 0 ? x0 - 0.5 : x0 + 0.5);
    *top = (int16_t)(y0 < 0 ? y0 - 0.5 : y0 + 0.5);
    *right = (int16_t)(x1 < 0 ? x1 - 0.5 : x1 + 0.5);
    *bottom = (int16_t)(y1 < 0 ? y1 - 0.5 : y1 + 0.5);
}

/* ---- Writing ---- */

static void write_udta(wbuf *w, gaddr udata)
{
    if (!udata)
        return;
    uint32_t n;
    uint8_t *b = qt_udata_flatten(udata, &n);
    if (n) {
        size_t a = begin(w, "udta");
        wbytes(w, b, n);
        end(w, a);
    }
    free(b);
}

static void write_stbl(wbuf *w, const qtm_media *md, int virtual_track)
{
    size_t stbl = begin(w, "stbl");
    size_t a = begin(w, "stsd");
    w32(w, 0);
    w32(w, (uint32_t)md->ndescs);
    for (int i = 0; i < md->ndescs; i++)
        wbytes(w, md->descs[i], md->desc_len[i]);
    end(w, a);
    /* time to sample */
    a = begin(w, "stts");
    w32(w, 0);
    size_t count_at = w->n;
    w32(w, 0);
    uint32_t entries = 0, run_count = 0, run_dur = 0;
    for (int i = 0; i < md->nruns; i++) {
        const qtm_run *r = &md->runs[i];
        if (run_count && r->duration == run_dur) {
            run_count += r->count;
            continue;
        }
        if (run_count) {
            w32(w, run_count);
            w32(w, run_dur);
            entries++;
        }
        run_count = r->count;
        run_dur = r->duration;
    }
    if (run_count) {
        w32(w, run_count);
        w32(w, run_dur);
        entries++;
    }
    w->b[count_at] = (uint8_t)(entries >> 24), w->b[count_at + 1] = (uint8_t)(entries >> 16);
    w->b[count_at + 2] = (uint8_t)(entries >> 8), w->b[count_at + 3] = (uint8_t)entries;
    end(w, a);
    /* display offsets, if any */
    bool any_comp = false, all_sync = true, same_size = true;
    for (int i = 0; i < md->nruns; i++) {
        any_comp |= md->runs[i].comp_offset != 0;
        all_sync &= md->runs[i].sync;
        same_size &= md->runs[i].size == md->runs[0].size;
    }
    if (any_comp) {
        a = begin(w, "ctts");
        w32(w, 0);
        w32(w, (uint32_t)md->nruns);
        for (int i = 0; i < md->nruns; i++) {
            w32(w, md->runs[i].count);
            w32(w, (uint32_t)md->runs[i].comp_offset);
        }
        end(w, a);
    }
    if (!all_sync) {
        a = begin(w, "stss");
        w32(w, 0);
        count_at = w->n;
        w32(w, 0);
        uint32_t n = 0, s = 0;
        for (int i = 0; i < md->nruns; i++) {
            for (uint32_t k = 0; k < md->runs[i].count; k++) {
                s++;
                if (md->runs[i].sync) {
                    w32(w, s);
                    n++;
                }
            }
        }
        w->b[count_at] = (uint8_t)(n >> 24), w->b[count_at + 1] = (uint8_t)(n >> 16);
        w->b[count_at + 2] = (uint8_t)(n >> 8), w->b[count_at + 3] = (uint8_t)n;
        end(w, a);
    }
    if (md->nshadow) {
        a = begin(w, "stsh");
        w32(w, 0);
        w32(w, (uint32_t)md->nshadow);
        for (int i = 0; i < md->nshadow * 2; i++)
            w32(w, md->shadow[i]);
        end(w, a);
    }
    /* chunks: a run that starts one, and those after it */
    int nchunks = 0;
    for (int i = 0; i < md->nruns; i++)
        nchunks += md->runs[i].chunk || i == 0;
    a = begin(w, "stsc");
    w32(w, 0);
    count_at = w->n;
    w32(w, 0);
    uint32_t entries_sc = 0, last_per = 0, last_desc = 0, chunk = 0;
    bool big = false;
    for (int i = 0; i < md->nruns;) {
        uint32_t per = 0, desc = md->runs[i].desc;
        uint64_t off = md->runs[i].offset;
        if (virtual_track)
            off |= (uint64_t)virtual_track << 48 | (uint64_t)qtm_desc_dref(md, desc) << 40;
        if (off > 0xFFFFFFFFu)
            big = true;
        int j = i;
        do
            per += md->runs[j++].count;
        while (j < md->nruns && !md->runs[j].chunk);
        chunk++;
        if (per != last_per || desc != last_desc) {
            w32(w, chunk);
            w32(w, per);
            w32(w, desc);
            entries_sc++;
            last_per = per;
            last_desc = desc;
        }
        i = j;
    }
    w->b[count_at] = (uint8_t)(entries_sc >> 24), w->b[count_at + 1] = (uint8_t)(entries_sc >> 16);
    w->b[count_at + 2] = (uint8_t)(entries_sc >> 8), w->b[count_at + 3] = (uint8_t)entries_sc;
    end(w, a);
    a = begin(w, "stsz");
    w32(w, 0);
    uint32_t total = qtm_sample_count(md);
    if (same_size && md->nruns) {
        w32(w, md->runs[0].size);
        w32(w, total);
    } else {
        w32(w, 0);
        w32(w, total);
        for (int i = 0; i < md->nruns; i++)
            for (uint32_t k = 0; k < md->runs[i].count; k++)
                w32(w, md->runs[i].size);
    }
    end(w, a);
    a = begin(w, big ? "co64" : "stco");
    w32(w, 0);
    w32(w, (uint32_t)nchunks);
    for (int i = 0; i < md->nruns; i++) {
        if (!(md->runs[i].chunk || i == 0))
            continue;
        uint64_t off = md->runs[i].offset;
        if (virtual_track)
            off |= (uint64_t)virtual_track << 48 | (uint64_t)qtm_desc_dref(md, md->runs[i].desc) << 40;
        if (big)
            w64(w, off);
        else
            w32(w, (uint32_t)off);
    }
    end(w, a);
    wbytes(w, md->stbl_extra, md->stbl_extra_len);
    end(w, stbl);
}

static void write_mdia(wbuf *w, const qtm_media *md, int virtual_track)
{
    size_t mdia = begin(w, "mdia");
    size_t a = begin(w, "mdhd");
    bool v1 = md->duration > 0xFFFFFFFFLL;
    w32(w, v1 ? 0x01000000u : 0);
    if (v1) {
        w64(w, md->created);
        w64(w, md->modified);
        w32(w, (uint32_t)md->scale);
        w64(w, (uint64_t)md->duration);
    } else {
        w32(w, md->created);
        w32(w, md->modified);
        w32(w, (uint32_t)md->scale);
        w32(w, (uint32_t)md->duration);
    }
    w16(w, (uint16_t)md->language);
    w16(w, (uint16_t)md->quality);
    end(w, a);
    a = begin(w, "hdlr");
    w32(w, 0);
    w32(w, md->handler_type ? md->handler_type : four("mhlr"));
    w32(w, md->type);
    w32(w, md->manufacturer);
    w32(w, md->handler_flags);
    w32(w, md->handler_mask);
    size_t sn = strlen(md->handler_name);
    w8(w, (uint32_t)sn);
    wbytes(w, md->handler_name, sn);
    end(w, a);
    size_t minf = begin(w, "minf");
    wbytes(w, md->header, md->header_len);
    a = begin(w, "hdlr");
    w32(w, 0);
    w32(w, four("dhlr"));
    w32(w, md->ndrefs ? md->drefs[0].type : four("alis"));
    w32(w, four("appl"));
    w32(w, 0);
    w32(w, 0);
    static const char dh_name[] = "Apple Alias Data Handler";
    w8(w, sizeof dh_name - 1);
    wbytes(w, dh_name, sizeof dh_name - 1);
    end(w, a);
    size_t dinf = begin(w, "dinf");
    a = begin(w, "dref");
    w32(w, 0);
    w32(w, (uint32_t)md->ndrefs);
    for (int i = 0; i < md->ndrefs; i++) {
        const qtm_dref *d = &md->drefs[i];
        /* a reference into memory can't be kept in a file: it stays in the
         * description for playing, and saving goes through flattening */
        w32(w, 12 + d->len);
        w32(w, d->type);
        w32(w, d->flags & 0xFFFFFF);
        wbytes(w, d->data, d->len);
    }
    end(w, a);
    end(w, dinf);
    write_stbl(w, md, virtual_track);
    wbytes(w, md->extra, md->extra_len);
    end(w, minf);
    write_udta(w, md->udata);
    end(w, mdia);
}

static void write_trak(wbuf *w, const qtm_movie *m, const qtm_track *t, int virtual_track)
{
    size_t trak = begin(w, "trak");
    size_t a = begin(w, "tkhd");
    int64_t dur = qtm_track_duration(t);
    bool v1 = dur > 0xFFFFFFFFLL;
    w32(w, (v1 ? 0x01000000u : 0) | (t->flags & 0xFFFFFF));
    if (v1) {
        w64(w, t->created);
        w64(w, t->modified);
        w32(w, t->id);
        w32(w, 0);
        w64(w, (uint64_t)dur);
    } else {
        w32(w, t->created);
        w32(w, t->modified);
        w32(w, t->id);
        w32(w, 0);
        w32(w, (uint32_t)dur);
    }
    wzero(w, 8);
    w16(w, (uint16_t)t->layer);
    w16(w, (uint16_t)t->alternate);
    w16(w, (uint16_t)t->volume);
    w16(w, 0);
    write_matrix(w, t->matrix);
    w32(w, (uint32_t)t->width);
    w32(w, (uint32_t)t->height);
    end(w, a);
    if (t->clip) {
        uint32_t n = (uint32_t)tb_GetHandleSize(t->clip);
        size_t c = begin(w, "clip");
        size_t r = begin(w, "crgn");
        uint8_t *rb = malloc(n ? n : 1);
        if (n)
            gget(rb, rd32(t->clip), n);
        wbytes(w, rb, n);
        free(rb);
        end(w, r);
        end(w, c);
    }
    if (t->matte_atom)
        wbytes(w, t->matte_atom, t->matte_len);
    if (t->nedits) {
        size_t e = begin(w, "edts");
        a = begin(w, "elst");
        bool big = false;
        for (int i = 0; i < t->nedits; i++)
            big |= t->edits[i].duration > 0xFFFFFFFFLL || t->edits[i].media_time > 0x7FFFFFFFLL;
        w32(w, big ? 0x01000000u : 0);
        w32(w, (uint32_t)t->nedits);
        for (int i = 0; i < t->nedits; i++) {
            if (big) {
                w64(w, (uint64_t)t->edits[i].duration);
                w64(w, (uint64_t)t->edits[i].media_time);
            } else {
                w32(w, (uint32_t)t->edits[i].duration);
                w32(w, (uint32_t)t->edits[i].media_time);
            }
            w32(w, (uint32_t)t->edits[i].rate);
        }
        end(w, a);
        end(w, e);
    }
    if (t->ntrefs) {
        size_t r = begin(w, "tref");
        for (int i = 0; i < t->ntrefs; i++) {
            if (!t->trefs[i].n)
                continue;
            char ty[4] = { (char)(t->trefs[i].type >> 24), (char)(t->trefs[i].type >> 16),
                           (char)(t->trefs[i].type >> 8), (char)t->trefs[i].type };
            size_t x = begin(w, ty);
            for (int k = 0; k < t->trefs[i].n; k++)
                w32(w, t->trefs[i].ids[k]);
            end(w, x);
        }
        end(w, r);
    }
    if (t->load_time || t->load_duration || t->load_flags || t->load_hints) {
        a = begin(w, "load");
        w32(w, (uint32_t)t->load_time);
        w32(w, (uint32_t)t->load_duration);
        w32(w, (uint32_t)t->load_flags);
        w32(w, (uint32_t)t->load_hints);
        end(w, a);
    }
    if (t->media)
        write_mdia(w, t->media, virtual_track);
    write_udta(w, t->udata);
    wbytes(w, t->extra, t->extra_len);
    (void)m;
    end(w, trak);
}

uint8_t *qtm_serialize(const qtm_movie *m, bool virtual_offsets, uint32_t *len)
{
    wbuf w = { 0 };
    size_t moov = begin(&w, "moov");
    size_t a = begin(&w, "mvhd");
    bool v1 = m->duration > 0xFFFFFFFFLL;
    w32(&w, v1 ? 0x01000000u : 0);
    if (v1) {
        w64(&w, m->created);
        w64(&w, m->modified);
        w32(&w, (uint32_t)m->scale);
        w64(&w, (uint64_t)m->duration);
    } else {
        w32(&w, m->created);
        w32(&w, m->modified);
        w32(&w, (uint32_t)m->scale);
        w32(&w, (uint32_t)m->duration);
    }
    w32(&w, (uint32_t)m->pref_rate);
    w16(&w, (uint16_t)m->pref_volume);
    wzero(&w, 10);
    write_matrix(&w, m->matrix);
    w32(&w, (uint32_t)m->preview_time);
    w32(&w, (uint32_t)m->preview_duration);
    w32(&w, (uint32_t)m->poster_time);
    w32(&w, (uint32_t)m->sel_time);
    w32(&w, (uint32_t)m->sel_duration);
    w32(&w, (uint32_t)m->current_time);
    w32(&w, m->next_track_id);
    end(&w, a);
    if (m->ctab && rd32(m->ctab)) {
        uint32_t n = (uint32_t)tb_GetHandleSize(m->ctab);
        a = begin(&w, "ctab");
        uint8_t *cb = malloc(n ? n : 1);
        if (n)
            gget(cb, rd32(m->ctab), n);
        wbytes(&w, cb, n);
        free(cb);
        end(&w, a);
    }
    for (int i = 0; i < m->ntracks; i++)
        write_trak(&w, m, m->tracks[i], virtual_offsets ? i + 1 : 0);
    write_udta(&w, m->udata);
    wbytes(&w, m->extra, m->extra_len);
    end(&w, moov);
    *len = (uint32_t)w.n;
    return w.b;
}
