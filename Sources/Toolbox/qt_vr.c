/*
 * QuickTime VR: panoramas and object movies, shown and navigated.
 *
 * A QTVR 2 movie has a QTVR track ('qtvr') whose sample description holds
 * the VR world (an atom container: the nodes and the default one) and
 * whose samples are its nodes' information (node header, hot spots and
 * their links), one sample per node, at the node's time.  A panorama node
 * has a 'pano' track sample at the same time (its 'pdat': angles, the
 * image's size and its tiles) whose track references name the image and
 * hot spot tracks: video tracks holding the panorama turned a quarter
 * turn, in tiles, and the same at 8 bits whose colour indices are hot
 * spot IDs.  An object node has an 'obje' track sample (its 'obji':
 * columns and rows of views, each lasting viewDuration, with animation
 * frames within) and an image track of its frames.  A QTVR 1.0 panorama
 * keeps the same pictures behind an 'STpn' track whose sample description
 * says where.
 *
 * The movie is drawn as its view: the panorama's tiles decoded once into
 * a picture and projected from the cylinder for the pan, tilt and field
 * of view; an object's frame for its column, row, view state and
 * animation time, zoomed about its view center.  Angles here are degrees;
 * qt_vr_lib.c converts to the program's units.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "qd_internal.h"
#include "qt_atoms.h"
#include "qt_codec.h"
#include "qt_comp.h"
#include "qt_dref.h"
#include "qt_errors.h"
#include "qt_model.h"
#include "qt_movie_internal.h"
#include "qt_vr_internal.h"
#include "tb_internal.h"

void qt_movie_place(const qtm_movie *mv, gaddr *port, qrect *box); /* quicktime.c */

#define MAX_VR 16
#define PI 3.14159265358979323846

static vr vrs[MAX_VR];

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static float bef(const uint8_t *p)
{
    uint32_t u = be32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static float fixed_f(const uint8_t *p) { return (float)((int32_t)be32(p) / 65536.0); }
static double rad(double d) { return d * PI / 180.0; }
static double deg(double r) { return r * 180.0 / PI; }

bool qt_vr_available(void) { return true; }

/* ---- Samples and frames ---- */

/* A sample's bytes (malloc'd), and its description's index. */
static uint8_t *read_sample(qtm_movie *mv, qtm_media *md, uint32_t n, uint32_t *len, uint32_t *desc)
{
    uint64_t off, bytes;
    uint32_t d;
    bool sync;
    int run;
    if (!qtm_sample_info(md, n, &off, &bytes, &d, &sync, &run) || bytes > 64u << 20)
        return NULL;
    int16_t di = qtm_desc_dref(md, d);
    if (di < 1 || di > md->ndrefs)
        di = 1;
    if (md->ndrefs < 1)
        return NULL;
    const qtm_dref *r = &md->drefs[di - 1];
    qt_src src;
    if (!qt_src_open(r->type, r->flags, r->data, r->len, qt_movie_path(mv), false, &src))
        return NULL;
    uint8_t *b = malloc(bytes ? bytes : 1);
    bool ok = b && qt_src_read(&src, off, b, (uint32_t)bytes);
    qt_src_close(&src);
    if (!ok) {
        free(b);
        return NULL;
    }
    *len = (uint32_t)bytes;
    if (desc)
        *desc = d;
    return b;
}

/* Movie time to a track's media sample (0: none). */
static uint32_t sample_at_movie_time(const qtm_movie *mv, const qtm_track *t, int64_t time)
{
    if (!t || !t->media)
        return 0;
    int32_t rate;
    int64_t mt = qtm_track_to_media(t, time, mv->scale, &rate);
    if (mt < 0)
        return 0;
    int64_t start, dur;
    return qtm_sample_at(t->media, mt, &start, &dur);
}

/* A media time in movie time, through the track's edits. */
static int64_t media_to_movie(const qtm_movie *mv, const qtm_track *t, int64_t mt)
{
    const qtm_media *md = t->media;
    int64_t at = 0;
    for (int i = 0; i < t->nedits; i++) {
        const qtm_edit *e = &t->edits[i];
        int64_t span = e->duration * md->scale / (mv->scale ? mv->scale : 1);
        if (e->media_time >= 0 && mt >= e->media_time && mt < e->media_time + (span ? span : 1))
            return at + (mt - e->media_time) * mv->scale / md->scale;
        at += e->duration;
    }
    return mt * mv->scale / (md->scale ? md->scale : 1);
}

/* Decoding a video track's samples in order, from a key frame. */
typedef struct {
    qtm_movie *mv;
    qtm_media *md;
    qt_vdec *dec;
    uint32_t desc;
    qt_video_format f;
    uint32_t pal[256];
    uint8_t *frame;
    uint32_t next;
    bool ids;     /* hot spots: colour index i as 0x0000ii */
} frames;

static bool video_format(frames *r, uint32_t d)
{
    if (d < 1 || (int)d > r->md->ndescs)
        return false;
    const uint8_t *p = r->md->descs[d - 1];
    uint32_t sz = r->md->desc_len[d - 1];
    if (sz < 86)
        return false;
    qt_video_format *f = &r->f;
    f->format = be32(p + 4);
    f->width = be16(p + 32);
    f->height = be16(p + 34);
    f->depth = (int16_t)be16(p + 82);
    f->palette = NULL;
    int16_t ctab = (int16_t)be16(p + 84);
    int bits = f->depth > 32 ? f->depth - 32 : f->depth;
    if (r->ids && bits <= 8) {
        for (int i = 0; i < 256; i++)
            r->pal[i] = (uint32_t)i;
        f->palette = r->pal;
    } else if (ctab == 0 && bits >= 1 && bits <= 8 && f->depth <= 8 && sz >= 94) {
        const uint8_t *c = p + 86;
        int count = (int)be16(c + 6) + 1;
        memset(r->pal, 0, sizeof r->pal);
        for (int k = 0; k < count && k < 256 && 94 + (uint32_t)k * 8 + 8 <= sz; k++) {
            const uint8_t *e = c + 8 + k * 8;
            int v = (int)(be16(c + 4) & 0x8000 ? (uint32_t)k : be16(e)) & 255;
            r->pal[v] = (uint32_t)e[2] << 16 | (uint32_t)e[4] << 8 | e[6];
        }
        f->palette = r->pal;
    }
    return f->width > 0 && f->height > 0 && qt_vdec_supported(f->format, f->depth);
}

static void frames_close(frames *r)
{
    if (r->dec)
        qt_vdec_free(r->dec);
    free(r->frame);
    memset(r, 0, sizeof *r);
}

/* The frame of sample n (RGBA, r->f.width by r->f.height), or NULL. */
static const uint8_t *frames_get(frames *r, uint32_t n)
{
    if (!n || n > qtm_sample_count(r->md))
        return NULL;
    if (!r->dec || n < r->next) { /* from the key frame at or before it */
        uint32_t k = n;
        while (k > 1) {
            uint64_t off, bytes;
            uint32_t d;
            bool sync;
            int run;
            if (qtm_sample_info(r->md, k, &off, &bytes, &d, &sync, &run) && sync)
                break;
            k--;
        }
        r->next = k;
        if (r->dec) {
            qt_vdec_free(r->dec);
            r->dec = NULL;
        }
    }
    for (; r->next <= n; r->next++) {
        uint32_t len, d;
        uint8_t *b = read_sample(r->mv, r->md, r->next, &len, &d);
        if (!b)
            return NULL;
        if (!r->dec || d != r->desc) {
            if (r->dec)
                qt_vdec_free(r->dec);
            r->dec = NULL;
            if (!video_format(r, d)) {
                free(b);
                return NULL;
            }
            r->desc = d;
            r->dec = qt_vdec_new(&r->f);
            free(r->frame);
            r->frame = calloc((size_t)r->f.width * r->f.height, 4);
            if (!r->dec || !r->frame) {
                free(b);
                return NULL;
            }
        }
        qt_vdec_decode(r->dec, b, len, r->frame);
        free(b);
    }
    return r->frame;
}

/* ---- Atoms ---- */

static const uint8_t *atom_leaf(const uint8_t *c, uint32_t len, uint32_t parent, uint32_t type, uint32_t id,
                                uint32_t *n)
{
    uint32_t a = id ? qta_find(c, len, parent, type, id) : qta_index(c, len, parent, type, 1, NULL);
    return a ? qta_data(c, len, a, n) : NULL;
}

static qtm_track *track_ref(const qtm_movie *mv, const qtm_track *t, uint32_t type, uint32_t index)
{
    for (int i = 0; t && i < t->ntrefs; i++)
        if (t->trefs[i].type == type && index >= 1 && (int)index <= t->trefs[i].n)
            return qtm_track_by_id(mv, t->trefs[i].ids[index - 1]);
    return NULL;
}

/* ---- Instances ---- */

vr *vr_for_token(gaddr token)
{
    for (int i = 0; i < MAX_VR; i++)
        if (vrs[i].used && vrs[i].token == token && token)
            return &vrs[i];
    return NULL;
}

/* A QTVR 1.0 object movie's description: its 'NAVG' user data (a copy
 * of 48 bytes), when its controller type ('ctyp') is 'stna'. */
static bool old_object_info(const qtm_movie *mv, uint8_t navg[48])
{
    uint32_t len;
    uint8_t *u = mv->udata ? qt_udata_flatten(mv->udata, &len) : NULL;
    bool stna = false, found = false;
    for (uint32_t o = 0; u && o + 8 <= len;) {
        uint32_t sz = be32(u + o);
        if (sz < 8 || sz > len - o)
            break;
        if (!memcmp(u + o + 4, "ctyp", 4) && sz >= 12 && !memcmp(u + o + 8, "stna", 4))
            stna = true;
        if (!memcmp(u + o + 4, "NAVG", 4) && sz >= 8 + 44) {
            memset(navg, 0, 48);
            memcpy(navg, u + o + 8, sz - 8 < 48 ? sz - 8 : 48);
            found = true;
        }
        o += sz;
    }
    free(u);
    return stna && found;
}

static qtm_track *first_video(const qtm_movie *mv)
{
    for (int i = 0; i < mv->ntracks; i++)
        if (mv->tracks[i]->media && mv->tracks[i]->media->type == fourcc("vide"))
            return mv->tracks[i];
    return NULL;
}

/* The movie's index'th QTVR track (1-based); a 1.0 panorama's 'STpn'
 * track, or a 1.0 object movie's video track. */
gaddr vr_find_track(gaddr movie, int32_t index)
{
    qtm_movie *mv = qt_movie_model(movie);
    if (!mv)
        return 0;
    int k = 0;
    for (int i = 0; i < mv->ntracks; i++) {
        qtm_track *t = mv->tracks[i];
        if (t->media && (t->media->type == fourcc("qtvr") || t->media->type == fourcc("STpn")) && ++k == index)
            return t->token;
    }
    uint8_t navg[48];
    qtm_track *t = !k && index == 1 && old_object_info(mv, navg) ? first_video(mv) : NULL;
    return t ? t->token : 0;
}

static void free_faces(vr *v)
{
    for (int i = 0; i < 6; i++) {
        free(v->faces[i]);
        free(v->hot_faces[i]);
        v->faces[i] = NULL;
        v->hot_faces[i] = NULL;
    }
    v->cube = false;
}

static void free_node_state(vr *v)
{
    free(v->image);
    free(v->hot);
    free(v->obj_frame);
    free_faces(v);
    v->image = NULL;
    v->hot = NULL;
    v->obj_frame = NULL;
    v->loaded = false;
}

static void free_vr(vr *v)
{
    free_node_state(v);
    for (int i = 0; i < v->nnodes; i++)
        free(v->nodes[i].info);
    free(v->nodes);
    free(v->world);
    free(v->view);
    free(v->areas);
    if (v->token)
        tb_DisposePtr(v->token);
    memset(v, 0, sizeof *v);
}

static bool load_nodes(vr *v)
{
    qtm_movie *mv;
    qtm_track *t = qt_track_model(v->track, &mv);
    if (!t || !t->media)
        return false;
    qtm_media *md = t->media;
    uint8_t navg[48];
    if (md->type == fourcc("vide") && old_object_info(mv, navg)) { /* QTVR 1.0: one object */
        v->old = v->old_object = true;
        v->nodes = calloc(1, sizeof *v->nodes);
        v->nodes[0] = (vr_node){ 1, fourcc("obje"), 0, mv->duration, NULL, 0 };
        v->nnodes = 1;
        v->default_node = 1;
        return true;
    }
    if (md->type == fourcc("STpn")) { /* QTVR 1.0: one panorama */
        v->old = true;
        v->nodes = calloc(1, sizeof *v->nodes);
        v->nodes[0] = (vr_node){ 1, fourcc("pano"), 0, mv->duration, NULL, 0 };
        v->nnodes = 1;
        v->default_node = 1;
        return true;
    }
    if (md->ndescs < 1 || md->desc_len[0] < 16 + QTA_HEADER + QTA_ATOM)
        return false;
    v->world_len = md->desc_len[0] - 16;
    v->world = malloc(v->world_len);
    memcpy(v->world, md->descs[0] + 16, v->world_len);
    uint32_t n;
    const uint8_t *hdr = atom_leaf(v->world, v->world_len, 0, fourcc("vrsc"), 0, &n);
    v->default_node = hdr && n >= 12 ? be32(hdr + 8) : 0;
    uint32_t count = qtm_sample_count(md);
    if (count > VR_MAX_NODES)
        count = VR_MAX_NODES;
    v->nodes = calloc(count ? count : 1, sizeof *v->nodes);
    for (uint32_t s = 1; s <= count; s++) {
        int64_t start, dur;
        if (!qtm_sample_time(md, s, &start, &dur))
            continue;
        uint32_t len;
        uint8_t *info = read_sample(mv, md, s, &len, NULL);
        if (!info)
            continue;
        vr_node *nd = &v->nodes[v->nnodes];
        nd->info = info;
        nd->info_len = len;
        nd->time = media_to_movie(mv, t, start);
        nd->duration = dur * mv->scale / (md->scale ? md->scale : 1);
        const uint8_t *h = atom_leaf(info, len, 0, fourcc("ndhd"), 0, &n);
        nd->type = h && n >= 12 ? be32(h + 4) : fourcc("pano");
        nd->id = h && n >= 12 ? be32(h + 8) : (uint32_t)s;
        v->nnodes++;
    }
    if (!v->default_node && v->nnodes)
        v->default_node = v->nodes[0].id;
    return v->nnodes > 0;
}

/* A VR instance for the track (one per movie). */
vr *vr_instance(gaddr track, gaddr mc)
{
    qtm_movie *mv;
    qtm_track *t = qt_track_model(track, &mv);
    uint8_t navg[48];
    if (!t || !t->media ||
        (t->media->type != fourcc("qtvr") && t->media->type != fourcc("STpn") &&
         !(t->media->type == fourcc("vide") && t == first_video(mv) && old_object_info(mv, navg))))
        return NULL;
    gaddr movie = qt_movie_handle(mv);
    for (int i = 0; i < MAX_VR; i++)
        if (vrs[i].used && vrs[i].movie == movie) {
            if (vrs[i].track != track) { /* another QTVR track of the movie */
                free_vr(&vrs[i]);
                break;
            }
            if (mc)
                vrs[i].mc = mc;
            return &vrs[i];
        }
    int i = 0;
    while (i < MAX_VR && vrs[i].used)
        i++;
    if (i == MAX_VR)
        return NULL;
    vr *v = &vrs[i];
    memset(v, 0, sizeof *v);
    v->used = true;
    v->movie = movie;
    v->track = track;
    v->mc = mc;
    v->cur = v->prev = -1;
    v->units = 1; /* radians */
    v->visible = true;
    v->over_tracking = v->down_tracking = true;
    v->mode = kQTVRStatic;
    v->interaction[1] = 2;   /* kQTVRInteractionMouseClickHysteresis */
    v->interaction[2] = 30;  /* kQTVRInteractionMouseClickTimeout */
    v->interaction[3] = 5;   /* pan and tilt speed */
    v->interaction[4] = 5;   /* zoom speed */
    v->mouse_motion_scale = 180;
    v->transition_on = false;
    v->bb_geometry = fourcc("vcyl");
    v->bb_resolution = 1;
    v->bb_format = 32;
    v->bb_cache = 1;
    for (int m = 1; m <= 2; m++) {
        v->imaging[m][1] = 2;                                /* kQTVRImagingCorrection: full */
        v->imaging[m][2] = m == kQTVRStatic ? 0x300 : 0x200; /* quality: codecHighQuality, codecNormalQuality */
    }
    v->token = tb_NewPtr(4, true, true);
    if (!v->token || !load_nodes(v)) {
        free_vr(v);
        return NULL;
    }
    vr_go_to_node(v, kQTVRDefaultNode);
    return v;
}

vr *vr_for_movie(gaddr movie, bool make)
{
    for (int i = 0; i < MAX_VR; i++)
        if (vrs[i].used && vrs[i].movie == movie)
            return &vrs[i];
    if (!make)
        return NULL;
    gaddr t = vr_find_track(movie, 1);
    return t ? vr_instance(t, 0) : NULL;
}

int vr_node_index(const vr *v, uint32_t id)
{
    for (int i = 0; i < v->nnodes; i++)
        if (v->nodes[i].id == id)
            return i;
    return -1;
}

/* ---- Loading a node ---- */

/* The tiles of a panorama's track (count of them, from the node's time)
 * into one picture: the stored image (sx by sy) of fx by fy tiles. */
static uint32_t *assemble(qtm_movie *mv, qtm_track *t, int64_t time, uint32_t sx, uint32_t sy, uint16_t fx, uint16_t fy,
                          bool ids)
{
    if (!t || !t->media || !sx || !sy || !fx || !fy || sx > 16384 || sy > 65536)
        return NULL;
    uint32_t first = sample_at_movie_time(mv, t, time);
    if (!first)
        first = 1;
    uint32_t *out = calloc((size_t)sx * sy, 4);
    if (!out)
        return NULL;
    frames r = { .mv = mv, .md = t->media, .ids = ids };
    uint32_t tw = sx / fx, th = sy / fy;
    for (uint32_t k = 0; k < (uint32_t)fx * fy; k++) {
        const uint8_t *f = frames_get(&r, first + k);
        if (!f)
            break;
        uint32_t ox = (k % fx) * tw, oy = (k / fx) * th;
        for (uint32_t y = 0; y < th && y < (uint32_t)r.f.height; y++)
            for (uint32_t x = 0; x < tw && x < (uint32_t)r.f.width; x++) {
                const uint8_t *p = f + ((size_t)y * r.f.width + x) * 4;
                out[(size_t)(oy + y) * sx + ox + x] = ids ? p[2] : (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
            }
    }
    frames_close(&r);
    return out;
}

/* The stored picture turned upright: stored as the panorama turned a
 * quarter turn anticlockwise (its top row is the stored picture's left
 * column), unless kept horizontal. */
static uint32_t *unrotate(uint32_t *s, uint32_t sx, uint32_t sy, bool horizontal, int *w, int *h)
{
    if (horizontal) {
        *w = (int)sx;
        *h = (int)sy;
        return s;
    }
    *w = (int)sy;
    *h = (int)sx;
    uint32_t *o = malloc((size_t)sx * sy * 4);
    if (!o) {
        free(s);
        return NULL;
    }
    for (uint32_t u = 0; u < sy; u++)     /* across the panorama */
        for (uint32_t v = 0; v < sx; v++) /* down it */
            o[(size_t)v * sy + u] = s[(size_t)(sy - 1 - u) * sx + v];
    free(s);
    return o;
}

static void set_limits(vr *v, float p0, float p1, float t0, float t1, float f0, float f1)
{
    v->lim_min[VR_PAN] = p0 < p1 ? p0 : p1;
    v->lim_max[VR_PAN] = p0 < p1 ? p1 : p0;
    v->lim_min[VR_TILT] = t0 < t1 ? t0 : t1;
    v->lim_max[VR_TILT] = t0 < t1 ? t1 : t0;
    v->lim_min[VR_FOV] = f0 < f1 ? f0 : f1;
    v->lim_max[VR_FOV] = f0 < f1 ? f1 : f0;
    for (int k = 0; k < 3; k++) {
        v->con_min[k] = v->lim_min[k];
        v->con_max[k] = v->lim_max[k];
    }
}

static void load_picture(vr *v, qtm_movie *mv, int64_t time)
{
    free(v->image);
    free(v->hot);
    v->image = NULL;
    v->hot = NULL;
    qtm_track *it = qt_track_model(v->image_track, NULL), *ht = qt_track_model(v->hot_track, NULL);
    uint32_t *s = assemble(mv, it, time, v->image_size[0], v->image_size[1], v->frames[0], v->frames[1], false);
    if (s)
        v->image = unrotate(s, v->image_size[0], v->image_size[1], v->horizontal, &v->iw, &v->ih);
    s = assemble(mv, ht, time, v->hot_size[0], v->hot_size[1], v->hot_frames[0], v->hot_frames[1], true);
    if (s) {
        uint32_t *h = unrotate(s, v->hot_size[0], v->hot_size[1], v->horizontal, &v->hw, &v->hh);
        if (h) {
            v->hot = malloc((size_t)v->hw * v->hh);
            for (size_t i = 0; i < (size_t)v->hw * v->hh; i++)
                v->hot[i] = (uint8_t)h[i];
            free(h);
        }
    }
}

/* A cubic panorama's six faces (QuickTime VR 5): the image track's six
 * frames from the node's time, each turned upright as a cylinder's tiles
 * are unless kept horizontal; its hot spots likewise. */
static bool load_faces(vr *v, qtm_movie *mv, int64_t time)
{
    qtm_track *it = qt_track_model(v->image_track, NULL), *ht = qt_track_model(v->hot_track, NULL);
    for (int pass = 0; pass < 2; pass++) {
        qtm_track *t = pass ? ht : it;
        uint32_t first = sample_at_movie_time(mv, t, time);
        if (!t || !first)
            continue;
        frames r = { .mv = mv, .md = t->media, .ids = pass == 1 };
        for (int f = 0; f < 6; f++) {
            const uint8_t *px = frames_get(&r, first + (uint32_t)f);
            if (!px)
                break;
            uint32_t sx = (uint32_t)r.f.width, sy = (uint32_t)r.f.height;
            uint32_t *s = malloc((size_t)sx * sy * 4);
            for (size_t i = 0; i < (size_t)sx * sy; i++)
                s[i] = pass ? px[i * 4 + 2] : (uint32_t)px[i * 4] << 16 | (uint32_t)px[i * 4 + 1] << 8 | px[i * 4 + 2];
            int w, h;
            uint32_t *u = unrotate(s, sx, sy, v->horizontal, &w, &h);
            if (!u)
                break;
            if (pass) {
                v->hot_faces[f] = malloc((size_t)w * h);
                for (size_t i = 0; i < (size_t)w * h; i++)
                    v->hot_faces[f][i] = (uint8_t)u[i];
                free(u);
                v->hw = w;
                v->hh = h;
            } else {
                v->faces[f] = u;
                v->fw = w;
                v->fh = h;
            }
        }
        frames_close(&r);
    }
    for (int f = 0; f < 6; f++)
        if (!v->faces[f])
            return false;
    return true;
}

static bool load_pano(vr *v, qtm_movie *mv, const vr_node *nd)
{
    qtm_track *qt = qt_track_model(v->track, NULL);
    qtm_track *pt = track_ref(mv, qt, fourcc("pano"), 1);
    uint32_t s = sample_at_movie_time(mv, pt, nd->time);
    uint32_t len, n;
    uint8_t *c = s ? read_sample(mv, pt->media, s, &len, NULL) : NULL;
    const uint8_t *d = c ? atom_leaf(c, len, 0, fourcc("pdat"), 0, &n) : NULL;
    if (!d || n < 80) {
        free(c);
        return false;
    }
    uint32_t iref = be32(d + 4), href = be32(d + 8);
    float min_pan = bef(d + 12), max_pan = bef(d + 16), min_tilt = bef(d + 20), max_tilt = bef(d + 24);
    float min_fov = bef(d + 28), max_fov = bef(d + 32);
    v->def_pan = bef(d + 36);
    v->def_tilt = bef(d + 40);
    v->def_fov = bef(d + 44);
    v->image_size[0] = be32(d + 48);
    v->image_size[1] = be32(d + 52);
    v->frames[0] = be16(d + 56);
    v->frames[1] = be16(d + 58);
    v->hot_size[0] = be32(d + 60);
    v->hot_size[1] = be32(d + 64);
    v->hot_frames[0] = be16(d + 68);
    v->hot_frames[1] = be16(d + 70);
    v->horizontal = (be32(d + 72) & 1) != 0;
    bool cube = n >= 80 && be32(d + 76) == fourcc("cube");
    float cv[9] = { -180, 180, -90, 90, 5, 120, 0, 0, 90 }; /* a cube's view limits and defaults ('cuvw') */
    const uint8_t *cuvw = cube ? atom_leaf(c, len, 0, fourcc("cuvw"), 0, &n) : NULL;
    for (int k = 0; cuvw && n >= 36 && k < 9; k++)
        cv[k] = bef(cuvw + 4 * k);
    free(c);
    qtm_track *it = track_ref(mv, pt, fourcc("imgt"), iref ? iref : 1);
    qtm_track *ht = href ? track_ref(mv, pt, fourcc("hott"), href) : NULL;
    v->image_track = it ? it->token : 0;
    v->hot_track = ht ? ht->token : 0;
    if (cube) {
        v->cube = true;
        if (!load_faces(v, mv, nd->time))
            return false;
        float f0 = cv[4] > 0 ? cv[4] : 5, f1 = cv[5] > f0 ? cv[5] : 120;
        set_limits(v, cv[0], cv[1], cv[2], cv[3], f0, f1);
        v->wraps = cv[1] - cv[0] >= 359.9f;
        v->def_pan = cv[6];
        v->def_tilt = cv[7];
        v->def_fov = cv[8] > 0 && cv[8] <= f1 ? cv[8] : f1;
        return true;
    }
    if (max_pan <= min_pan)
        max_pan = min_pan + 360;
    load_picture(v, mv, nd->time);
    if (!v->image)
        return false;
    /* The picture's own vertical extent bounds the tilt and field of view. */
    double r = v->iw / rad(max_pan - min_pan);
    float edge = (float)deg(atan(v->ih / 2.0 / r));
    if (max_tilt <= min_tilt || max_tilt > edge || min_tilt < -edge) {
        max_tilt = max_tilt <= min_tilt || max_tilt > edge ? edge : max_tilt;
        min_tilt = min_tilt >= max_tilt || min_tilt < -edge ? -edge : min_tilt;
    }
    if (max_fov <= 0 || max_fov > max_tilt - min_tilt)
        max_fov = max_tilt - min_tilt;
    if (min_fov <= 0 || min_fov >= max_fov)
        min_fov = max_fov / 8 > 5 ? 5 : max_fov / 8;
    set_limits(v, min_pan, max_pan, min_tilt, max_tilt, min_fov, max_fov);
    v->wraps = max_pan - min_pan >= 359.9f;
    if (v->def_fov <= 0 || v->def_fov > max_fov)
        v->def_fov = max_fov;
    return true;
}

/* QTVR 1.0: the 'STpn' track's description says where the picture is. */
static bool load_old_pano(vr *v, qtm_movie *mv)
{
    qtm_track *t = qt_track_model(v->track, NULL);
    qtm_media *md = t->media;
    if (md->ndescs < 1 || md->desc_len[0] < 152)
        return false;
    const uint8_t *d = md->descs[0];
    uint32_t scene = be32(d + 20), hot = be32(d + 52);
    float p0 = fixed_f(d + 92), p1 = fixed_f(d + 96), top = fixed_f(d + 100), bottom = fixed_f(d + 104);
    float min_zoom = fixed_f(d + 108), max_zoom = fixed_f(d + 112);
    v->image_size[0] = be32(d + 116);
    v->image_size[1] = be32(d + 120);
    v->frames[0] = be16(d + 130);
    v->frames[1] = be16(d + 132);
    v->hot_size[0] = be32(d + 136);
    v->hot_size[1] = be32(d + 140);
    v->hot_frames[0] = be16(d + 146);
    v->hot_frames[1] = be16(d + 148);
    if (!v->frames[0])
        v->frames[0] = 1;
    if (!v->frames[1])
        v->frames[1] = (uint16_t)be32(d + 124);
    v->horizontal = false;
    qtm_track *it = qtm_track_by_id(mv, scene), *ht = hot ? qtm_track_by_id(mv, hot) : NULL;
    v->image_track = it ? it->token : 0;
    v->hot_track = ht ? ht->token : 0;
    load_picture(v, mv, 0);
    if (!v->image)
        return false;
    if (p1 <= p0)
        p1 = p0 + 360;
    double r = v->iw / rad(p1 - p0);
    float edge = (float)deg(atan(v->ih / 2.0 / r));
    float t1 = top > 0 && top <= edge ? top : edge, t0 = bottom < 0 && bottom >= -edge ? bottom : -edge;
    float f1 = max_zoom > 0 && max_zoom <= t1 - t0 ? max_zoom : t1 - t0;
    float f0 = min_zoom > 0 && min_zoom < f1 ? min_zoom : 5;
    set_limits(v, p0, p1, t0, t1, f0, f1);
    v->wraps = p1 - p0 >= 359.9f;
    v->def_pan = p0;
    v->def_tilt = 0;
    v->def_fov = f1;
    uint32_t len;
    uint8_t *h = read_sample(mv, md, 1, &len, NULL);
    if (h && len >= 24 && !memcmp(h + 4, "pHdr", 4)) {
        v->def_pan = fixed_f(h + 12);
        v->def_tilt = fixed_f(h + 16);
        float z = fixed_f(h + 20);
        if (z > 0)
            v->def_fov = z < f0 ? f0 : z > f1 ? f1 : z;
    }
    free(h);
    return true;
}

static bool load_object(vr *v, qtm_movie *mv, const vr_node *nd)
{
    qtm_track *qt = qt_track_model(v->track, NULL);
    qtm_track *ot = track_ref(mv, qt, fourcc("obje"), 1);
    uint32_t s = sample_at_movie_time(mv, ot, nd->time);
    uint32_t len, n;
    uint8_t *c = s ? read_sample(mv, ot->media, s, &len, NULL) : NULL;
    const uint8_t *d = c ? atom_leaf(c, len, 0, fourcc("obji"), 0, &n) : NULL;
    if (!d || n < 88) {
        free(c);
        return false;
    }
    v->ui = be16(d + 4);
    v->view_states = be16(d + 6) ? be16(d + 6) : 1;
    v->def_state = be16(d + 8) ? be16(d + 8) : 1;
    v->down_state = be16(d + 10) ? be16(d + 10) : v->def_state;
    v->view_duration = be32(d + 12);
    v->columns = be32(d + 16) ? be32(d + 16) : 1;
    v->rows = be32(d + 20) ? be32(d + 20) : 1;
    v->motion_scale = bef(d + 24);
    float p0 = bef(d + 28), p1 = bef(d + 32), t0 = bef(d + 40), t1 = bef(d + 44);
    v->def_pan = bef(d + 36);
    v->def_tilt = bef(d + 48);
    float f0 = bef(d + 52), f1 = bef(d + 56);
    v->def_fov = bef(d + 60);
    v->def_vch = bef(d + 64);
    v->def_vcv = bef(d + 68);
    v->view_rate = bef(d + 72);
    v->frame_rate = bef(d + 76);
    v->anim = be32(d + 80);
    v->control = be32(d + 84);
    /* where the image track is: an 'imtr' atom, else the first 'imgt' reference */
    uint32_t iref = 1, hn;
    const uint8_t *imtr = atom_leaf(c, len, 0, fourcc("imtr"), 0, &hn);
    if (imtr && hn >= 4)
        iref = be32(imtr);
    qtm_track *it = track_ref(mv, ot, fourcc("imgt"), iref);
    if (!it) /* the 2.0 format's image track: the movie's first video track */
        for (int i = 0; i < mv->ntracks && !it; i++)
            if (mv->tracks[i]->media && mv->tracks[i]->media->type == fourcc("vide"))
                it = mv->tracks[i];
    uint32_t href = 1;
    const uint8_t *hstr = atom_leaf(c, len, 0, fourcc("hstr"), 0, &hn);
    if (hstr && hn >= 4)
        href = be32(hstr);
    qtm_track *ht = track_ref(mv, ot, fourcc("hott"), href);
    free(c);
    v->image_track = it ? it->token : 0;
    v->hot_track = ht ? ht->token : 0;
    if (!it)
        return false;
    if (v->motion_scale <= 0)
        v->motion_scale = 180;
    if (v->frame_rate <= 0)
        v->frame_rate = 1;
    if (v->view_rate <= 0)
        v->view_rate = 1;
    if (!v->view_duration)
        v->view_duration = (uint32_t)(nd->duration / ((int64_t)v->columns * v->rows * v->view_states));
    if (f1 < 1)
        f1 = 180;
    if (f0 < 1 || f0 > f1)
        f0 = f1 / 4 > 1 ? f1 / 4 : 1;
    set_limits(v, p0, p1, t0, t1, f0, f1);
    /* the columns and rows run from the start angles (min) to the end ones */
    v->lim_min[VR_PAN] = p0;
    v->lim_max[VR_PAN] = p1;
    v->lim_min[VR_TILT] = t0;
    v->lim_max[VR_TILT] = t1;
    v->con_min[VR_PAN] = p0 < p1 ? p0 : p1;
    v->con_max[VR_PAN] = p0 < p1 ? p1 : p0;
    v->con_min[VR_TILT] = t0 < t1 ? t0 : t1;
    v->con_max[VR_TILT] = t0 < t1 ? t1 : t0;
    v->wraps = (v->control & 1) != 0; /* kQTVRObjectWrapPanOn */
    if (v->def_fov <= 0 || v->def_fov > f1)
        v->def_fov = f1;
    v->cur_state = v->def_state;
    v->frame_anim = (v->anim & 1) != 0; /* kQTVRObjectAnimateViewFramesOn */
    v->view_anim = (v->anim & 8) != 0;  /* kQTVRObjectAnimateViewsOn */
    v->view_time = 0;
    v->obj_frame_time = -1;
    /* an animation frame's duration: the image track's first sample's */
    v->frame_dur = v->view_duration;
    uint32_t fs = sample_at_movie_time(mv, it, nd->time);
    int64_t st, sd;
    if (fs && qtm_sample_time(it->media, fs, &st, &sd) && sd > 0)
        v->frame_dur = sd * mv->scale / (it->media->scale ? it->media->scale : 1);
    if (v->frame_dur <= 0 || v->frame_dur > v->view_duration)
        v->frame_dur = v->view_duration ? v->view_duration : 1;
    return true;
}

/* QTVR 1.0 objects: 'NAVG' says columns, rows, frames per view and their
 * duration, and the pan and tilt each runs through. */
static bool load_old_object(vr *v, qtm_movie *mv)
{
    uint8_t d[48];
    qtm_track *it = first_video(mv);
    if (!old_object_info(mv, d) || !it)
        return false;
    v->columns = be16(d + 2) ? be16(d + 2) : 1;
    v->rows = be16(d + 4) ? be16(d + 4) : 1;
    uint32_t loop = be16(d + 8) ? be16(d + 8) : 1, frame = be16(d + 10) ? be16(d + 10) : 1;
    v->ui = be16(d + 12);
    v->view_duration = loop * frame;
    v->frame_dur = frame;
    float f1 = fixed_f(d + 16), p0 = fixed_f(d + 20), p1 = fixed_f(d + 24), t1 = fixed_f(d + 28);
    float t0 = fixed_f(d + 32);
    v->def_pan = fixed_f(d + 36);
    v->def_tilt = fixed_f(d + 40);
    if (f1 < 1)
        f1 = 180;
    v->view_states = v->def_state = v->down_state = v->cur_state = 1;
    v->motion_scale = 180;
    v->frame_rate = v->view_rate = 1;
    v->anim = loop > 1 ? 1 : 0;
    v->control = fabsf(fabsf(p1 - p0) - 360) < 0.01f ? 1 : 0;
    set_limits(v, p0, p1, t0, t1, f1, f1);
    v->lim_min[VR_PAN] = p0;
    v->lim_max[VR_PAN] = p1;
    v->lim_min[VR_TILT] = t0;
    v->lim_max[VR_TILT] = t1;
    v->wraps = v->control & 1;
    v->def_fov = f1;
    v->def_vch = v->def_vcv = 0;
    v->image_track = it->token;
    v->hot_track = 0;
    v->frame_anim = loop > 1;
    v->view_anim = false;
    v->view_time = 0;
    v->obj_frame_time = -1;
    return true;
}

static void enter_node_time(vr *v, const vr_node *nd)
{
    tb_SetMovieTimeValue((int32_t)v->movie, (int32_t)nd->time);
}

/* Show the node: id is a node's, or kQTVRCurrentNode, kQTVRPreviousNode or
 * kQTVRDefaultNode. */
int16_t vr_go_to_node(vr *v, int32_t id)
{
    int idx;
    if (id == kQTVRCurrentNode)
        idx = v->cur >= 0 ? v->cur : vr_node_index(v, v->default_node);
    else if (id == kQTVRPreviousNode)
        idx = v->prev >= 0 ? v->prev : v->cur;
    else if (id == kQTVRDefaultNode)
        idx = vr_node_index(v, v->default_node);
    else
        idx = vr_node_index(v, (uint32_t)id);
    if (idx < 0 && id == kQTVRDefaultNode && v->nnodes)
        idx = 0;
    if (idx < 0)
        return invalidNodeIDErr;
    if (idx == v->cur && v->loaded)
        return noErr;
    if (v->cur >= 0 && vr_leave_proc(v, v->nodes[v->cur].id, v->nodes[idx].id))
        return noErr;
    qtm_movie *mv = qt_movie_model(v->movie);
    if (!mv)
        return invalidMovie;
    free_node_state(v);
    if (v->cur != idx)
        v->prev = v->cur;
    v->cur = idx;
    vr_node *nd = &v->nodes[idx];
    v->object = nd->type == fourcc("obje");
    bool ok = v->old_object ? load_old_object(v, mv)
              : v->old       ? load_old_pano(v, mv)
              : v->object    ? load_object(v, mv, nd)
                             : load_pano(v, mv, nd);
    v->loaded = ok;
    v->over_id = v->down_id = 0;
    vr_show_default(v);
    enter_node_time(v, nd);
    if (v->backbuf.proc)
        vr_refresh_back_buffer(v);
    vr_enter_proc(v, nd->id);
    vr_update(v, false);
    return ok ? noErr : invalidNodeFormatErr;
}

/* ---- Angles and constraints ---- */

static float view_aspect(const vr *v)
{
    int w = v->box.right - v->box.left, h = v->box.bottom - v->box.top;
    return w > 0 && h > 0 ? (float)w / h : 4.0f / 3;
}

/* The range a value may take now: a panorama's view stays within its
 * picture (half the field of view in from the edges). */
void vr_effective_limits(const vr *v, int kind, float *lo, float *hi)
{
    if (kind == VR_VCH || kind == VR_VCV) {
        int full = kind == VR_VCH ? v->ow : v->oh;
        *lo = 0;
        *hi = (float)(full > 0 ? full : 0);
        return;
    }
    *lo = v->con_min[kind];
    *hi = v->con_max[kind];
    if (v->object || !v->loaded)
        return;
    if (kind == VR_TILT && v->cube) {
        /* a cube's view may look straight up and down */
    } else if (kind == VR_TILT) {
        *lo += v->fov / 2;
        *hi -= v->fov / 2;
    } else if (kind == VR_PAN && !v->wraps) {
        float half = (float)deg(atan(tan(rad(v->fov / 2)) * view_aspect(v)));
        *lo += half;
        *hi -= half;
    } else if (kind == VR_FOV && !v->cube && v->con_max[VR_TILT] - v->con_min[VR_TILT] < *hi) {
        *hi = v->con_max[VR_TILT] - v->con_min[VR_TILT];
    }
    if (*lo > *hi)
        *lo = *hi = (*lo + *hi) / 2;
}

static bool pan_wraps(const vr *v) { return v->wraps; }

float vr_wrap_constrain(const vr *v, int kind, float value)
{
    float lo, hi;
    if (kind == VR_PAN && pan_wraps(v)) {
        float base = v->object ? (v->con_min[VR_PAN]) : v->lim_min[VR_PAN];
        float span = v->object ? v->con_max[VR_PAN] - v->con_min[VR_PAN] : 360;
        if (span <= 0)
            span = 360;
        float r = fmodf(value - base, span);
        if (r < 0)
            r += span;
        return base + r;
    }
    if (kind == VR_TILT && v->object && (v->control & 2)) { /* kQTVRObjectWrapTiltOn */
        float span = v->con_max[VR_TILT] - v->con_min[VR_TILT];
        if (span > 0) {
            float r = fmodf(value - v->con_min[VR_TILT], span);
            return v->con_min[VR_TILT] + (r < 0 ? r + span : r);
        }
    }
    vr_effective_limits(v, kind, &lo, &hi);
    return value < lo ? lo : value > hi ? hi : value;
}

bool vr_set_angle(vr *v, int kind, float d)
{
    float c = vr_wrap_constrain(v, kind, d);
    float *slot = kind == VR_PAN ? &v->pan : kind == VR_TILT ? &v->tilt : &v->fov;
    if (*slot != c) {
        *slot = c;
        if (kind == VR_FOV) { /* the others' ranges follow the field of view */
            v->tilt = vr_wrap_constrain(v, VR_TILT, v->tilt);
            v->pan = vr_wrap_constrain(v, VR_PAN, v->pan);
        }
        vr_update(v, false);
    }
    return fabsf(c - d) < 1e-4f || (kind == VR_PAN && pan_wraps(v));
}

void vr_set_center(vr *v, float h, float vv)
{
    v->vch = h;
    v->vcv = vv;
    vr_update(v, false);
}

uint32_t vr_constraint_status(const vr *v)
{
    uint32_t s = 0;
    float lo, hi;
    const float e = 1e-3f;
    if (!pan_wraps(v)) {
        vr_effective_limits(v, VR_PAN, &lo, &hi);
        if (v->object) {
            if (v->pan <= lo + e)
                s |= 2; /* can't pan right (objects turn the other way) */
            if (v->pan >= hi - e)
                s |= 1;
        } else {
            if (v->pan >= hi - e)
                s |= 1; /* kQTVRCantPanLeft: pan grows to the left */
            if (v->pan <= lo + e)
                s |= 2;
        }
    }
    vr_effective_limits(v, VR_TILT, &lo, &hi);
    if (v->tilt >= hi - e)
        s |= 4;
    if (v->tilt <= lo + e)
        s |= 8;
    vr_effective_limits(v, VR_FOV, &lo, &hi);
    if (v->fov <= lo + e)
        s |= 16;
    if (v->fov >= hi - e)
        s |= 32;
    return s;
}

void vr_show_default(vr *v)
{
    v->fov = v->def_fov;
    v->fov = vr_wrap_constrain(v, VR_FOV, v->fov);
    v->pan = vr_wrap_constrain(v, VR_PAN, v->def_pan);
    v->tilt = vr_wrap_constrain(v, VR_TILT, v->def_tilt);
    v->vch = v->def_vch;
    v->vcv = v->def_vcv;
    v->view_time = 0;
    v->cur_state = v->def_state ? v->def_state : 1;
    vr_update(v, false);
}

/* ---- Objects: columns, rows and frames ---- */

static float pan_step(const vr *v)
{
    float span = v->lim_max[VR_PAN] - v->lim_min[VR_PAN];
    uint32_t n = v->wraps || fabsf(fabsf(span) - 360) < 0.01f ? v->columns : v->columns - 1;
    return n ? span / (float)n : 0;
}

static float tilt_step(const vr *v)
{
    return v->rows > 1 ? (v->lim_max[VR_TILT] - v->lim_min[VR_TILT]) / (float)(v->rows - 1) : 0;
}

int vr_pan_column(const vr *v, float pan)
{
    if (v->object) {
        float st = pan_step(v);
        int c = st != 0 ? (int)lroundf((pan - v->lim_min[VR_PAN]) / st) : 0;
        if (v->wraps && v->columns)
            c = ((c % (int)v->columns) + (int)v->columns) % (int)v->columns;
        return c < 0 ? 0 : c >= (int)v->columns ? (int)v->columns - 1 : c;
    }
    /* a panorama: its picture's column */
    float span = v->lim_max[VR_PAN] - v->lim_min[VR_PAN];
    return span > 0 ? (int)((v->lim_max[VR_PAN] - pan) / span * v->iw) : 0;
}

int vr_tilt_row(const vr *v, float tilt)
{
    if (v->object) {
        float st = tilt_step(v);
        int r = st != 0 ? (int)lroundf((tilt - v->lim_min[VR_TILT]) / st) : 0;
        return r < 0 ? 0 : r >= (int)v->rows ? (int)v->rows - 1 : r;
    }
    float x, y;
    vr_coord_from_angles(v, v->pan, tilt, &x, &y);
    return (int)y;
}

float vr_column_pan(const vr *v, int col)
{
    if (v->object)
        return v->lim_min[VR_PAN] + col * pan_step(v);
    float span = v->lim_max[VR_PAN] - v->lim_min[VR_PAN];
    return v->iw ? v->lim_max[VR_PAN] - (float)col * span / v->iw : 0;
}

float vr_row_tilt(const vr *v, int row)
{
    if (v->object)
        return v->lim_min[VR_TILT] + row * tilt_step(v);
    float pan, tilt;
    vr_angles_from_coord(v, 0, (float)row, &pan, &tilt);
    return tilt;
}

void vr_object_position(const vr *v, int *col, int *row)
{
    *col = vr_pan_column(v, v->pan);
    *row = vr_tilt_row(v, v->tilt);
}

/* ---- Panorama coordinates ---- */

static double cyl_radius(const vr *v)
{
    double span = rad(v->lim_max[VR_PAN] - v->lim_min[VR_PAN]);
    return span > 0 ? v->iw / span : 1;
}

void vr_coord_from_angles(const vr *v, float pan, float tilt, float *x, float *y)
{
    if (v->object) {
        *x = (float)vr_pan_column(v, pan);
        *y = (float)vr_tilt_row(v, tilt);
        return;
    }
    float span = v->lim_max[VR_PAN] - v->lim_min[VR_PAN];
    float u = span > 0 ? (v->lim_max[VR_PAN] - pan) / span * v->iw : 0;
    if (v->wraps && v->iw) {
        u = fmodf(u, (float)v->iw);
        if (u < 0)
            u += v->iw;
    }
    *x = u;
    *y = (float)(v->ih / 2.0 - cyl_radius(v) * tan(rad(tilt)));
}

void vr_angles_from_coord(const vr *v, float x, float y, float *pan, float *tilt)
{
    if (v->object) {
        *pan = vr_column_pan(v, (int)x);
        *tilt = vr_row_tilt(v, (int)y);
        return;
    }
    float span = v->lim_max[VR_PAN] - v->lim_min[VR_PAN];
    *pan = v->iw ? v->lim_max[VR_PAN] - x / v->iw * span : 0;
    if (v->wraps)
        *pan = vr_wrap_constrain(v, VR_PAN, *pan);
    *tilt = (float)deg(atan((v->ih / 2.0 - y) / cyl_radius(v)));
}

/* The direction through a view pixel (dx right of centre, dy above it):
 * its pan and the tangent of its elevation. */
static void view_ray(const vr *v, double f, double ct, double st, double dx, double dy, double *pan, double *el)
{
    double yy = dy * ct + f * st, zz = f * ct - dy * st;
    *pan = v->pan - deg(atan2(dx, zz));
    *el = yy / sqrt(dx * dx + zz * zz);
}

static double focal(const vr *v, int h) { return h / 2.0 / tan(rad(v->fov / 2)); }

bool vr_pt_to_angles(const vr *v, int h, int vv, float *pan, float *tilt)
{
    int w = v->box.right - v->box.left, ht = v->box.bottom - v->box.top;
    if (w <= 0 || ht <= 0)
        return false;
    double dx = h - v->box.left - w / 2.0 + 0.5, dy = ht / 2.0 - (vv - v->box.top) - 0.5;
    if (v->object) {
        *pan = v->pan;
        *tilt = v->tilt;
        return true;
    }
    double f = focal(v, ht), p, el;
    view_ray(v, f, cos(rad(v->tilt)), sin(rad(v->tilt)), dx, dy, &p, &el);
    *pan = vr_wrap_constrain(v, VR_PAN, (float)p);
    if (!v->wraps)
        *pan = (float)p;
    *tilt = (float)deg(atan(el));
    return true;
}

/* A panorama's picture coordinates for a view pixel (false outside it). */
static bool pano_uv(const vr *v, double f, double ct, double st, double dx, double dy, double *u, double *y)
{
    double p, el;
    view_ray(v, f, ct, st, dx, dy, &p, &el);
    double span = v->lim_max[VR_PAN] - v->lim_min[VR_PAN];
    *u = (v->lim_max[VR_PAN] - p) / span * v->iw;
    if (v->wraps) {
        *u = fmod(*u, v->iw);
        if (*u < 0)
            *u += v->iw;
    }
    *y = v->ih / 2.0 - cyl_radius(v) * el;
    return *u >= 0 && *u < v->iw && *y >= 0 && *y < v->ih;
}

/* ---- Objects' frames ---- */

static int64_t object_frame_time(const vr *v)
{
    int col, row;
    vr_object_position(v, &col, &row);
    const vr_node *nd = &v->nodes[v->cur];
    int64_t state = (v->cur_state ? v->cur_state : 1) - 1;
    int64_t view = state * v->rows * v->columns + (int64_t)row * v->columns + col;
    return nd->time + view * v->view_duration + v->view_time;
}

/* An object's frame now (RGBA as packed 0x00RRGGBB), decoded if it changed. */
static bool object_frame(vr *v)
{
    int64_t t = object_frame_time(v);
    if (v->obj_frame && t == v->obj_frame_time)
        return true;
    qtm_movie *mv;
    qtm_track *it = qt_track_model(v->image_track, &mv);
    uint32_t s = sample_at_movie_time(mv, it, t);
    if (!s)
        return v->obj_frame != NULL;
    frames r = { .mv = mv, .md = it->media };
    const uint8_t *f = frames_get(&r, s);
    if (f) {
        free(v->obj_frame);
        v->ow = r.f.width;
        v->oh = r.f.height;
        v->obj_frame = malloc((size_t)v->ow * v->oh * 4);
        for (size_t i = 0; i < (size_t)v->ow * v->oh; i++)
            v->obj_frame[i] = (uint32_t)f[i * 4] << 16 | (uint32_t)f[i * 4 + 1] << 8 | f[i * 4 + 2];
        v->obj_frame_time = t;
        if (v->vch == 0 && v->vcv == 0) { /* the frame's centre */
            v->vch = v->def_vch = v->ow / 2.0f;
            v->vcv = v->def_vcv = v->oh / 2.0f;
        }
    }
    frames_close(&r);
    return v->obj_frame != NULL;
}

/* The object's frame pixel shown at a view pixel. */
static bool object_xy(const vr *v, int x, int y, int w, int h, int *ox, int *oy)
{
    float z = v->lim_max[VR_FOV] > 0 ? v->fov / v->lim_max[VR_FOV] : 1;
    double sw = v->ow * z, sh = v->oh * z;
    double fx = v->vch - sw / 2 + (x + 0.5) * sw / w, fy = v->vcv - sh / 2 + (y + 0.5) * sh / h;
    *ox = (int)floor(fx);
    *oy = (int)floor(fy);
    return *ox >= 0 && *ox < v->ow && *oy >= 0 && *oy < v->oh;
}

/* An object's hot spot ID under one of its frame's pixels. */
static uint8_t object_hot(vr *v, int ox, int oy)
{
    qtm_movie *mv;
    qtm_track *ht = qt_track_model(v->hot_track, &mv);
    if (!ht)
        return 0;
    uint32_t s = sample_at_movie_time(mv, ht, object_frame_time(v));
    if (!s)
        return 0;
    frames r = { .mv = mv, .md = ht->media, .ids = true };
    const uint8_t *f = frames_get(&r, s);
    uint8_t id = 0;
    if (f && r.f.width > 0 && v->ow > 0) {
        int hx = ox * r.f.width / v->ow, hy = oy * r.f.height / v->oh;
        if (hx >= 0 && hx < r.f.width && hy >= 0 && hy < r.f.height)
            id = f[((size_t)hy * r.f.width + hx) * 4 + 2];
    }
    frames_close(&r);
    return id;
}

/* ---- Drawing ---- */

static void render_cube(vr *v, int w, int h);

static void render(vr *v, int w, int h)
{
    if (w != v->vw || h != v->vh || !v->view) {
        free(v->view);
        v->view = calloc((size_t)w * h, 4);
        v->vw = w;
        v->vh = h;
    }
    if (!v->view)
        return;
    if (v->object) {
        if (!object_frame(v))
            return;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int ox, oy;
                v->view[(size_t)y * w + x] = object_xy(v, x, y, w, h, &ox, &oy) ? v->obj_frame[(size_t)oy * v->ow + ox] : 0;
            }
        return;
    }
    if (v->cube) {
        render_cube(v, w, h);
        return;
    }
    if (!v->image) {
        memset(v->view, 0, (size_t)w * h * 4);
        return;
    }
    double f = focal(v, h), ct = cos(rad(v->tilt)), st = sin(rad(v->tilt));
    for (int y = 0; y < h; y++) {
        double dy = h / 2.0 - y - 0.5;
        for (int x = 0; x < w; x++) {
            double u, yy;
            uint32_t px = 0;
            if (pano_uv(v, f, ct, st, x - w / 2.0 + 0.5, dy, &u, &yy)) {
                /* bilinear, wrapping across the seam of a whole circle */
                double fu = u - 0.5, fv = yy - 0.5;
                int u0 = (int)floor(fu), v0 = (int)floor(fv);
                double au = fu - u0, av = fv - v0;
                int u1 = u0 + 1, v1 = v0 + 1;
                if (v->wraps) {
                    u0 = (u0 + v->iw) % v->iw;
                    u1 = u1 % v->iw;
                } else {
                    u0 = u0 < 0 ? 0 : u0;
                    u1 = u1 >= v->iw ? v->iw - 1 : u1;
                }
                v0 = v0 < 0 ? 0 : v0;
                v1 = v1 >= v->ih ? v->ih - 1 : v1;
                uint32_t a = v->image[(size_t)v0 * v->iw + u0], b = v->image[(size_t)v0 * v->iw + u1];
                uint32_t c = v->image[(size_t)v1 * v->iw + u0], d = v->image[(size_t)v1 * v->iw + u1];
                for (int sh = 0; sh < 24; sh += 8) {
                    double top = ((a >> sh) & 255) * (1 - au) + ((b >> sh) & 255) * au;
                    double bot = ((c >> sh) & 255) * (1 - au) + ((d >> sh) & 255) * au;
                    px |= (uint32_t)(top * (1 - av) + bot * av + 0.5) << sh;
                }
            }
            v->view[(size_t)y * w + x] = px;
        }
    }
}

/* A cube's face and the face's pixel for a view pixel: the ray turned by
 * the tilt and pan, then the face it meets most squarely. */
static const int8_t face_axes[6][3][3] = {
    /* forward, right, up for front, right, back, left, top, bottom */
    { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } },   { { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } },
    { { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 } }, { { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
    { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } },  { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
};

static int cube_face(const vr *v, double f, double ct, double st, double dx, double dy, double *u, double *y)
{
    double cx = dx, cy = dy * ct + f * st, cz = f * ct - dy * st; /* tilted */
    double p = rad(v->pan), sp = sin(p), cp = cos(p);
    double d[3] = { cx * cp - cz * sp, cy, cx * sp + cz * cp }; /* panned (to the left) */
    int best = 0;
    double bd = -1e30;
    for (int i = 0; i < 6; i++) {
        double k = d[0] * face_axes[i][0][0] + d[1] * face_axes[i][0][1] + d[2] * face_axes[i][0][2];
        if (k > bd) {
            bd = k;
            best = i;
        }
    }
    const int8_t (*a)[3] = face_axes[best];
    double r = d[0] * a[1][0] + d[1] * a[1][1] + d[2] * a[1][2], up = d[0] * a[2][0] + d[1] * a[2][1] + d[2] * a[2][2];
    *u = (0.5 + 0.5 * r / bd) * v->fw;
    *y = (0.5 - 0.5 * up / bd) * v->fh;
    return best;
}

static void render_cube(vr *v, int w, int h)
{
    double f = focal(v, h), ct = cos(rad(v->tilt)), st = sin(rad(v->tilt));
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            double u, yy;
            int k = cube_face(v, f, ct, st, x - w / 2.0 + 0.5, h / 2.0 - y - 0.5, &u, &yy);
            int iu = (int)u, iv = (int)yy;
            iu = iu < 0 ? 0 : iu >= v->fw ? v->fw - 1 : iu;
            iv = iv < 0 ? 0 : iv >= v->fh ? v->fh - 1 : iv;
            v->view[(size_t)y * w + x] = v->faces[k] ? v->faces[k][(size_t)iv * v->fw + iu] : 0;
        }
}

/* The hot spot under a view pixel (0 none), whether enabled or not. */
static uint32_t raw_hot(vr *v, int x, int y, int w, int h)
{
    if (v->object) {
        int ox, oy;
        return v->obj_frame && object_xy(v, x, y, w, h, &ox, &oy) ? object_hot(v, ox, oy) : 0;
    }
    if (v->cube) {
        double u, yy;
        int k = cube_face(v, focal(v, h), cos(rad(v->tilt)), sin(rad(v->tilt)), x - w / 2.0 + 0.5, h / 2.0 - y - 0.5,
                          &u, &yy);
        if (!v->hot_faces[k] || !v->fw)
            return 0;
        int hx = (int)(u * v->hw / v->fw), hy = (int)(yy * v->hh / v->fh);
        return hx >= 0 && hx < v->hw && hy >= 0 && hy < v->hh ? v->hot_faces[k][(size_t)hy * v->hw + hx] : 0;
    }
    if (!v->hot)
        return 0;
    double u, yy;
    if (!pano_uv(v, focal(v, h), cos(rad(v->tilt)), sin(rad(v->tilt)), x - w / 2.0 + 0.5, h / 2.0 - y - 0.5, &u, &yy))
        return 0;
    int hx = (int)(u * v->hw / v->iw), hy = (int)(yy * v->hh / v->ih);
    if (hx < 0 || hx >= v->hw || hy < 0 || hy >= v->hh)
        return 0;
    return v->hot[(size_t)hy * v->hw + hx];
}

/* Hot spots outlined when shown (kQTVRHotSpotsVisible): their pixels
 * tinted. */
static void tint_hots(vr *v, int w, int h)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t id = raw_hot(v, x, y, w, h);
            if (id && vr_hot_enabled(v, id)) {
                uint32_t p = v->view[(size_t)y * w + x];
                uint32_t r = ((p >> 16) & 255) / 2 + 64, g = ((p >> 8) & 255) / 2 + 64, b = (p & 255) / 2 + 128;
                v->view[(size_t)y * w + x] = r << 16 | g << 8 | (b > 255 ? 255 : b);
            }
        }
}

static void draw(vr *v)
{
    qtm_movie *mv = qt_movie_model(v->movie);
    if (!mv || !v->visible)
        return;
    gaddr port;
    qrect box;
    qt_movie_place(mv, &port, &box);
    if (port) {
        v->port = port;
        v->box = box;
    }
    int w = v->box.right - v->box.left, h = v->box.bottom - v->box.top;
    v->dirty = false;
    if (!v->port || w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return;
    render(v, w, h);
    if (!v->view)
        return;
    if (v->hots_visible)
        tint_hots(v, w, h);
    uint8_t *rgba = malloc((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint32_t p = v->view[i];
        rgba[i * 4] = (uint8_t)(p >> 16);
        rgba[i * 4 + 1] = (uint8_t)(p >> 8);
        rgba[i * 4 + 2] = (uint8_t)p;
        rgba[i * 4 + 3] = 255;
    }
    gaddr saved = qd_port();
    tb_SetPort(v->port);
    qd_draw_rgba(rgba, w, h, v->box);
    tb_SetPort(saved);
    free(rgba);
    vr_prescreen(v);
}

void vr_update(vr *v, bool now)
{
    v->dirty = true;
    if (now)
        draw(v);
}

/* ---- Hot spots ---- */

static uint32_t hots_parent(const vr *v, const uint8_t *info, uint32_t len)
{
    (void)v;
    return info ? qta_find(info, len, 0, fourcc("hspa"), 1) : 0;
}

static uint32_t hot_atom(const vr *v, uint32_t id, const uint8_t **info, uint32_t *len)
{
    if (v->cur < 0)
        return 0;
    *info = v->nodes[v->cur].info;
    *len = v->nodes[v->cur].info_len;
    uint32_t p = hots_parent(v, *info, *len);
    return p ? qta_find(*info, *len, p, fourcc("hots"), id) : 0;
}

bool vr_hot_known(const vr *v, uint32_t id)
{
    const uint8_t *info;
    uint32_t len;
    return id && (hot_atom(v, id, &info, &len) || v->old || v->hot || v->hot_track);
}

uint32_t vr_hot_type(const vr *v, uint32_t id)
{
    const uint8_t *info;
    uint32_t len, n;
    uint32_t a = hot_atom(v, id, &info, &len);
    const uint8_t *d = a ? atom_leaf(info, len, a, fourcc("hsin"), 0, &n) : NULL;
    return d && n >= 8 ? be32(d + 4) : fourcc("undf");
}

bool vr_hot_enabled(const vr *v, uint32_t id)
{
    if (v->all_hots_off)
        return false;
    for (int i = 0; i < v->noff_ids; i++)
        if (v->off_ids[i] == id)
            return false;
    if (v->noff_types) {
        uint32_t t = vr_hot_type(v, id);
        for (int i = 0; i < v->noff_types; i++)
            if (v->off_types[i] == t)
                return false;
    }
    return true;
}

uint32_t vr_hot_at(vr *v, int h, int vv)
{
    int w = v->box.right - v->box.left, ht = v->box.bottom - v->box.top;
    if (w <= 0 || ht <= 0 || h < v->box.left || h >= v->box.right || vv < v->box.top || vv >= v->box.bottom)
        return 0;
    if (v->object)
        object_frame(v);
    uint32_t id = raw_hot(v, h - v->box.left, vv - v->box.top, w, ht);
    return id && vr_hot_enabled(v, id) ? id : 0;
}

int vr_visible_hots(vr *v, uint32_t *ids, int max)
{
    int w = v->box.right - v->box.left, h = v->box.bottom - v->box.top, n = 0;
    bool seen[256] = { false };
    if (v->object)
        object_frame(v);
    for (int y = 0; y < h; y += 2)
        for (int x = 0; x < w; x += 2) {
            uint32_t id = raw_hot(v, x, y, w, h);
            if (id && !seen[id] && vr_hot_enabled(v, id)) {
                seen[id] = true;
                if (n < max)
                    ids[n] = id;
                n++;
            }
        }
    return n;
}

/* The hot spot's pixels in the view, as a region in the port's coordinates. */
gaddr vr_hot_region(vr *v, uint32_t id, gaddr rgn)
{
    int w = v->box.right - v->box.left, h = v->box.bottom - v->box.top;
    tb_SetEmptyRgn(rgn);
    if (w <= 0 || h <= 0)
        return rgn;
    if (v->object)
        object_frame(v);
    int rb = ((w + 15) / 16) * 2;
    gaddr bits = tb_NewPtr(rb * h, false, true), bm = tb_NewPtr(14, false, true);
    if (!bits || !bm)
        return rgn;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (raw_hot(v, x, y, w, h) == id) {
                gaddr p = bits + (uint32_t)(y * rb + x / 8);
                wr8(p, rd8(p) | (uint8_t)(0x80 >> (x & 7)));
            }
    wr32(bm, bits);
    wr16(bm + 4, (uint16_t)rb);
    qrect_put(bm + 6, (qrect){ v->box.top, v->box.left, v->box.bottom, v->box.right });
    tb_BitMapToRegion(rgn, bm);
    tb_DisposePtr(bits);
    tb_DisposePtr(bm);
    return rgn;
}

/* A hot spot's action: a link goes to its node (and view); a URL hot
 * spot's URL goes to the movie controller. */
int16_t vr_trigger(vr *v, uint32_t id, const uint8_t *info, uint32_t info_len, uint32_t atom)
{
    uint32_t n;
    if (!info) {
        if (v->cur < 0)
            return invalidHotSpotIDErr;
        info = v->nodes[v->cur].info;
        info_len = v->nodes[v->cur].info_len;
    }
    if (!atom) {
        uint32_t p = info ? hots_parent(v, info, info_len) : 0;
        atom = p ? qta_find(info, info_len, p, fourcc("hots"), id) : 0;
    }
    if (!atom)
        return v->old || v->hot ? noErr : invalidHotSpotIDErr;
    const uint8_t *hs = atom_leaf(info, info_len, atom, fourcc("hsin"), 0, &n);
    uint32_t type = hs && n >= 8 ? be32(hs + 4) : fourcc("undf");
    if (type == fourcc("link")) {
        const uint8_t *l = atom_leaf(info, info_len, atom, fourcc("link"), 0, &n);
        if (!l || n < 56)
            return noErr;
        uint32_t to = be32(l + 4), valid = be32(l + 32);
        int16_t err = vr_go_to_node(v, (int32_t)to);
        if (err)
            return err;
        if (valid & 4)
            vr_set_angle(v, VR_FOV, bef(l + 44));
        if (valid & 1)
            vr_set_angle(v, VR_PAN, bef(l + 36));
        if (valid & 2)
            vr_set_angle(v, VR_TILT, bef(l + 40));
        if (valid & 8)
            vr_set_center(v, bef(l + 48), bef(l + 52));
        vr_update(v, true);
        return noErr;
    }
    if (type == fourcc("url ") && v->mc) {
        const uint8_t *u = atom_leaf(info, info_len, atom, fourcc("url "), 0, &n);
        if (u && n) {
            gaddr h = tb_NewHandle((int32_t)n + 1, false, true);
            gput(rd32(h), u, n);
            uint32_t args[] = { 59 /* mcActionLinkToURL */, h };
            qtc_call_named(v->mc, "MCDoAction", args);
            tb_DisposeHandle(h);
        }
    }
    return noErr;
}

/* ---- Movement ---- */

/* A nudge one step in a direction (degrees, 0 right, 90 up): a panorama
 * turns by a part of its view, an object by a column or row. */
void vr_nudge(vr *v, int direction)
{
    double a = rad(direction);
    double dx = cos(a), dy = sin(a);
    if (fabs(dx) < 1e-6)
        dx = 0;
    if (fabs(dy) < 1e-6)
        dy = 0;
    if (v->object) {
        float ps = pan_step(v), ts = tilt_step(v);
        int sx = dx > 0 ? 1 : dx < 0 ? -1 : 0, sy = dy > 0 ? 1 : dy < 0 ? -1 : 0;
        if (v->control & 8) /* kQTVRObjectReverseHControlOn */
            sx = -sx;
        if (v->control & 16)
            sy = -sy;
        if (sx)
            vr_set_angle(v, VR_PAN, v->pan + sx * ps);
        if (sy)
            vr_set_angle(v, VR_TILT, v->tilt - sy * ts);
        return;
    }
    float step = v->fov / 8;
    if (dx)
        vr_set_angle(v, VR_PAN, v->pan - (float)(dx * step));
    if (dy)
        vr_set_angle(v, VR_TILT, v->tilt + (float)(dy * step));
}

/* An object's animation and view animation as time passes. */
void vr_animate(vr *v)
{
    int64_t now = (int64_t)tb_TickCount() * 1000 / 60;
    /* QTVRSetViewParameter's rates of change */
    if (v->rate[VR_PAN] != 0 || v->rate[VR_TILT] != 0 || v->rate[VR_FOV] != 0) {
        float secs = v->rate_last_ms ? (float)(now - v->rate_last_ms) / 1000 : 0;
        v->rate_last_ms = now;
        if (secs > 0) {
            float *cur[3] = { &v->pan, &v->tilt, &v->fov };
            for (int k = 0; k < 3; k++)
                if (v->rate[k] != 0)
                    vr_set_angle(v, k, *cur[k] + v->rate[k] * secs);
        }
    } else {
        v->rate_last_ms = 0;
    }
    if (!v->object || !v->loaded)
        return;
    int64_t dt = v->anim_last_ms ? now - v->anim_last_ms : 0;
    v->anim_last_ms = now;
    qtm_movie *mv = qt_movie_model(v->movie);
    if (!mv || dt <= 0)
        return;
    if (v->frame_anim && v->view_duration > v->frame_dur) {
        int64_t adv = (int64_t)(dt * mv->scale / 1000 * v->frame_rate);
        if (adv > 0) {
            int64_t t = v->view_time + adv;
            if (t >= v->view_duration)
                t = v->anim & 64 ? v->view_duration - v->frame_dur : t % v->view_duration; /* kQTVRObjectDontLoopViewFramesOn */
            if (t / v->frame_dur != v->view_time / v->frame_dur)
                vr_update(v, false);
            v->view_time = t;
        }
    }
    if (v->view_anim) {
        static int64_t carry;
        carry += (int64_t)(dt * v->view_rate);
        int64_t per = v->view_duration * 1000 / (mv->scale ? mv->scale : 600);
        if (per <= 0)
            per = 100;
        while (carry >= per) {
            carry -= per;
            vr_set_angle(v, VR_PAN, v->pan + pan_step(v));
        }
    }
}

/* ---- The back buffer: a program's routine drawing into the panorama ---- */

void vr_refresh_back_buffer(vr *v)
{
    qtm_movie *mv = qt_movie_model(v->movie);
    if (!mv || v->object || v->cur < 0)
        return;
    load_picture(v, mv, v->old ? 0 : v->nodes[v->cur].time);
    if (!v->backbuf.proc || !v->image)
        return;
    /* The back buffer as the program sees it: the stored (turned) picture
     * unless it asked for horizontal, in a 32-bit GWorld. */
    for (int a = 0; a < v->nareas || (a == 0 && !v->nareas); a++) {
        bool horiz = v->horizontal || (v->nareas && (v->areas[a].flags & 8));
        int bw = horiz ? v->iw : v->ih, bh = horiz ? v->ih : v->iw;
        gaddr r = tb_NewPtr(8, false, true), gw = tb_NewPtr(4, false, true);
        qrect_put(r, (qrect){ 0, 0, (int16_t)(bh > 32767 ? 32767 : bh), (int16_t)(bw > 32767 ? 32767 : bw) });
        if (tb_NewGWorld(gw, 32, r, 0, 0, 0) || !rd32(gw)) {
            tb_DisposePtr(r);
            tb_DisposePtr(gw);
            return;
        }
        gaddr world = rd32(gw);
        gaddr pm = tb_GetGWorldPixMap(world);
        tb_LockPixels(pm);
        gaddr base = rd32(rd32(pm));
        uint32_t rb = rd16(rd32(pm) + 4) & 0x3FFF;
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++) {
                /* horizontal: as shown; else turned (row y is column iw-1-y) */
                uint32_t p = horiz ? v->image[(size_t)y * v->iw + x] : v->image[(size_t)x * v->iw + (v->iw - 1 - y)];
                wr32(base + (uint32_t)y * rb + (uint32_t)x * 4, p);
            }
        /* the area's rectangle in the back buffer */
        qrect dr = { 0, 0, (int16_t)bh, (int16_t)bw };
        if (v->nareas) {
            float x0, y0, x1, y1;
            vr_coord_from_angles(v, v->areas[a].pan + v->areas[a].width, v->areas[a].tilt, &x0, &y0);
            vr_coord_from_angles(v, v->areas[a].pan, v->areas[a].tilt - v->areas[a].height, &x1, &y1);
            if (x1 < x0)
                x1 += v->iw;
            int l = (int)x0, t = (int)y0, rr = (int)x1, b = (int)y1;
            dr = horiz ? (qrect){ (int16_t)t, (int16_t)l, (int16_t)b, (int16_t)rr }
                       : (qrect){ (int16_t)(v->iw - rr), (int16_t)t, (int16_t)(v->iw - l), (int16_t)b };
        }
        gaddr saved_port = tb_NewPtr(8, false, true);
        tb_GetGWorld(saved_port, saved_port + 4);
        tb_SetGWorld(world, 0);
        gaddr rect = tb_NewPtr(8, false, true), out = tb_NewPtr(4, false, true);
        qrect_put(rect, dr);
        uint32_t args[] = { v->token, rect, (uint32_t)a, 3 /* visible, refreshed */, out, (uint32_t)v->backbuf.refcon };
        mm_call(v->backbuf.proc, 0x0003FBE0, args, 6);
        tb_SetGWorld(rd32(saved_port), rd32(saved_port + 4));
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++) {
                uint32_t p = rd32(base + (uint32_t)y * rb + (uint32_t)x * 4) & 0xFFFFFF;
                if (horiz)
                    v->image[(size_t)y * v->iw + x] = p;
                else
                    v->image[(size_t)x * v->iw + (v->iw - 1 - y)] = p;
            }
        tb_DisposeGWorld(world);
        tb_DisposePtr(r);
        tb_DisposePtr(gw);
        tb_DisposePtr(rect);
        tb_DisposePtr(out);
        tb_DisposePtr(saved_port);
        if (!v->nareas)
            break;
    }
    vr_update(v, false);
}

/* Zooming while a button or key is held: by time, not by how often the
 * program asks (QuickTime VR's zoom speed 5 is about a third a second). */
static uint32_t zoom_ticks;

static void zoom_step(vr *v, int dir, bool first)
{
    uint32_t now = tb_TickCount();
    uint32_t t = first || now - zoom_ticks > 30 ? 1 : now - zoom_ticks;
    zoom_ticks = now;
    if (!t && !first)
        return;
    float speed = (float)(v->interaction[4] ? v->interaction[4] : 5) / 5;
    vr_set_angle(v, VR_FOV, v->fov * powf(dir > 0 ? 0.993f : 1.007f, (float)t * speed));
}

/* ---- The Movie Toolbox's hooks ---- */

bool qt_vr_movie_draw(gaddr movie, gaddr port, qrect box, bool force)
{
    vr *v = vr_for_movie(movie, true);
    if (!v)
        return false;
    v->port = port;
    v->box = box;
    if (force || v->dirty)
        draw(v);
    return true;
}

static uint32_t local_mouse(vr *v);

bool qt_vr_movie(gaddr movie) { return vr_for_movie(movie, true) != NULL; }

void qt_vr_movie_idle(gaddr movie)
{
    vr *v = vr_for_movie(movie, false);
    if (!v)
        return;
    vr_animate(v);
    if (v->mc && v->port) /* a controller shows it: hot spots under the mouse */
        vr_idle_mouse(v, local_mouse(v));
    if (v->dirty)
        draw(v);
}

static uint32_t local_mouse(vr *v)
{
    gaddr p = tb_NewPtr(4, false, true), saved = qd_port();
    if (v->port)
        tb_SetPort(v->port);
    tb_GetMouse(p);
    tb_SetPort(saved);
    uint32_t pt = rd32(p);
    tb_DisposePtr(p);
    return pt;
}

/* The controller's click: the program's mouse calls in a loop while the
 * button is down, as QuickTime VR's controller did. */
bool qt_vr_movie_click(gaddr movie, gaddr mc, uint32_t pt, uint16_t modifiers)
{
    vr *v = vr_for_movie(movie, true);
    if (!v)
        return false;
    int h = pt_h(pt), vv = pt_v(pt);
    if (h < v->box.left || h >= v->box.right || vv < v->box.top || vv >= v->box.bottom)
        return false;
    if (mc)
        v->mc = mc;
    uint32_t p[6] = { pt, tb_TickCount(), modifiers, 0, 0, 0 };
    vr_call(v, SEL_DOWN, p, 5);
    uint32_t last = pt;
    for (int guard = 0; guard < 100000 && tb_StillDown() && !g_cpu->halted; guard++) {
        ev_wait_tick(); /* time passes (the view turns by time) */
        last = local_mouse(v);
        uint32_t q[6] = { last, 0, 0, 0, 0, 0 };
        vr_call(v, SEL_STILLDOWN, q, 3);
        if (v->dirty)
            draw(v);
    }
    uint32_t q[6] = { last, 0, 0, 0, 0, 0 };
    vr_call(v, SEL_UP, q, 3);
    if (v->dirty)
        draw(v);
    return true;
}

/* Keys: the arrows turn, shift and control (or + and -) zoom in and out. */
bool qt_vr_movie_key(gaddr movie, uint8_t ch, uint16_t modifiers)
{
    vr *v = vr_for_movie(movie, false);
    if (!v)
        return false;
    (void)modifiers;
    switch (ch) {
    case 0x1C: vr_nudge(v, 180); break;
    case 0x1D: vr_nudge(v, 0); break;
    case 0x1E: vr_nudge(v, 90); break;
    case 0x1F: vr_nudge(v, 270); break;
    case '+': case '=': vr_set_angle(v, VR_FOV, v->fov * 0.8f); break;
    case '-': vr_set_angle(v, VR_FOV, v->fov * 1.25f); break;
    default: return false;
    }
    if (v->dirty)
        draw(v);
    return true;
}

void qt_vr_movie_modifiers(gaddr movie, uint16_t modifiers)
{
    vr *v = vr_for_movie(movie, false);
    if (!v)
        return;
    static bool held;
    if (modifiers & 0x0200) /* shift: in */
        zoom_step(v, 1, !held);
    else if (modifiers & 0x1000) /* control: out */
        zoom_step(v, -1, !held);
    held = (modifiers & 0x1200) != 0;
}

/* ---- The controller's bar ---- */

enum { BTN_BACK, BTN_ZOOM_OUT, BTN_ZOOM_IN, BTN_HOTS, NBTN };

/* The buttons shown, left to right (mcFlagQTVRSuppress... hide some). */
static int bar_buttons(const vr *v, int32_t flags, int out[NBTN])
{
    int n = 0;
    if (!(flags & (1 << 16)) && v->nnodes > 1)
        out[n++] = BTN_BACK;
    if (!(flags & (1 << 17)) && (!v->object || (v->control & 4))) {
        out[n++] = BTN_ZOOM_OUT;
        out[n++] = BTN_ZOOM_IN;
    }
    if (!(flags & (1 << 18)))
        out[n++] = BTN_HOTS;
    return n;
}

static qrect button_rect(qrect bar, int i)
{
    int16_t l = (int16_t)(bar.left + 3 + i * 22);
    return (qrect){ (int16_t)(bar.top + 2), l, (int16_t)(bar.bottom - 2), (int16_t)(l + 20) };
}

static void line(int h0, int v0, int h1, int v1)
{
    tb_MoveTo((int16_t)h0, (int16_t)v0);
    tb_LineTo((int16_t)h1, (int16_t)v1);
}

static void draw_button(qrect r, int kind, bool pressed)
{
    gaddr p = tb_NewPtr(8, false, true);
    qrect_put(p, r);
    tb_EraseRect(p);
    tb_FrameRect(p);
    int ch = (r.left + r.right) / 2, cv = (r.top + r.bottom) / 2;
    switch (kind) {
    case BTN_BACK: /* a left arrow */
        line(ch - 4, cv, ch + 4, cv);
        line(ch - 4, cv, ch - 1, cv - 3);
        line(ch - 4, cv, ch - 1, cv + 3);
        break;
    case BTN_ZOOM_OUT: line(ch - 4, cv, ch + 4, cv); break;
    case BTN_ZOOM_IN:
        line(ch - 4, cv, ch + 4, cv);
        line(ch, cv - 4, ch, cv + 4);
        break;
    case BTN_HOTS: /* an up arrow */
        line(ch, cv - 4, ch, cv + 4);
        line(ch, cv - 4, ch - 3, cv - 1);
        line(ch, cv - 4, ch + 3, cv - 1);
        break;
    }
    if (pressed)
        tb_InvertRect(p);
    tb_DisposePtr(p);
}

bool qt_vr_draw_bar(gaddr movie, gaddr port, qrect bar, int32_t mc_flags)
{
    vr *v = vr_for_movie(movie, true);
    if (!v)
        return false;
    gaddr saved = qd_port();
    tb_SetPort(port);
    tb_PenNormal();
    gaddr p = tb_NewPtr(8, false, true);
    qrect_put(p, bar);
    tb_EraseRect(p);
    tb_FrameRect(p);
    tb_DisposePtr(p);
    int b[NBTN], n = bar_buttons(v, mc_flags, b);
    for (int i = 0; i < n; i++)
        draw_button(button_rect(bar, i), b[i], b[i] == BTN_HOTS && v->hots_visible);
    tb_SetPort(saved);
    return true;
}

bool qt_vr_bar_click(gaddr movie, gaddr mc, uint32_t pt, qrect bar, int32_t mc_flags)
{
    vr *v = vr_for_movie(movie, true);
    if (!v)
        return false;
    if (mc)
        v->mc = mc;
    int b[NBTN], n = bar_buttons(v, mc_flags, b), hit = -1;
    for (int i = 0; i < n; i++) {
        qrect r = button_rect(bar, i);
        if (pt_h(pt) >= r.left && pt_h(pt) < r.right && pt_v(pt) >= r.top && pt_v(pt) < r.bottom)
            hit = i;
    }
    if (hit < 0)
        return true; /* the rest of the bar does nothing */
    gaddr saved = qd_port();
    tb_SetPort(v->port ? v->port : saved);
    qrect r = button_rect(bar, hit);
    draw_button(r, b[hit], true);
    switch (b[hit]) {
    case BTN_BACK:
        vr_go_to_node(v, kQTVRPreviousNode);
        break;
    case BTN_ZOOM_OUT:
    case BTN_ZOOM_IN: {
        int guard = 0;
        do {
            zoom_step(v, b[hit] == BTN_ZOOM_IN ? 1 : -1, guard == 0);
            if (v->dirty)
                draw(v);
            ev_wait_tick();
        } while (tb_StillDown() && !g_cpu->halted && ++guard < 100000);
        break;
    }
    case BTN_HOTS: /* shown while the button is held */
        v->hots_visible = true;
        draw(v);
        for (int guard = 0; tb_StillDown() && !g_cpu->halted && guard < 100000; guard++)
            ev_wait_tick();
        v->hots_visible = false;
        break;
    }
    for (int guard = 0; tb_StillDown() && !g_cpu->halted && guard < 100000; guard++)
        ev_wait_tick();
    draw_button(r, b[hit], false);
    tb_SetPort(saved);
    draw(v);
    return true;
}

void qt_vr_movie_gone(gaddr movie)
{
    for (int i = 0; i < MAX_VR; i++)
        if (vrs[i].used && vrs[i].movie == movie)
            free_vr(&vrs[i]);
}

void qt_vr_reset(void)
{
    for (int i = 0; i < MAX_VR; i++)
        if (vrs[i].used) {
            vrs[i].token = 0; /* (the old heap's) */
            free_vr(&vrs[i]);
        }
}
