/*
 * Movie editing on a movie's description: each track's edit list (which
 * stretch of its media shows at which movie time) read from the 'moov'
 * atom, cut, copied and pasted, and written back into a new 'moov' with the
 * durations to match.  The samples never move: an edited movie shows the
 * same media in a new order, as QuickTime's editing did until a movie was
 * flattened.
 *
 * elst entries: track duration (movie time units), media time (the
 * media's units; -1 an empty stretch), rate (Fixed).  A track without an
 * edit list shows its whole media from the start.
 */
#include <stdlib.h>
#include <string.h>

#include "qt_edit.h"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* The child atom of a type in [p, end): its start (header included) and size. */
static const uint8_t *child(const uint8_t *p, const uint8_t *end, const char *type, uint32_t *size)
{
    while (p + 8 <= end) {
        uint32_t n = be32(p);
        if (n < 8 || n > (uint64_t)(end - p))
            return NULL;
        if (!memcmp(p + 4, type, 4)) {
            *size = n;
            return p;
        }
        p += n;
    }
    return NULL;
}

static bool push(qt_edit_track *t, qt_edit e)
{
    if (e.len <= 0)
        return true;
    if (t->n && t->e[t->n - 1].media_time >= 0 && e.media_time >= 0 && t->e[t->n - 1].rate == e.rate) {
        /* joins the one before when it carries straight on */
        qt_edit *last = &t->e[t->n - 1];
        int64_t end = last->media_time + last->len * t->media_scale / t->movie_scale;
        if (end == e.media_time) {
            last->len += e.len;
            return true;
        }
    } else if (t->n && t->e[t->n - 1].media_time < 0 && e.media_time < 0) {
        t->e[t->n - 1].len += e.len;
        return true;
    }
    qt_edit *grown = realloc(t->e, sizeof *grown * (size_t)(t->n + 1));
    if (!grown)
        return false;
    t->e = grown;
    t->e[t->n++] = e;
    return true;
}

bool qt_edits_read(const uint8_t *moov, uint32_t len, qt_edits *out)
{
    memset(out, 0, sizeof *out);
    if (len < 8 || memcmp(moov + 4, "moov", 4))
        return false;
    const uint8_t *end = moov + len;
    uint32_t n;
    const uint8_t *mvhd = child(moov + 8, end, "mvhd", &n);
    if (!mvhd || n < 8 + 20 || mvhd[8] != 0)
        return false; /* version 0 only: QuickTime 2's */
    out->scale = (int32_t)be32(mvhd + 8 + 12);
    if (out->scale <= 0)
        return false;
    for (const uint8_t *t = moov + 8; t + 8 <= end;) {
        uint32_t tn = be32(t);
        if (tn < 8 || tn > (uint64_t)(end - t))
            break;
        if (!memcmp(t + 4, "trak", 4)) {
            qt_edit_track *grown = realloc(out->tracks, sizeof *grown * (size_t)(out->ntracks + 1));
            if (!grown)
                return false;
            out->tracks = grown;
            qt_edit_track *tr = &out->tracks[out->ntracks++];
            memset(tr, 0, sizeof *tr);
            tr->movie_scale = out->scale;
            uint32_t k, mk;
            const uint8_t *tkhd = child(t + 8, t + tn, "tkhd", &k);
            int64_t tduration = tkhd && k >= 8 + 24 ? be32(tkhd + 8 + 20) : 0;
            const uint8_t *mdia = child(t + 8, t + tn, "mdia", &k);
            const uint8_t *mdhd = mdia ? child(mdia + 8, mdia + k, "mdhd", &mk) : NULL;
            tr->media_scale = mdhd && mk >= 8 + 20 ? (int32_t)be32(mdhd + 8 + 12) : out->scale;
            if (tr->media_scale <= 0)
                tr->media_scale = out->scale;
            if (tduration <= 0 && mdhd && mk >= 8 + 20 && mdhd[8] == 0) /* no duration in the header: the media's */
                tduration = (int64_t)be32(mdhd + 8 + 16) * out->scale / tr->media_scale;
            const uint8_t *edts = child(t + 8, t + tn, "edts", &k);
            const uint8_t *elst = edts ? child(edts + 8, edts + k, "elst", &mk) : NULL;
            if (elst && mk >= 16) {
                uint32_t count = be32(elst + 12);
                for (uint32_t i = 0; i < count && 16 + i * 12 + 12 <= mk; i++) {
                    const uint8_t *e = elst + 16 + i * 12;
                    qt_edit ed = { be32(e), (int32_t)be32(e + 4), (int32_t)be32(e + 8) };
                    if (ed.len > 0) { /* kept as they are: no joining on reading */
                        qt_edit *g = realloc(tr->e, sizeof *g * (size_t)(tr->n + 1));
                        if (!g)
                            return false;
                        tr->e = g;
                        tr->e[tr->n++] = ed;
                    }
                }
            } else if (tduration > 0) {
                push(tr, (qt_edit){ tduration, 0, 0x10000 });
            }
        }
        t += tn;
    }
    return true;
}

void qt_edits_free(qt_edits *e)
{
    for (int i = 0; i < e->ntracks; i++)
        free(e->tracks[i].e);
    free(e->tracks);
    memset(e, 0, sizeof *e);
}

int64_t qt_edits_track_duration(const qt_edit_track *t)
{
    int64_t d = 0;
    for (int i = 0; i < t->n; i++)
        d += t->e[i].len;
    return d;
}

int64_t qt_edits_duration(const qt_edits *e)
{
    int64_t d = 0;
    for (int i = 0; i < e->ntracks; i++) {
        int64_t td = qt_edits_track_duration(&e->tracks[i]);
        if (td > d)
            d = td;
    }
    return d;
}

/* Part of an edit: from offset into it, for len (movie units). */
static qt_edit part(const qt_edit_track *t, qt_edit e, int64_t offset, int64_t len)
{
    qt_edit p = { len, e.media_time, e.rate };
    if (e.media_time >= 0)
        p.media_time = e.media_time + offset * t->media_scale / t->movie_scale * e.rate / 0x10000;
    return p;
}

/* The track's edits for [from, from + len), into out (appended). */
static bool extract(const qt_edit_track *t, int64_t from, int64_t len, qt_edit_track *out)
{
    int64_t at = 0, to = from + len;
    for (int i = 0; i < t->n && at < to; i++) {
        int64_t s = at, e = at + t->e[i].len;
        at = e;
        if (e <= from)
            continue;
        int64_t a = s > from ? s : from, b = e < to ? e : to;
        if (!push(out, part(t, t->e[i], a - s, b - a)))
            return false;
    }
    int64_t got = qt_edits_track_duration(out);
    (void)got;
    return true;
}

static void replace(qt_edit_track *t, qt_edit_track *with)
{
    free(t->e);
    t->e = with->e;
    t->n = with->n;
}

bool qt_edits_delete(qt_edits *m, int64_t from, int64_t len)
{
    if (from < 0 || len <= 0)
        return true;
    for (int i = 0; i < m->ntracks; i++) {
        qt_edit_track *t = &m->tracks[i], out = *t;
        out.e = NULL;
        out.n = 0;
        int64_t d = qt_edits_track_duration(t);
        if (!extract(t, 0, from, &out) || !extract(t, from + len, d, &out)) {
            free(out.e);
            return false;
        }
        replace(t, &out);
    }
    return true;
}

bool qt_edits_insert_empty(qt_edits *m, int64_t at, int64_t len)
{
    if (len <= 0)
        return true;
    for (int i = 0; i < m->ntracks; i++) {
        qt_edit_track *t = &m->tracks[i], out = *t;
        out.e = NULL;
        out.n = 0;
        int64_t d = qt_edits_track_duration(t);
        bool ok = extract(t, 0, at, &out);
        if (ok && d > at) /* a track that ends before stays as it is */
            ok = push(&out, (qt_edit){ len, -1, 0x10000 }) && extract(t, at, d, &out);
        if (!ok) {
            free(out.e);
            return false;
        }
        replace(t, &out);
    }
    return true;
}

bool qt_edits_insert(qt_edits *dst, const qt_edits *src, int64_t src_from, int64_t src_len, int64_t dst_at)
{
    if (src_len <= 0)
        return true;
    /* src's time units are its movie's: convert the stretch to dst's */
    int64_t len = src_len * dst->scale / src->scale;
    for (int i = 0; i < dst->ntracks; i++) {
        qt_edit_track *t = &dst->tracks[i], out = *t;
        out.e = NULL;
        out.n = 0;
        int64_t d = qt_edits_track_duration(t);
        bool ok = extract(t, 0, dst_at < d ? dst_at : d, &out);
        if (ok && dst_at > d) /* the track ended before: empty up to the paste */
            ok = push(&out, (qt_edit){ dst_at - d, -1, 0x10000 });
        if (ok && i < src->ntracks) {
            qt_edit_track piece = src->tracks[i];
            qt_edit_track got = piece;
            got.e = NULL;
            got.n = 0;
            ok = extract(&piece, src_from, src_len, &got);
            int64_t have = 0;
            for (int k = 0; ok && k < got.n; k++) { /* into dst's time units */
                qt_edit e = got.e[k];
                e.len = e.len * dst->scale / src->scale;
                have += e.len;
                ok = push(&out, e);
            }
            if (ok && have < len)
                ok = push(&out, (qt_edit){ len - have, -1, 0x10000 });
            free(got.e);
        } else if (ok) {
            ok = push(&out, (qt_edit){ len, -1, 0x10000 });
        }
        if (ok && d > dst_at)
            ok = extract(t, dst_at, d, &out);
        if (!ok) {
            free(out.e);
            return false;
        }
        replace(t, &out);
    }
    return true;
}

bool qt_edits_copy(const qt_edits *src, int64_t from, int64_t len, qt_edits *out)
{
    memset(out, 0, sizeof *out);
    out->scale = src->scale;
    out->tracks = calloc((size_t)(src->ntracks ? src->ntracks : 1), sizeof *out->tracks);
    if (!out->tracks)
        return false;
    out->ntracks = src->ntracks;
    for (int i = 0; i < src->ntracks; i++) {
        out->tracks[i] = src->tracks[i];
        out->tracks[i].e = NULL;
        out->tracks[i].n = 0;
        if (!extract(&src->tracks[i], from, len, &out->tracks[i]))
            return false;
    }
    return true;
}

/* The 'moov' atom again, each track's edit list and durations replaced. */
uint8_t *qt_edits_write(const uint8_t *moov, uint32_t len, const qt_edits *e, uint32_t *out_len)
{
    size_t cap = len + 64;
    for (int i = 0; i < e->ntracks; i++)
        cap += 36 + (size_t)e->tracks[i].n * 12;
    uint8_t *out = malloc(cap);
    if (!out)
        return NULL;
    size_t o = 8;
    const uint8_t *end = moov + len;
    int ti = 0;
    for (const uint8_t *a = moov + 8; a + 8 <= end;) {
        uint32_t n = be32(a);
        if (n < 8 || n > (uint64_t)(end - a))
            break;
        if (!memcmp(a + 4, "trak", 4) && ti < e->ntracks) {
            const qt_edit_track *t = &e->tracks[ti++];
            size_t start = o;
            o += 8;
            memcpy(out + start + 4, "trak", 4);
            for (const uint8_t *c = a + 8; c + 8 <= a + n;) {
                uint32_t cn = be32(c);
                if (cn < 8 || cn > (uint64_t)(a + n - c))
                    break;
                if (memcmp(c + 4, "edts", 4)) {
                    memcpy(out + o, c, cn);
                    if (!memcmp(c + 4, "tkhd", 4) && cn >= 8 + 24 && c[8] == 0)
                        put32(out + o + 8 + 20, (uint32_t)qt_edits_track_duration(t));
                    o += cn;
                }
                if (!memcmp(c + 4, "tkhd", 4)) { /* the edit list after the header */
                    uint32_t en = 8 + 8 + 8 + (uint32_t)t->n * 12;
                    put32(out + o, en);
                    memcpy(out + o + 4, "edts", 4);
                    put32(out + o + 8, en - 8);
                    memcpy(out + o + 12, "elst", 4);
                    put32(out + o + 16, 0);
                    put32(out + o + 20, (uint32_t)t->n);
                    for (int k = 0; k < t->n; k++) {
                        uint8_t *p = out + o + 24 + 12 * (size_t)k;
                        put32(p, (uint32_t)t->e[k].len);
                        put32(p + 4, (uint32_t)(int32_t)t->e[k].media_time);
                        put32(p + 8, (uint32_t)t->e[k].rate);
                    }
                    o += en;
                }
                c += cn;
            }
            put32(out + start, (uint32_t)(o - start));
        } else {
            memcpy(out + o, a, n);
            if (!memcmp(a + 4, "mvhd", 4) && n >= 8 + 20 && a[8] == 0)
                put32(out + o + 8 + 16, (uint32_t)qt_edits_duration(e));
            o += n;
        }
        a += n;
    }
    put32(out, (uint32_t)o);
    memcpy(out + 4, "moov", 4);
    *out_len = (uint32_t)o;
    return out;
}
