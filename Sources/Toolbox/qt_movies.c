/*
 * The Movie Toolbox's calls on tracks and media: making them, their
 * settings, their sample tables (adding samples, reading them back, finding
 * them by time), their data references, edits within a track, track
 * references, alternates and the times worth stopping at.  They work on the
 * records quicktime.c keeps for each movie (qt_model.c), and a movie plays
 * from them again once they change.
 *
 * New samples are written where the media's data reference says, through
 * qt_dref.c: a movie file's data fork, another file, or a handle in memory
 * (a movie made with NewMovie and no file keeps its media's data in a
 * handle of its own, so it plays like any other).
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qd_internal.h"
#include "qt_atoms.h"
#include "qt_comp.h"
#include "qt_dref.h"
#include "qt_edit.h"
#include "qt_errors.h"
#include "qt_model.h"
#include "qt_movie_internal.h"
#include "tb_internal.h"

uint32_t mac_now(void);

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static qtm_movie *movie_or_error(int32_t theMovie)
{
    qtm_movie *mv = qt_movie_model((gaddr)theMovie);
    qt_set_error(mv ? noErr : invalidMovie);
    return mv;
}

static qtm_track *track_or_error(int32_t theTrack, qtm_movie **mv)
{
    qtm_track *t = qt_track_model((gaddr)theTrack, mv);
    qt_set_error(t ? noErr : invalidTrack);
    return t;
}

static qtm_media *media_or_error(int32_t theMedia, qtm_track **t, qtm_movie **mv)
{
    qtm_media *md = qt_media_model((gaddr)theMedia, t, mv);
    qt_set_error(md ? noErr : invalidMedia);
    return md;
}

static void put_matrix(gaddr p, const int32_t m[9])
{
    for (int i = 0; i < 9; i++)
        wr32(p + 4 * (uint32_t)i, (uint32_t)m[i]);
}

static void get_matrix(gaddr p, int32_t m[9])
{
    if (!p) {
        qtm_identity(m);
        return;
    }
    for (int i = 0; i < 9; i++)
        m[i] = (int32_t)rd32(p + 4 * (uint32_t)i);
}

static int64_t rescale(int64_t v, int32_t from, int32_t to)
{
    if (from <= 0 || to <= 0 || from == to)
        return v;
    return (int64_t)((__int128)v * to / from);
}

/* ---- Characteristics ---- */

bool qt_media_has_characteristic(uint32_t type, uint32_t c)
{
    switch (c) {
    case 0x65796573: /* 'eyes' VisualMediaCharacteristic */
        return type == fourcc("vide") || type == fourcc("text") || type == fourcc("sprt") ||
               type == fourcc("tx3g") || type == fourcc("qd3d") || type == fourcc("flsh") ||
               type == fourcc("pano") || type == fourcc("qtvr") || type == fourcc("mpeg") || type == fourcc("strm");
    case 0x65617273: /* 'ears' AudioMediaCharacteristic */
        return type == fourcc("soun") || type == fourcc("musi") || type == fourcc("mpeg") || type == fourcc("strm");
    case 0x76736E64: /* 'vsnd' kCharacteristicCanSendVideo */
        return type == fourcc("vide");
    case 0x6163746E: /* 'actn' kCharacteristicProvidesActions */
        return type == fourcc("sprt") || type == fourcc("text") || type == fourcc("flsh") || type == fourcc("qtvr");
    case 0x6E6F6E6C: /* 'nonl' kCharacteristicNonLinear */
        return type == fourcc("qtvr") || type == fourcc("pano") || type == fourcc("obje");
    case 0x73746570: /* 'step' kCharacteristicCanStep */
        return type == fourcc("vide") || type == fourcc("text") || type == fourcc("sprt") || type == fourcc("mpeg");
    case 0x6E6F7469: /* 'noti' kCharacteristicHasNoDuration */
        return false;
    }
    return false;
}

/* ---- Movies ---- */

void tb_GoToEndOfMovie(int32_t theMovie)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (mv)
        tb_SetMovieTimeValue(theMovie, (int32_t)mv->duration);
}

int8_t tb_GetMoviePreviewMode(int32_t theMovie)
{
    qtm_movie *mv = movie_or_error(theMovie);
    return mv && mv->preview_mode;
}

/* In preview mode only the tracks used in the preview play, over the
 * preview's stretch: the active segment is the preview. */
void tb_SetMoviePreviewMode(int32_t theMovie, int8_t usePreview)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (!mv)
        return;
    mv->preview_mode = usePreview != 0;
    if (mv->preview_mode && mv->preview_duration > 0) {
        mv->active_start = mv->preview_time;
        mv->active_duration = mv->preview_duration;
        tb_SetMovieTimeValue(theMovie, mv->preview_time);
    } else {
        mv->active_start = mv->active_duration = -1;
    }
}

/* PlayMoviePreview: the preview played from its start, the rest of the
 * movie out of it, until it ends (the active segment stops it there). */
void tb_PlayMoviePreview(int32_t theMovie, int32_t callOutProc, int32_t refcon)
{
    (void)callOutProc;
    (void)refcon;
    qtm_movie *mv = movie_or_error(theMovie);
    if (!mv || mv->preview_duration <= 0)
        return;
    tb_SetMoviePreviewMode(theMovie, 1);
    tb_StartMovie((gaddr)theMovie);
}

void tb_ChooseMovieClock(int32_t m, int32_t flags)
{
    (void)flags;
    /* the movie's own clock again (ours all keep the machine's time) */
    if (movie_or_error(m))
        tb_SetMovieMasterClock(m, 0, 0);
}

void tb_SetMovieDrawingCompleteProc(int32_t theMovie, int32_t flags, int32_t proc, int32_t refCon)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (!mv)
        return;
    mv->draw_proc = (gaddr)proc;
    mv->draw_flags = flags;
    mv->draw_refcon = refCon;
}

/* The drawing-complete procedure: pascal OSErr (Movie, long refCon), after
 * each frame (movieDrawingCallWhenChanged) or every draw (Always). */
void qt_movie_drawing_complete(qtm_movie *mv)
{
    if (!mv->draw_proc)
        return;
    cpu68k *c = g_cpu;
    cpu_push16(c, 0);
    cpu_push32(c, qt_movie_handle(mv));
    cpu_push32(c, (uint32_t)mv->draw_refcon);
    guest_call(mv->draw_proc);
    cpu_pop16(c);
}

/* Compositing order: tracks are drawn from the highest layer (furthest
 * back) to the lowest, those of a layer in the order they are in. */
static int compare_layer(const qtm_track *a, int ia, const qtm_track *b, int ib)
{
    if (a->layer != b->layer)
        return a->layer > b->layer ? -1 : 1;
    return ia < ib ? -1 : ia > ib ? 1 : 0;
}

static int32_t step_compositing(int32_t theMovie, int32_t theTrack, int dir)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (!mv)
        return 0;
    int at = -1;
    for (int i = 0; i < mv->ntracks; i++)
        if (mv->tracks[i]->token == (gaddr)theTrack)
            at = i;
    int best = -1;
    for (int i = 0; i < mv->ntracks; i++) {
        if (i == at)
            continue;
        const qtm_track *t = mv->tracks[i];
        if (at >= 0 && compare_layer(t, i, mv->tracks[at], at) * dir <= 0)
            continue;
        if (best < 0 || compare_layer(t, i, mv->tracks[best], best) * dir < 0)
            best = i;
    }
    return best >= 0 ? (int32_t)mv->tracks[best]->token : 0;
}

int32_t tb_GetNextTrackForCompositing(int32_t theMovie, int32_t theTrack)
{
    return step_compositing(theMovie, theTrack, 1);
}

int32_t tb_GetPrevTrackForCompositing(int32_t theMovie, int32_t theTrack)
{
    return step_compositing(theMovie, theTrack, -1);
}

void tb_SetMovieVideoOutput(int32_t theMovie, int32_t vout)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (mv)
        mv->video_output = (gaddr)vout;
}

/* A new time scale: every time in the movie's scale converted. */
void tb_SetMovieTimeScale(int32_t theMovie, int32_t timeScale)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (!mv || timeScale <= 0 || timeScale == mv->scale)
        return;
    int32_t from = mv->scale;
    for (int i = 0; i < mv->ntracks; i++)
        for (int k = 0; k < mv->tracks[i]->nedits; k++)
            mv->tracks[i]->edits[k].duration = rescale(mv->tracks[i]->edits[k].duration, from, timeScale);
    mv->preview_time = (int32_t)rescale(mv->preview_time, from, timeScale);
    mv->preview_duration = (int32_t)rescale(mv->preview_duration, from, timeScale);
    mv->poster_time = (int32_t)rescale(mv->poster_time, from, timeScale);
    mv->sel_time = (int32_t)rescale(mv->sel_time, from, timeScale);
    mv->sel_duration = (int32_t)rescale(mv->sel_duration, from, timeScale);
    int64_t now = qt_movie_now(mv);
    mv->scale = timeScale;
    qt_movie_touch(mv);
    tb_SetMovieTimeValue(theMovie, (int32_t)rescale(now, from, timeScale));
}

void tb_GetMovieMatrix(int32_t theMovie, gaddr matrix)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (mv && matrix)
        put_matrix(matrix, mv->matrix);
}

void tb_SetMovieMatrix(int32_t theMovie, gaddr matrix)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (!mv)
        return;
    get_matrix(matrix, mv->matrix);
    qt_movie_touch(mv);
}

void tb_GetMoviePreviewTime(int32_t theMovie, gaddr previewTime, gaddr previewDuration)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (previewTime)
        wr32(previewTime, mv ? (uint32_t)mv->preview_time : 0);
    if (previewDuration)
        wr32(previewDuration, mv ? (uint32_t)mv->preview_duration : 0);
}

void tb_SetMoviePreviewTime(int32_t theMovie, int32_t previewTime, int32_t previewDuration)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (!mv)
        return;
    if (previewTime < 0 || previewDuration < 0) {
        qt_set_error(invalidTime);
        return;
    }
    mv->preview_time = previewTime;
    mv->preview_duration = previewDuration;
    qt_movie_touch(mv);
}

int32_t tb_GetMoviePosterTime(int32_t theMovie)
{
    qtm_movie *mv = movie_or_error(theMovie);
    return mv ? mv->poster_time : 0;
}

void tb_SetMoviePosterTime(int32_t theMovie, int32_t posterTime)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (!mv)
        return;
    if (posterTime < 0) {
        qt_set_error(invalidTime);
        return;
    }
    mv->poster_time = posterTime;
    qt_movie_touch(mv);
}

void tb_SetMovieActiveSegment(int32_t theMovie, int32_t startTime, int32_t duration)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (!mv)
        return;
    mv->active_start = startTime;
    mv->active_duration = startTime < 0 ? -1 : duration;
}

void tb_GetMovieActiveSegment(int32_t theMovie, gaddr startTime, gaddr duration)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (startTime)
        wr32(startTime, mv ? (uint32_t)mv->active_start : (uint32_t)-1);
    if (duration)
        wr32(duration, mv ? (uint32_t)mv->active_duration : 0);
}

int32_t tb_GetMovieUserData(int32_t theMovie)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (mv && !mv->udata)
        mv->udata = qt_udata_new();
    return mv ? (int32_t)mv->udata : 0;
}

void tb_SetMovieLanguage(int32_t theMovie, int32_t language)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (!mv)
        return;
    mv->language = (int16_t)language;
    tb_SelectMovieAlternates(theMovie);
}

int16_t tb_CopyMovieUserData(int32_t srcMovie, int32_t dstMovie, int32_t copyRule)
{
    qtm_movie *s = movie_or_error(srcMovie), *d = movie_or_error(dstMovie);
    if (!s || !d)
        return invalidMovie;
    return qt_set_error(tb_CopyUserData((int32_t)s->udata, (int32_t)d->udata, copyRule));
}

int16_t tb_CopyTrackUserData(int32_t srcTrack, int32_t dstTrack, int32_t copyRule)
{
    qtm_track *s = track_or_error(srcTrack, NULL), *d = track_or_error(dstTrack, NULL);
    if (!s || !d)
        return invalidTrack;
    return qt_set_error(tb_CopyUserData((int32_t)s->udata, (int32_t)d->udata, copyRule));
}

int16_t tb_CopyMediaUserData(int32_t srcMedia, int32_t dstMedia, int32_t copyRule)
{
    qtm_media *s = media_or_error(srcMedia, NULL, NULL), *d = media_or_error(dstMedia, NULL, NULL);
    if (!s || !d)
        return invalidMedia;
    return qt_set_error(tb_CopyUserData((int32_t)s->udata, (int32_t)d->udata, copyRule));
}

/* Property atoms: copies kept, copies given. */
static gaddr copy_handle(gaddr h)
{
    if (!h || !rd32(h))
        return 0;
    int32_t n = tb_GetHandleSize(h);
    gaddr c = tb_NewHandle(n, false, false);
    if (c && rd32(c) && n)
        gmemcpy(rd32(c), rd32(h), (uint32_t)n);
    return c;
}

int16_t tb_SetMoviePropertyAtom(int32_t theMovie, int32_t propertyAtom)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (!mv)
        return invalidMovie;
    if (mv->property_atom)
        tb_DisposeHandle(mv->property_atom);
    mv->property_atom = copy_handle((gaddr)propertyAtom);
    return noErr;
}

int16_t tb_GetMoviePropertyAtom(int32_t theMovie, gaddr propertyAtom)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (propertyAtom)
        wr32(propertyAtom, 0);
    if (!mv)
        return invalidMovie;
    gaddr h = mv->property_atom ? copy_handle(mv->property_atom) : qt_atoms_new();
    if (propertyAtom)
        wr32(propertyAtom, h);
    return h ? noErr : memFullErr;
}

int16_t tb_SetMovieColorTable(int32_t theMovie, int32_t ctab)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (!mv)
        return invalidMovie;
    if (mv->ctab)
        tb_DisposeHandle(mv->ctab);
    mv->ctab = copy_handle((gaddr)ctab);
    qt_movie_touch(mv);
    return noErr;
}

int16_t tb_GetMovieColorTable(int32_t theMovie, gaddr ctab)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (ctab)
        wr32(ctab, mv && mv->ctab ? copy_handle(mv->ctab) : 0);
    return mv ? noErr : invalidMovie;
}

void tb_SetMovieProgressProc(int32_t theMovie, int32_t p, int32_t refcon)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (!mv)
        return;
    /* -1: the Movie Toolbox's own progress dialog, which we don't show */
    mv->progress_proc = p == -1 ? 0 : (gaddr)p;
    mv->progress_refcon = refcon;
}

void tb_GetMovieProgressProc(int32_t theMovie, gaddr p, gaddr refcon)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (p)
        wr32(p, mv ? mv->progress_proc : 0);
    if (refcon)
        wr32(refcon, mv ? (uint32_t)mv->progress_refcon : 0);
}

void tb_GetPosterBox(int32_t theMovie, gaddr boxRect)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (!boxRect)
        return;
    if (mv && (mv->poster_box[2] > mv->poster_box[0] || mv->poster_box[3] > mv->poster_box[1])) {
        for (int i = 0; i < 4; i++)
            wr16(boxRect + 2 * (uint32_t)i, (uint16_t)mv->poster_box[i]);
    } else if (mv) {
        tb_GetMovieBox((gaddr)theMovie, boxRect);
    }
}

void tb_SetPosterBox(int32_t theMovie, gaddr boxRect)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (!mv || !boxRect)
        return;
    for (int i = 0; i < 4; i++)
        mv->poster_box[i] = (int16_t)rd16(boxRect + 2 * (uint32_t)i);
}

void tb_SetMovieCoverProcs(int32_t theMovie, int32_t uncoverProc, int32_t coverProc, int32_t refcon)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (!mv)
        return;
    mv->uncover_proc = (gaddr)uncoverProc;
    mv->cover_proc = (gaddr)coverProc;
    mv->cover_refcon = refcon;
}

int16_t tb_GetMovieCoverProcs(int32_t theMovie, gaddr uncoverProc, gaddr coverProc, gaddr refcon)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (uncoverProc)
        wr32(uncoverProc, mv ? mv->uncover_proc : 0);
    if (coverProc)
        wr32(coverProc, mv ? mv->cover_proc : 0);
    if (refcon)
        wr32(refcon, mv ? (uint32_t)mv->cover_refcon : 0);
    return mv ? noErr : invalidMovie;
}

/* Everything is on this machine: loaded, all of it. */
int32_t tb_GetMovieLoadState(int32_t theMovie)
{
    return movie_or_error(theMovie) ? 100000 /* kMovieLoadStateComplete */ : -1;
}

int16_t tb_GetMaxLoadedTimeInMovie(int32_t theMovie, gaddr time)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (time)
        wr32(time, mv ? (uint32_t)mv->duration : 0);
    return mv ? noErr : invalidMovie;
}

int16_t tb_QTMovieNeedsTimeTable(int32_t theMovie, gaddr needsTimeTable)
{
    if (needsTimeTable)
        wr8(needsTimeTable, 0);
    return movie_or_error(theMovie) ? noErr : invalidMovie;
}

int32_t tb_GetMovieStatus(int32_t theMovie, gaddr firstProblemTrack)
{
    if (firstProblemTrack)
        wr32(firstProblemTrack, 0);
    return movie_or_error(theMovie) ? noErr : invalidMovie;
}

int32_t tb_GetTrackStatus(int32_t theTrack) { return track_or_error(theTrack, NULL) ? noErr : invalidTrack; }

int16_t tb_LoadMovieIntoRam(int32_t theMovie, int32_t time, int32_t duration, int32_t flags)
{
    (void)time, (void)duration, (void)flags;
    return qt_set_error(movie_or_error(theMovie) ? noErr : invalidMovie);
}

int16_t tb_LoadTrackIntoRam(int32_t theTrack, int32_t time, int32_t duration, int32_t flags)
{
    (void)time, (void)duration, (void)flags;
    return qt_set_error(track_or_error(theTrack, NULL) ? noErr : invalidTrack);
}

int16_t tb_LoadMediaIntoRam(int32_t theMedia, int32_t time, int32_t duration, int32_t flags)
{
    (void)time, (void)duration, (void)flags;
    return qt_set_error(media_or_error(theMedia, NULL, NULL) ? noErr : invalidMedia);
}

/* PrePrerollMovie: everything is local, so it is ready at once (an
 * asynchronous request's procedure is called back with no error). */
int16_t tb_PrePrerollMovie(int32_t m, int32_t time, int32_t rate, int32_t proc, gaddr refcon)
{
    (void)time, (void)rate;
    if (!movie_or_error(m))
        return invalidMovie;
    if (proc) { /* pascal void (Movie, OSErr, void *refcon) */
        cpu68k *c = g_cpu;
        cpu_push32(c, (uint32_t)m);
        cpu_push16(c, noErr);
        cpu_push32(c, refcon);
        guest_call((gaddr)proc);
    }
    return noErr;
}

void tb_AbortPrePrerollMovie(int32_t m, int16_t err) { (void)m, (void)err; }

/* ---- Tracks ---- */

int32_t tb_GetMovieTrack(int32_t theMovie, int32_t trackID)
{
    qtm_movie *mv = movie_or_error(theMovie);
    qtm_track *t = mv ? qtm_track_by_id(mv, (uint32_t)trackID) : NULL;
    if (mv && !t)
        qt_set_error(trackIDNotFound);
    return t ? (int32_t)t->token : 0;
}

int32_t tb_GetTrackID(int32_t theTrack)
{
    qtm_track *t = track_or_error(theTrack, NULL);
    return t ? (int32_t)t->id : 0;
}

int32_t tb_NewMovieTrack(int32_t theMovie, int32_t width, int32_t height, int16_t trackVolume)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (!mv)
        return 0;
    qtm_track *t = qtm_add_track(mv, width, height, trackVolume, mac_now());
    qt_movie_touch(mv);
    return (int32_t)t->token;
}

/* A track's references to one going away are removed with it. */
static void forget_references_to(qtm_movie *mv, uint32_t id)
{
    for (int i = 0; i < mv->ntracks; i++)
        for (int r = 0; r < mv->tracks[i]->ntrefs; r++) {
            qtm_tref *f = &mv->tracks[i]->trefs[r];
            for (int k = 0; k < f->n; k++)
                if (f->ids[k] == id)
                    f->ids[k] = 0;
        }
}

void tb_DisposeMovieTrack(int32_t theTrack)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    if (!t)
        return;
    forget_references_to(mv, t->id);
    qtm_remove_track(mv, t);
    qt_movie_touch(mv);
}

int32_t tb_GetTrackCreationTime(int32_t theTrack)
{
    qtm_track *t = track_or_error(theTrack, NULL);
    return t ? (int32_t)t->created : 0;
}

int32_t tb_GetTrackModificationTime(int32_t theTrack)
{
    qtm_track *t = track_or_error(theTrack, NULL);
    return t ? (int32_t)t->modified : 0;
}

int32_t tb_GetTrackUsage(int32_t theTrack)
{
    qtm_track *t = track_or_error(theTrack, NULL);
    return t ? (int32_t)(t->flags & 0xE) : 0;
}

void tb_SetTrackUsage(int32_t theTrack, int32_t usage)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    if (!t)
        return;
    t->flags = (t->flags & ~0xEu) | ((uint32_t)usage & 0xE);
    qt_movie_touch(mv);
}

int32_t tb_GetTrackDuration(int32_t theTrack)
{
    qtm_track *t = track_or_error(theTrack, NULL);
    return t ? (int32_t)qtm_track_duration(t) : 0;
}

/* A track's offset: the empty stretch its edits begin with. */
int32_t tb_GetTrackOffset(int32_t theTrack)
{
    qtm_track *t = track_or_error(theTrack, NULL);
    return t && t->nedits && t->edits[0].media_time < 0 ? (int32_t)t->edits[0].duration : 0;
}

void tb_SetTrackOffset(int32_t theTrack, int32_t movieOffsetTime)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    if (!t)
        return;
    if (movieOffsetTime < 0) {
        qt_set_error(invalidTime);
        return;
    }
    bool has = t->nedits && t->edits[0].media_time < 0;
    if (has && movieOffsetTime) {
        t->edits[0].duration = movieOffsetTime;
    } else if (has) {
        memmove(t->edits, t->edits + 1, (size_t)(t->nedits - 1) * sizeof *t->edits);
        t->nedits--;
    } else if (movieOffsetTime) {
        t->edits = realloc(t->edits, (size_t)(t->nedits + 1) * sizeof *t->edits);
        memmove(t->edits + 1, t->edits, (size_t)t->nedits * sizeof *t->edits);
        t->edits[0] = (qtm_edit){ movieOffsetTime, -1, 0x10000 };
        t->nedits++;
    }
    qt_movie_touch(mv);
}

int16_t tb_GetTrackLayer(int32_t theTrack)
{
    qtm_track *t = track_or_error(theTrack, NULL);
    return t ? t->layer : 0;
}

void tb_SetTrackLayer(int32_t theTrack, int16_t layer)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    if (!t)
        return;
    t->layer = layer;
    qt_movie_touch(mv);
}

/* Alternate groups: tracks of the same group number, one of which plays.
 * GetTrackAlternate gives the next in the group (round to the first). */
int32_t tb_GetTrackAlternate(int32_t theTrack)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    if (!t)
        return 0;
    if (!t->alternate)
        return theTrack;
    int at = 0;
    while (at < mv->ntracks && mv->tracks[at] != t)
        at++;
    for (int k = 1; k <= mv->ntracks; k++) {
        qtm_track *o = mv->tracks[(at + k) % mv->ntracks];
        if (o->alternate == t->alternate)
            return (int32_t)o->token;
    }
    return theTrack;
}

void tb_SetTrackAlternate(int32_t theTrack, int32_t alternateT)
{
    qtm_movie *mv, *mv2;
    qtm_track *t = track_or_error(theTrack, &mv);
    if (!t)
        return;
    if (!alternateT || alternateT == theTrack) { /* out of its group */
        t->alternate = 0;
        qt_movie_touch(mv);
        return;
    }
    qtm_track *o = track_or_error(alternateT, &mv2);
    if (!o || mv2 != mv)
        return;
    if (!o->alternate) {
        int16_t g = 0;
        for (int i = 0; i < mv->ntracks; i++)
            if (mv->tracks[i]->alternate > g)
                g = mv->tracks[i]->alternate;
        o->alternate = (int16_t)(g + 1);
    }
    t->alternate = o->alternate;
    qt_movie_touch(mv);
}

void tb_SetAutoTrackAlternatesEnabled(int32_t theMovie, int8_t enable)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (mv)
        mv->auto_alternates_off = !enable;
}

/* In each alternate group, the track whose language is the movie's (or
 * the first), best quality first; the others disabled. */
void tb_SelectMovieAlternates(int32_t theMovie)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (!mv)
        return;
    for (int i = 0; i < mv->ntracks; i++) {
        int16_t g = mv->tracks[i]->alternate;
        if (!g)
            continue;
        int best = -1, best_score = -1;
        for (int k = 0; k < mv->ntracks; k++) {
            const qtm_track *t = mv->tracks[k];
            if (t->alternate != g)
                continue;
            int score = (t->media && t->media->language == mv->language ? 0x10000 : 0) +
                        (t->media ? (t->media->quality & 0xFF) : 0);
            if (score > best_score)
                best = k, best_score = score;
        }
        for (int k = 0; k < mv->ntracks; k++)
            if (mv->tracks[k]->alternate == g)
                mv->tracks[k]->flags = k == best ? mv->tracks[k]->flags | 1 : mv->tracks[k]->flags & ~1u;
    }
    qt_movie_touch(mv);
}

int16_t tb_GetTrackVolume(int32_t theTrack)
{
    qtm_track *t = track_or_error(theTrack, NULL);
    return t ? t->volume : 0;
}

void tb_SetTrackVolume(int32_t theTrack, int16_t volume)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    if (!t)
        return;
    t->volume = volume;
    qt_movie_touch(mv);
}

void tb_GetTrackMatrix(int32_t theTrack, gaddr matrix)
{
    qtm_track *t = track_or_error(theTrack, NULL);
    if (t && matrix)
        put_matrix(matrix, t->matrix);
}

void tb_SetTrackMatrix(int32_t theTrack, gaddr matrix)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    if (!t)
        return;
    get_matrix(matrix, t->matrix);
    qt_movie_touch(mv);
}

void tb_GetTrackDimensions(int32_t theTrack, gaddr width, gaddr height)
{
    qtm_track *t = track_or_error(theTrack, NULL);
    if (width)
        wr32(width, t ? (uint32_t)t->width : 0);
    if (height)
        wr32(height, t ? (uint32_t)t->height : 0);
}

void tb_SetTrackDimensions(int32_t theTrack, int32_t width, int32_t height)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    if (!t)
        return;
    t->width = width;
    t->height = height;
    qt_movie_touch(mv);
}

int32_t tb_GetTrackUserData(int32_t theTrack)
{
    qtm_track *t = track_or_error(theTrack, NULL);
    if (t && !t->udata)
        t->udata = qt_udata_new();
    return t ? (int32_t)t->udata : 0;
}

/* The matrix from the track's own coordinates to the movie's display:
 * the track's, then the movie's, then natural bounds to the movie's box. */
static void concat(const int32_t a[9], const int32_t b[9], int32_t out[9])
{
    double x[9], y[9], r[9];
    for (int i = 0; i < 9; i++) {
        double s = (i % 3 == 2) ? (double)(1 << 30) : 65536.0;
        x[i] = a[i] / s;
        y[i] = b[i] / s;
    }
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r[i * 3 + j] = x[i * 3] * y[j] + x[i * 3 + 1] * y[3 + j] + x[i * 3 + 2] * y[6 + j];
    for (int i = 0; i < 9; i++) {
        double s = (i % 3 == 2) ? (double)(1 << 30) : 65536.0;
        out[i] = (int32_t)(r[i] * s + (r[i] < 0 ? -0.5 : 0.5));
    }
}

int16_t tb_GetTrackDisplayMatrix(int32_t theTrack, gaddr matrix)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    if (!t)
        return invalidTrack;
    int32_t m1[9], m2[9];
    concat(t->matrix, mv->matrix, m1);
    /* the movie's natural bounds to its box */
    gaddr box = tb_NewPtr(8, false, true), nat = tb_NewPtr(8, false, true);
    gaddr mh = qt_movie_handle(mv);
    tb_GetMovieBox(mh, box);
    tb_GetMovieNaturalBoundsRect((int32_t)mh, nat);
    int32_t map[9];
    qtm_identity(map);
    int nw = (int16_t)rd16(nat + 6) - (int16_t)rd16(nat + 2), nh = (int16_t)rd16(nat + 4) - (int16_t)rd16(nat);
    int bw = (int16_t)rd16(box + 6) - (int16_t)rd16(box + 2), bh = (int16_t)rd16(box + 4) - (int16_t)rd16(box);
    if (nw > 0 && nh > 0) {
        map[0] = (int32_t)((int64_t)bw * 65536 / nw);
        map[4] = (int32_t)((int64_t)bh * 65536 / nh);
    }
    map[6] = (int32_t)(int16_t)rd16(box + 2) << 16;
    map[7] = (int32_t)(int16_t)rd16(box) << 16;
    tb_DisposePtr(box);
    tb_DisposePtr(nat);
    concat(m1, map, m2);
    if (matrix)
        put_matrix(matrix, m2);
    return noErr;
}

int16_t tb_GetTrackSoundLocalizationSettings(int32_t theTrack, gaddr settings)
{
    qtm_track *t = track_or_error(theTrack, NULL);
    if (settings)
        wr32(settings, t && t->sound_localization ? copy_handle(t->sound_localization) : 0);
    return t ? noErr : invalidTrack;
}

int16_t tb_SetTrackSoundLocalizationSettings(int32_t theTrack, int32_t settings)
{
    qtm_track *t = track_or_error(theTrack, NULL);
    if (!t)
        return invalidTrack;
    if (t->sound_localization)
        tb_DisposeHandle(t->sound_localization);
    t->sound_localization = copy_handle((gaddr)settings);
    return noErr;
}

void tb_SetTrackGWorld(int32_t theTrack, int32_t port, int32_t gdh, int32_t proc, int32_t refCon)
{
    qtm_track *t = track_or_error(theTrack, NULL);
    if (!t)
        return;
    t->port = (gaddr)port;
    t->gdh = (gaddr)gdh;
    t->gworld_proc = proc;
    t->gworld_refcon = refCon;
}

void tb_SetTrackLoadSettings(int32_t theTrack, int32_t preloadTime, int32_t preloadDuration, int32_t preloadFlags,
                             int32_t defaultHints)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    if (!t)
        return;
    t->load_time = preloadTime;
    t->load_duration = preloadDuration;
    t->load_flags = preloadFlags;
    t->load_hints = defaultHints;
    qt_movie_touch(mv);
}

void tb_GetTrackLoadSettings(int32_t theTrack, gaddr preloadTime, gaddr preloadDuration, gaddr preloadFlags,
                             gaddr defaultHints)
{
    qtm_track *t = track_or_error(theTrack, NULL);
    if (preloadTime)
        wr32(preloadTime, t ? (uint32_t)t->load_time : 0);
    if (preloadDuration)
        wr32(preloadDuration, t ? (uint32_t)t->load_duration : 0);
    if (preloadFlags)
        wr32(preloadFlags, t ? (uint32_t)t->load_flags : 0);
    if (defaultHints)
        wr32(defaultHints, t ? (uint32_t)t->load_hints : 0);
}

/* ---- Track references ---- */

static qtm_tref *tref_of(qtm_track *t, uint32_t type, bool make)
{
    for (int i = 0; i < t->ntrefs; i++)
        if (t->trefs[i].type == type)
            return &t->trefs[i];
    if (!make)
        return NULL;
    t->trefs = realloc(t->trefs, (size_t)(t->ntrefs + 1) * sizeof *t->trefs);
    qtm_tref *f = &t->trefs[t->ntrefs++];
    *f = (qtm_tref){ type, NULL, 0 };
    return f;
}

int16_t tb_AddTrackReference(int32_t theTrack, int32_t refTrack, int32_t refType, gaddr addedIndex)
{
    qtm_movie *mv, *mv2;
    qtm_track *t = track_or_error(theTrack, &mv), *r = track_or_error(refTrack, &mv2);
    if (!t || !r)
        return invalidTrack;
    if (mv != mv2)
        return qt_set_error(trackNotInMovie);
    qtm_tref *f = tref_of(t, (uint32_t)refType, true);
    int at = 0;
    while (at < f->n && f->ids[at])
        at++;
    if (at == f->n) {
        f->ids = realloc(f->ids, (size_t)(f->n + 1) * sizeof *f->ids);
        f->n++;
    }
    f->ids[at] = r->id;
    if (addedIndex)
        wr32(addedIndex, (uint32_t)at + 1);
    qt_movie_touch(mv);
    return noErr;
}

int16_t tb_DeleteTrackReference(int32_t theTrack, int32_t refType, int32_t index)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    if (!t)
        return invalidTrack;
    qtm_tref *f = tref_of(t, (uint32_t)refType, false);
    if (!f || index < 1 || index > f->n)
        return qt_set_error(paramErr);
    if (index == f->n) {
        f->n--;
        while (f->n && !f->ids[f->n - 1])
            f->n--;
    } else {
        f->ids[index - 1] = 0; /* the indexes after it stay as they are */
    }
    qt_movie_touch(mv);
    return noErr;
}

int16_t tb_SetTrackReference(int32_t theTrack, int32_t refTrack, int32_t refType, int32_t index)
{
    qtm_movie *mv, *mv2;
    qtm_track *t = track_or_error(theTrack, &mv);
    if (!t)
        return invalidTrack;
    qtm_track *r = refTrack ? track_or_error(refTrack, &mv2) : NULL;
    if (refTrack && (!r || mv2 != mv))
        return qt_set_error(trackNotInMovie);
    qtm_tref *f = tref_of(t, (uint32_t)refType, false);
    if (!f || index < 1 || index > f->n)
        return qt_set_error(paramErr);
    f->ids[index - 1] = r ? r->id : 0;
    qt_movie_touch(mv);
    return noErr;
}

int32_t tb_GetTrackReference(int32_t theTrack, int32_t refType, int32_t index)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    qtm_tref *f = t ? tref_of(t, (uint32_t)refType, false) : NULL;
    if (!f || index < 1 || index > f->n || !f->ids[index - 1])
        return 0;
    qtm_track *r = qtm_track_by_id(mv, f->ids[index - 1]);
    return r ? (int32_t)r->token : 0;
}

int32_t tb_GetNextTrackReferenceType(int32_t theTrack, int32_t refType)
{
    qtm_track *t = track_or_error(theTrack, NULL);
    if (!t)
        return 0;
    bool past = refType == 0;
    for (int i = 0; i < t->ntrefs; i++) {
        if (past && t->trefs[i].n)
            return (int32_t)t->trefs[i].type;
        if (t->trefs[i].type == (uint32_t)refType)
            past = true;
    }
    return 0;
}

int32_t tb_GetTrackReferenceCount(int32_t theTrack, int32_t refType)
{
    qtm_track *t = track_or_error(theTrack, NULL);
    qtm_tref *f = t ? tref_of(t, (uint32_t)refType, false) : NULL;
    return f ? f->n : 0;
}

/* ---- Media ---- */

/* The data reference a new media gets: the one given, else the movie's
 * default, else its own file, else a handle of its own in memory. */
static void first_dref(qtm_movie *mv, qtm_media *md, int32_t dataRef, int32_t dataRefType)
{
    if (dataRef) {
        uint32_t n;
        uint8_t *b = qt_dref_bytes((gaddr)dataRef, &n);
        uint32_t type = dataRefType ? (uint32_t)dataRefType : fourcc("alis");
        const char *self = qt_movie_path(mv);
        char path[PATH_MAX];
        bool is_self = self && type == fourcc("alis") && qt_dref_path(type, b, n, NULL, path, sizeof path) &&
                       !strcmp(path, self);
        qtm_add_dref(md, type, is_self ? 1 : 0, is_self ? NULL : b, is_self ? 0 : n);
        free(b);
    } else if (mv->default_dref) {
        qtm_add_dref(md, mv->default_dref_type, 0, mv->default_dref, mv->default_dref_len);
    } else if (qt_movie_path(mv)) {
        qtm_add_dref(md, fourcc("alis"), 1, NULL, 0);
    } else {
        gaddr h = tb_NewHandle(0, false, false);
        uint8_t ref[4];
        put32(ref, h);
        qtm_add_dref(md, fourcc("hndl"), 0, ref, 4);
    }
}

int32_t tb_NewTrackMedia(int32_t theTrack, int32_t mediaType, int32_t timeScale, int32_t dataRef, int32_t dataRefType)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    if (!t)
        return 0;
    qtm_media *md = qtm_new_media(t, (uint32_t)mediaType, timeScale, mac_now());
    first_dref(mv, md, dataRef, dataRefType);
    qt_movie_touch(mv);
    return (int32_t)md->token;
}

void tb_DisposeTrackMedia(int32_t theMedia)
{
    qtm_track *t;
    qtm_movie *mv;
    qtm_media *md = media_or_error(theMedia, &t, &mv);
    if (!md)
        return;
    if (md->handler)
        components_close(md->handler);
    qtm_free_media(md);
    t->media = NULL;
    t->nedits = 0;
    qt_movie_touch(mv);
}

int32_t tb_GetMediaTrack(int32_t theMedia)
{
    qtm_track *t;
    return media_or_error(theMedia, &t, NULL) ? (int32_t)t->token : 0;
}

int32_t tb_GetMediaCreationTime(int32_t theMedia)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    return md ? (int32_t)md->created : 0;
}

int32_t tb_GetMediaModificationTime(int32_t theMedia)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    return md ? (int32_t)md->modified : 0;
}

int32_t tb_GetMediaTimeScale(int32_t theMedia)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    return md ? md->scale : 0;
}

void tb_SetMediaTimeScale(int32_t theMedia, int32_t timeScale)
{
    qtm_movie *mv;
    qtm_media *md = media_or_error(theMedia, NULL, &mv);
    if (!md || timeScale <= 0)
        return;
    md->scale = timeScale;
    qt_movie_touch(mv);
}

int32_t tb_GetMediaDuration(int32_t theMedia)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    return md ? (int32_t)md->duration : 0;
}

int16_t tb_GetMediaLanguage(int32_t theMedia)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    return md ? md->language : 0;
}

void tb_SetMediaLanguage(int32_t theMedia, int16_t language)
{
    qtm_movie *mv;
    qtm_media *md = media_or_error(theMedia, NULL, &mv);
    if (!md)
        return;
    md->language = language;
    qt_movie_touch(mv);
}

int16_t tb_GetMediaQuality(int32_t theMedia)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    return md ? md->quality : 0;
}

void tb_SetMediaQuality(int32_t theMedia, int16_t quality)
{
    qtm_movie *mv;
    qtm_media *md = media_or_error(theMedia, NULL, &mv);
    if (!md)
        return;
    md->quality = quality;
    qt_movie_touch(mv);
}

int32_t tb_GetMediaUserData(int32_t theMedia)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    if (md && !md->udata)
        md->udata = qt_udata_new();
    return md ? (int32_t)md->udata : 0;
}

int16_t tb_GetMediaInputMap(int32_t theMedia, gaddr inputMap)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    if (inputMap)
        wr32(inputMap, 0);
    if (!md)
        return invalidMedia;
    gaddr h = md->input_map ? copy_handle(md->input_map) : qt_atoms_new();
    if (inputMap)
        wr32(inputMap, h);
    return h ? noErr : memFullErr;
}

int16_t tb_SetMediaInputMap(int32_t theMedia, int32_t inputMap)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    if (!md)
        return invalidMedia;
    if (md->input_map)
        tb_DisposeHandle(md->input_map);
    md->input_map = copy_handle((gaddr)inputMap);
    return noErr;
}

int16_t tb_SetMediaPropertyAtom(int32_t theMedia, int32_t propertyAtom)
{
    qtm_movie *mv;
    qtm_media *md = media_or_error(theMedia, NULL, &mv);
    if (!md)
        return invalidMedia;
    if (md->property_atom)
        tb_DisposeHandle(md->property_atom);
    md->property_atom = copy_handle((gaddr)propertyAtom);
    qt_movie_touch(mv);
    return noErr;
}

int16_t tb_GetMediaPropertyAtom(int32_t theMedia, gaddr propertyAtom)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    if (propertyAtom)
        wr32(propertyAtom, 0);
    if (!md)
        return invalidMedia;
    gaddr h = md->property_atom ? copy_handle(md->property_atom) : qt_atoms_new();
    if (propertyAtom)
        wr32(propertyAtom, h);
    return h ? noErr : memFullErr;
}

void tb_SetMediaPlayHints(int32_t theMedia, int32_t flags, int32_t flagsMask)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    if (md)
        md->play_hints = (md->play_hints & ~flagsMask) | (flags & flagsMask);
}

void tb_GetMediaPlayHints(int32_t theMedia, gaddr flags)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    if (flags)
        wr32(flags, md ? (uint32_t)md->play_hints : 0);
}

int16_t tb_BeginMediaEdits(int32_t theMedia)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    if (!md)
        return invalidMedia;
    md->editing = true;
    return noErr;
}

int16_t tb_EndMediaEdits(int32_t theMedia)
{
    qtm_movie *mv;
    qtm_media *md = media_or_error(theMedia, NULL, &mv);
    if (!md)
        return invalidMedia;
    md->editing = false;
    md->modified = mac_now();
    qt_movie_touch(mv);
    return noErr;
}

/* ---- The media's handler ---- */

/* GetMediaHandler: an instance of the media handler component for its
 * type ('mhlr'), opened on first asking and kept with the media. */
int32_t tb_GetMediaHandler(int32_t theMedia)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    if (!md)
        return 0;
    if (!md->handler) {
        uint32_t c = components_find_next(0, fourcc("mhlr"), md->type, 0);
        int16_t err = noErr;
        md->handler = c ? components_open_component(c, &err) : 0;
        if (md->handler) { /* MediaInitialize(mh, GetMovieCompleteParams *): the media and its setting */
            qtm_track *t;
            qtm_movie *mv;
            qt_media_model((gaddr)theMedia, &t, &mv);
            gaddr g = tb_NewPtr(104, false, true);
            wr16(g, 1);
            wr32(g + 2, qt_movie_handle(mv));
            wr32(g + 6, t->token);
            wr32(g + 10, (uint32_t)theMedia);
            wr32(g + 14, (uint32_t)mv->scale);
            wr32(g + 18, (uint32_t)md->scale);
            wr32(g + 22, (uint32_t)mv->duration);
            wr32(g + 26, (uint32_t)qtm_track_duration(t));
            wr32(g + 30, (uint32_t)md->duration);
            wr32(g + 34, 0x10000);
            wr32(g + 38, (uint32_t)tb_GetMovieTimeBase((int32_t)qt_movie_handle(mv)));
            wr16(g + 42, (uint16_t)t->volume);
            wr32(g + 44, (uint32_t)t->width);
            wr32(g + 48, (uint32_t)t->height);
            put_matrix(g + 52, t->matrix);
            gaddr port = tb_NewPtr(8, false, true);
            tb_GetMovieGWorld((int32_t)qt_movie_handle(mv), port, port + 4);
            wr32(g + 88, rd32(port));
            wr32(g + 92, rd32(port + 4));
            tb_DisposePtr(port);
            wr32(g + 100, md->input_map);
            uint32_t args[] = { g };
            qtc_call_named(md->handler, "MediaInitialize", args);
            tb_DisposePtr(g);
        }
        if (!md->handler)
            qt_set_error(noMediaHandler);
    }
    return (int32_t)md->handler;
}

int16_t tb_SetMediaHandler(int32_t theMedia, int32_t mH)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    if (!md)
        return invalidMedia;
    if (md->handler && md->handler != (gaddr)mH)
        components_close(md->handler);
    /* a Component: an instance of it */
    uint32_t t, s, mf;
    if (components_describe((uint32_t)mH, &t, &s, &mf) && !components_component_of((uint32_t)mH)) {
        int16_t err;
        md->handler = components_open_component((uint32_t)mH, &err);
        return err;
    }
    md->handler = (gaddr)mH;
    return noErr;
}

/* ---- Data references and handlers ---- */

int16_t tb_GetMediaDataRefCount(int32_t theMedia, gaddr count)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    if (count)
        wr16(count, md ? (uint16_t)md->ndrefs : 0);
    return md ? noErr : invalidMedia;
}

int16_t tb_GetMediaDataRef(int32_t theMedia, int16_t index, gaddr dataRef, gaddr dataRefType, gaddr dataRefAttributes)
{
    qtm_movie *mv;
    qtm_media *md = media_or_error(theMedia, NULL, &mv);
    if (!md)
        return invalidMedia;
    if (index < 1 || index > md->ndrefs)
        return qt_set_error(badDataRefIndex);
    qtm_dref *d = &md->drefs[index - 1];
    if (dataRef) {
        gaddr h;
        if ((d->flags & 1) && qt_movie_path(mv))
            h = qt_dref_for_path(qt_movie_path(mv));
        else
            h = qt_dref_handle(d->data, d->len);
        wr32(dataRef, h);
    }
    if (dataRefType)
        wr32(dataRefType, d->type);
    if (dataRefAttributes)
        wr32(dataRefAttributes, (d->flags & 1) ? 1u | d->attributes : d->attributes); /* dataRefSelfReference */
    return noErr;
}

int16_t tb_SetMediaDataRef(int32_t theMedia, int16_t index, int32_t dataRef, int32_t dataRefType)
{
    qtm_movie *mv;
    qtm_media *md = media_or_error(theMedia, NULL, &mv);
    if (!md)
        return invalidMedia;
    if (index < 1 || index > md->ndrefs)
        return qt_set_error(badDataRefIndex);
    qtm_dref *d = &md->drefs[index - 1];
    free(d->data);
    d->data = qt_dref_bytes((gaddr)dataRef, &d->len);
    d->type = (uint32_t)dataRefType;
    d->flags = 0;
    qt_movie_touch(mv);
    return noErr;
}

int16_t tb_SetMediaDataRefAttributes(int32_t theMedia, int16_t index, int32_t dataRefAttributes)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    if (!md)
        return invalidMedia;
    if (index < 1 || index > md->ndrefs)
        return qt_set_error(badDataRefIndex);
    md->drefs[index - 1].attributes = (uint32_t)dataRefAttributes;
    return noErr;
}

int16_t tb_AddMediaDataRef(int32_t theMedia, gaddr index, int32_t dataRef, int32_t dataRefType)
{
    qtm_movie *mv;
    qtm_media *md = media_or_error(theMedia, NULL, &mv);
    if (!md)
        return invalidMedia;
    uint32_t n;
    uint8_t *b = qt_dref_bytes((gaddr)dataRef, &n);
    /* the same reference again is the one already there */
    int at = 0;
    for (int i = 0; i < md->ndrefs && !at; i++)
        if (md->drefs[i].type == (uint32_t)dataRefType && md->drefs[i].len == n && !memcmp(md->drefs[i].data, b, n))
            at = i + 1;
    if (!at)
        at = qtm_add_dref(md, (uint32_t)dataRefType, 0, b, n);
    free(b);
    if (index)
        wr16(index, (uint16_t)at);
    qt_movie_touch(mv);
    return noErr;
}

int16_t tb_SetMediaDefaultDataRefIndex(int32_t theMedia, int16_t index)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    if (!md)
        return invalidMedia;
    if (index < 1 || index > md->ndrefs)
        return qt_set_error(badDataRefIndex);
    md->default_dref = index;
    return noErr;
}

/* The data handler component for a kind of reference. */
static uint32_t data_handler_for(uint32_t type)
{
    return components_find_next(0, fourcc("dhlr"), type, 0);
}

void tb_GetMediaDataHandlerDescription(int32_t theMedia, int16_t index, gaddr dhType, int32_t creatorName,
                                       gaddr creatorManufacturer)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    uint32_t type = md && index >= 1 && index <= md->ndrefs ? md->drefs[index - 1].type : 0;
    if (md && !type)
        qt_set_error(badDataRefIndex);
    if (dhType)
        wr32(dhType, type);
    if (creatorManufacturer)
        wr32(creatorManufacturer, type ? fourcc("appl") : 0);
    if (creatorName) {
        const char *name = type == fourcc("alis") ? "Apple Alias Data Handler"
                         : type == fourcc("hndl") ? "Apple Handle Data Handler"
                         : type == fourcc("ptr ") ? "Apple Pointer Data Handler"
                         : type == fourcc("rsrc") ? "Apple Resource Data Handler"
                         : type == fourcc("url ") ? "Apple URL Data Handler"
                                                  : "";
        size_t n = strlen(name);
        wr8((gaddr)creatorName, (uint8_t)n);
        if (n)
            gput((gaddr)creatorName + 1, name, (uint32_t)n);
    }
}

/* An instance of the data handler for a reference, set to it (and opened
 * for reading, if asked). */
static uint32_t open_handler(uint32_t type, gaddr dataRef, bool read, int16_t *err)
{
    uint32_t c = data_handler_for(type);
    if (!c) {
        *err = noDataHandler;
        return 0;
    }
    uint32_t dh = components_open_component(c, err);
    if (!dh) {
        *err = cantOpenHandler;
        return 0;
    }
    uint32_t args[2] = { dataRef };
    int32_t r = qtc_call_named(dh, "DataHSetDataRef", args);
    if (!r && read)
        r = qtc_call_named(dh, "DataHOpenForRead", args);
    if (r) {
        components_close(dh);
        *err = (int16_t)r;
        return 0;
    }
    *err = noErr;
    return dh;
}

int32_t tb_GetMediaDataHandler(int32_t theMedia, int16_t index)
{
    qtm_movie *mv;
    qtm_media *md = media_or_error(theMedia, NULL, &mv);
    if (!md)
        return 0;
    if (index < 1 || index > md->ndrefs) {
        qt_set_error(badDataRefIndex);
        return 0;
    }
    gaddr ref = 0;
    uint32_t type = 0;
    gaddr out = tb_NewPtr(8, false, true);
    tb_GetMediaDataRef(theMedia, index, out, out + 4, 0);
    ref = rd32(out);
    type = rd32(out + 4);
    tb_DisposePtr(out);
    int16_t err;
    uint32_t dh = open_handler(type, ref, true, &err);
    if (ref)
        tb_DisposeHandle(ref);
    qt_set_error(err);
    return (int32_t)dh;
}

int16_t tb_SetMediaDataHandler(int32_t theMedia, int16_t index, int32_t dataHandler)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    if (!md)
        return invalidMedia;
    if (index < 1 || index > md->ndrefs)
        return qt_set_error(badDataRefIndex);
    (void)dataHandler; /* (ours read the reference themselves) */
    return noErr;
}

int32_t tb_GetDataHandler(int32_t dataRef, int32_t dataHandlerSubType, int32_t flags)
{
    (void)dataRef, (void)flags;
    return (int32_t)data_handler_for((uint32_t)dataHandlerSubType);
}

int16_t tb_OpenADataHandler(int32_t dataRef, int32_t dataHandlerSubType, int32_t anchorDataRef,
                            int32_t anchorDataRefType, int32_t tb, int32_t flags, gaddr dh)
{
    (void)anchorDataRef, (void)anchorDataRefType, (void)tb;
    int16_t err;
    uint32_t h = open_handler((uint32_t)dataHandlerSubType, (gaddr)dataRef, (flags & 1) != 0, &err);
    if (!err && (flags & 8)) { /* kDataHCanWrite */
        uint32_t none[1] = { 0 };
        int32_t r = qtc_call_named(h, "DataHOpenForWrite", none);
        if (r) {
            components_close(h);
            h = 0;
            err = (int16_t)r;
        }
    }
    if (dh)
        wr32(dh, h);
    return qt_set_error(err);
}

int16_t tb_QTNewAlias(gaddr fss, gaddr alias, int8_t minimal)
{
    (void)minimal;
    return minimal ? tb_NewAliasMinimal(fss, alias) : tb_NewAlias(0, fss, alias);
}

/* ---- Sample descriptions ---- */

int32_t tb_GetMediaSampleDescriptionCount(int32_t theMedia)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    return md ? md->ndescs : 0;
}

static int16_t desc_to_handle(const qtm_media *md, int32_t index, gaddr h)
{
    if (index < 1 || index > md->ndescs)
        return invalidSampleDescIndex;
    if (!h)
        return noErr;
    tb_SetHandleSize(h, (int32_t)md->desc_len[index - 1]);
    if (tb_GetHandleSize(h) != (int32_t)md->desc_len[index - 1])
        return memFullErr;
    gput(rd32(h), md->descs[index - 1], md->desc_len[index - 1]);
    return noErr;
}

void tb_GetMediaSampleDescription(int32_t theMedia, int32_t index, int32_t descH)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    if (md)
        qt_set_error(desc_to_handle(md, index, (gaddr)descH));
}

/* A description handle's bytes: its size field says how many. */
static uint8_t *desc_bytes(gaddr h, uint32_t *len)
{
    *len = 0;
    if (!h || !rd32(h))
        return NULL;
    uint32_t hn = (uint32_t)tb_GetHandleSize(h);
    uint32_t n = hn >= 4 ? rd32(rd32(h)) : 0;
    if (n < 16 || n > hn)
        n = hn;
    uint8_t *b = malloc(n ? n : 1);
    gget(b, rd32(h), n);
    *len = n;
    return b;
}

int16_t tb_SetMediaSampleDescription(int32_t theMedia, int32_t index, int32_t descH)
{
    qtm_movie *mv;
    qtm_media *md = media_or_error(theMedia, NULL, &mv);
    if (!md)
        return invalidMedia;
    if (index < 1 || index > md->ndescs)
        return qt_set_error(invalidSampleDescIndex);
    uint32_t n;
    uint8_t *b = desc_bytes((gaddr)descH, &n);
    if (!b)
        return qt_set_error(paramErr);
    free(md->descs[index - 1]);
    md->descs[index - 1] = b;
    md->desc_len[index - 1] = n;
    qt_movie_touch(mv);
    return noErr;
}

/* ---- Samples ---- */

int32_t tb_GetMediaSampleCount(int32_t theMedia)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    return md ? (int32_t)qtm_sample_count(md) : 0;
}

int32_t tb_GetMediaSyncSampleCount(int32_t theMedia)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    return md ? (int32_t)qtm_sync_count(md) : 0;
}

void tb_SampleNumToMediaTime(int32_t theMedia, int32_t logicalSampleNum, gaddr sampleTime, gaddr sampleDuration)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    int64_t start = -1, dur = 0;
    if (md && !qtm_sample_time(md, (uint32_t)logicalSampleNum, &start, &dur)) {
        qt_set_error(invalidSampleNum);
        start = -1;
    }
    if (sampleTime)
        wr32(sampleTime, (uint32_t)start);
    if (sampleDuration)
        wr32(sampleDuration, (uint32_t)dur);
}

void tb_MediaTimeToSampleNum(int32_t theMedia, int32_t time, gaddr sampleNum, gaddr sampleTime, gaddr sampleDuration)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    int64_t start = -1, dur = 0;
    uint32_t n = md ? qtm_sample_at(md, time, &start, &dur) : 0;
    if (md && !n)
        qt_set_error(timeNotInMedia);
    if (sampleNum)
        wr32(sampleNum, n);
    if (sampleTime)
        wr32(sampleTime, n ? (uint32_t)start : (uint32_t)-1);
    if (sampleDuration)
        wr32(sampleDuration, (uint32_t)dur);
}

/* The source new samples go to: the media's default data reference. */
static int16_t write_samples(qtm_movie *mv, qtm_media *md, uint32_t desc, const uint8_t *data, uint32_t n,
                             uint64_t *offset)
{
    int16_t di = qtm_desc_dref(md, desc);
    if (di < 1 || di > md->ndrefs)
        return badDataRefIndex;
    const qtm_dref *d = &md->drefs[di - 1];
    qt_src src;
    if (!qt_src_open(d->type, d->flags, d->data, d->len, qt_movie_path(mv), true, &src))
        return couldNotResolveDataRef;
    int64_t at = qt_src_size(&src);
    bool ok = at >= 0 && qt_src_write(&src, (uint64_t)at, data, n);
    qt_src_close(&src);
    if (!ok)
        return dataNotOpenForWrite;
    *offset = (uint64_t)at;
    return noErr;
}

/* The description for new samples: one the media has, else added, with
 * the default data reference's index in it. */
static int add_description(qtm_media *md, gaddr descH, int16_t *err)
{
    uint32_t n;
    uint8_t *b = desc_bytes(descH, &n);
    *err = noErr;
    if (!b || n < 16) {
        free(b);
        *err = badImageDescription;
        return 0;
    }
    b[14] = (uint8_t)(md->default_dref >> 8);
    b[15] = (uint8_t)md->default_dref;
    int d = qtm_find_desc(md, b, n);
    if (!d)
        d = qtm_add_desc(md, b, n);
    free(b);
    return d;
}

static void sample_added(qtm_movie *mv, qtm_media *md, int64_t start, gaddr sampleTime)
{
    md->duration = qtm_media_samples_duration(md);
    md->modified = mac_now();
    if (sampleTime)
        wr32(sampleTime, (uint32_t)start);
    qt_movie_touch(mv);
}

int16_t tb_AddMediaSample(int32_t theMedia, int32_t dataIn, int32_t inOffset, int32_t size, int32_t durationPerSample,
                          int32_t sampleDescriptionH, int32_t numberOfSamples, int16_t sampleFlags, gaddr sampleTime)
{
    qtm_movie *mv;
    qtm_media *md = media_or_error(theMedia, NULL, &mv);
    if (sampleTime)
        wr32(sampleTime, (uint32_t)-1);
    if (!md)
        return invalidMedia;
    if (!dataIn || !rd32((gaddr)dataIn) || size < 0 || inOffset < 0 ||
        (int64_t)inOffset + size > tb_GetHandleSize((gaddr)dataIn) || numberOfSamples < 1 || durationPerSample < 0)
        return qt_set_error(paramErr);
    int16_t err;
    uint32_t desc = sampleDescriptionH ? (uint32_t)add_description(md, (gaddr)sampleDescriptionH, &err)
                                       : (uint32_t)md->ndescs;
    if (sampleDescriptionH && err)
        return qt_set_error(err);
    if (!desc)
        return qt_set_error(invalidSampleDescription);
    uint8_t *data = malloc(size ? (size_t)size : 1);
    gget(data, rd32((gaddr)dataIn) + (uint32_t)inOffset, (uint32_t)size);
    uint64_t at = 0;
    err = write_samples(mv, md, desc, data, (uint32_t)size, &at);
    free(data);
    if (err)
        return qt_set_error(err);
    int64_t start = qtm_media_samples_duration(md);
    uint32_t each = (uint32_t)size / (uint32_t)numberOfSamples;
    qtm_add_samples(md, at, (uint64_t)size, (uint32_t)numberOfSamples, each ? each : (uint32_t)size,
                    (uint32_t)durationPerSample, desc, !(sampleFlags & 1), false);
    sample_added(mv, md, start, sampleTime);
    return noErr;
}

int16_t tb_AddMediaSampleReference(int32_t theMedia, int32_t dataOffset, int32_t size, int32_t durationPerSample,
                                   int32_t sampleDescriptionH, int32_t numberOfSamples, int16_t sampleFlags,
                                   gaddr sampleTime)
{
    qtm_movie *mv;
    qtm_media *md = media_or_error(theMedia, NULL, &mv);
    if (sampleTime)
        wr32(sampleTime, (uint32_t)-1);
    if (!md)
        return invalidMedia;
    if (numberOfSamples < 1 || size < 0 || durationPerSample < 0)
        return qt_set_error(paramErr);
    int16_t err;
    uint32_t desc = sampleDescriptionH ? (uint32_t)add_description(md, (gaddr)sampleDescriptionH, &err)
                                       : (uint32_t)md->ndescs;
    if (sampleDescriptionH && err)
        return qt_set_error(err);
    if (!desc)
        return qt_set_error(invalidSampleDescription);
    int64_t start = qtm_media_samples_duration(md);
    uint32_t each = (uint32_t)size / (uint32_t)numberOfSamples;
    qtm_add_samples(md, (uint32_t)dataOffset, (uint64_t)(uint32_t)size, (uint32_t)numberOfSamples,
                    each ? each : (uint32_t)size, (uint32_t)durationPerSample, desc, !(sampleFlags & 1), false);
    sample_added(mv, md, start, sampleTime);
    return noErr;
}

/* SampleReferenceRecord: dataOffset, dataSize, durationPerSample,
 * numberOfSamples (longs), sampleFlags (short): 18 bytes; the 64-bit one
 * has a wide offset first: 22. */
static int16_t add_references(int32_t theMedia, int32_t descH, int32_t count, gaddr refs, gaddr sampleTime, bool wide)
{
    qtm_movie *mv;
    qtm_media *md = media_or_error(theMedia, NULL, &mv);
    if (sampleTime)
        wr32(sampleTime, (uint32_t)-1);
    if (!md)
        return invalidMedia;
    if (count < 0 || (count && !refs))
        return qt_set_error(paramErr);
    int16_t err;
    uint32_t desc = descH ? (uint32_t)add_description(md, (gaddr)descH, &err) : (uint32_t)md->ndescs;
    if (descH && err)
        return qt_set_error(err);
    if (!desc)
        return qt_set_error(invalidSampleDescription);
    int64_t start = qtm_media_samples_duration(md);
    uint32_t rs = wide ? 22 : 18;
    for (int32_t i = 0; i < count; i++) {
        gaddr r = refs + (uint32_t)i * rs;
        uint64_t off = wide ? (uint64_t)rd32(r) << 32 | rd32(r + 4) : rd32(r);
        uint32_t o = wide ? 8 : 4;
        uint32_t size = rd32(r + o), dur = rd32(r + o + 4), n = rd32(r + o + 8);
        uint16_t flags = rd16(r + o + 12);
        if (!n)
            continue;
        qtm_add_samples(md, off, size, n, size / n ? size / n : size, dur, desc, !(flags & 1), false);
    }
    sample_added(mv, md, start, sampleTime);
    return noErr;
}

int16_t tb_AddMediaSampleReferences(int32_t theMedia, int32_t sampleDescriptionH, int32_t numberOfSamples,
                                    int32_t sampleRefs, gaddr sampleTime)
{
    return add_references(theMedia, sampleDescriptionH, numberOfSamples, (gaddr)sampleRefs, sampleTime, false);
}

int16_t tb_AddMediaSampleReferences64(int32_t theMedia, int32_t sampleDescriptionH, int32_t numberOfSamples,
                                      int32_t sampleRefs, gaddr sampleTime)
{
    return add_references(theMedia, sampleDescriptionH, numberOfSamples, (gaddr)sampleRefs, sampleTime, true);
}

/* The samples at a media time: the one holding it, and for sound the
 * rest of its run (up to max), all of one description and contiguous. */
typedef struct {
    uint32_t first, count;   /* sample numbers */
    int64_t start, dur;      /* the first's time; each's duration */
    uint64_t offset, bytes;
    uint32_t desc;
    bool sync;
} sample_span;

static bool span_at(const qtm_media *md, int32_t time, int32_t max, sample_span *s)
{
    memset(s, 0, sizeof *s);
    s->first = qtm_sample_at(md, time, &s->start, &s->dur);
    if (!s->first)
        return false;
    int run;
    uint64_t each;
    if (!qtm_sample_info(md, s->first, &s->offset, &each, &s->desc, &s->sync, &run))
        return false;
    s->count = 1;
    s->bytes = each;
    if (md->type == fourcc("soun") && max > 1) {
        /* the rest of the run from this frame on */
        const qtm_run *r = &md->runs[run];
        uint32_t n = 0;
        for (int i = 0; i < run; i++)
            n += md->runs[i].count;
        uint32_t left = r->count - (s->first - n - 1);
        s->count = left < (uint32_t)max ? left : (uint32_t)max;
        uint64_t k0 = s->first - n - 1;
        s->offset = r->offset + r->bytes * k0 / r->count;
        s->bytes = r->bytes * (k0 + s->count) / r->count - r->bytes * k0 / r->count;
    }
    return true;
}

static bool read_span(qtm_movie *mv, qtm_media *md, const sample_span *s, gaddr dst)
{
    int16_t di = qtm_desc_dref(md, s->desc);
    if (di < 1 || di > md->ndrefs)
        return false;
    const qtm_dref *d = &md->drefs[di - 1];
    qt_src src;
    if (!qt_src_open(d->type, d->flags, d->data, d->len, qt_movie_path(mv), false, &src))
        return false;
    bool ok = qt_src_read_guest(&src, s->offset, dst, (uint32_t)s->bytes);
    qt_src_close(&src);
    return ok;
}

int16_t tb_GetMediaSample(int32_t theMedia, int32_t dataOut, int32_t maxSizeToGrow, gaddr size, int32_t time,
                          gaddr sampleTime, gaddr durationPerSample, int32_t sampleDescriptionH,
                          gaddr sampleDescriptionIndex, int32_t maxNumberOfSamples, gaddr numberOfSamples,
                          gaddr sampleFlags)
{
    qtm_movie *mv;
    qtm_media *md = media_or_error(theMedia, NULL, &mv);
    if (!md)
        return invalidMedia;
    sample_span s;
    if (!span_at(md, time, maxNumberOfSamples > 0 ? maxNumberOfSamples : 1, &s))
        return qt_set_error(timeNotInMedia);
    if (dataOut) {
        if (maxSizeToGrow && s.bytes > (uint64_t)maxSizeToGrow)
            return qt_set_error(maxSizeToGrowTooSmall);
        tb_SetHandleSize((gaddr)dataOut, (int32_t)s.bytes);
        if ((uint64_t)tb_GetHandleSize((gaddr)dataOut) != s.bytes)
            return qt_set_error(memFullErr);
        if (!read_span(mv, md, &s, rd32((gaddr)dataOut)))
            return qt_set_error(couldNotResolveDataRef);
    }
    if (size)
        wr32(size, (uint32_t)s.bytes);
    if (sampleTime)
        wr32(sampleTime, (uint32_t)s.start);
    if (durationPerSample)
        wr32(durationPerSample, (uint32_t)s.dur);
    if (sampleDescriptionH)
        desc_to_handle(md, (int32_t)s.desc, (gaddr)sampleDescriptionH);
    if (sampleDescriptionIndex)
        wr32(sampleDescriptionIndex, s.desc);
    if (numberOfSamples)
        wr32(numberOfSamples, s.count);
    if (sampleFlags)
        wr16(sampleFlags, s.sync ? 0 : 1);
    return noErr;
}

int16_t tb_GetMediaSampleReference(int32_t theMedia, gaddr dataOffset, gaddr size, int32_t time, gaddr sampleTime,
                                   gaddr durationPerSample, int32_t sampleDescriptionH, gaddr sampleDescriptionIndex,
                                   int32_t maxNumberOfSamples, gaddr numberOfSamples, gaddr sampleFlags)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    if (!md)
        return invalidMedia;
    sample_span s;
    if (!span_at(md, time, maxNumberOfSamples > 0 ? maxNumberOfSamples : 1, &s))
        return qt_set_error(timeNotInMedia);
    if (dataOffset)
        wr32(dataOffset, (uint32_t)s.offset);
    if (size)
        wr32(size, (uint32_t)s.bytes);
    if (sampleTime)
        wr32(sampleTime, (uint32_t)s.start);
    if (durationPerSample)
        wr32(durationPerSample, (uint32_t)s.dur);
    if (sampleDescriptionH)
        desc_to_handle(md, (int32_t)s.desc, (gaddr)sampleDescriptionH);
    if (sampleDescriptionIndex)
        wr32(sampleDescriptionIndex, s.desc);
    if (numberOfSamples)
        wr32(numberOfSamples, s.count);
    if (sampleFlags)
        wr16(sampleFlags, s.sync ? 0 : 1);
    return noErr;
}

/* Sample references from a time on, a run at a time, all of one description. */
static int16_t get_references(int32_t theMedia, int32_t time, gaddr sampleTime, int32_t descH, gaddr descIndex,
                              int32_t max, gaddr actual, gaddr refs, bool wide)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    if (actual)
        wr32(actual, 0);
    if (!md)
        return invalidMedia;
    int64_t start, dur;
    uint32_t first = qtm_sample_at(md, time, &start, &dur);
    if (!first)
        return qt_set_error(timeNotInMedia);
    int run;
    uint64_t off, each;
    uint32_t desc;
    bool sync;
    qtm_sample_info(md, first, &off, &each, &desc, &sync, &run);
    if (sampleTime)
        wr32(sampleTime, (uint32_t)start);
    if (descH)
        desc_to_handle(md, (int32_t)desc, (gaddr)descH);
    if (descIndex)
        wr32(descIndex, desc);
    uint32_t n = 0, rs = wide ? 22 : 18;
    uint32_t before = 0;
    for (int i = 0; i < run; i++)
        before += md->runs[i].count;
    for (int i = run; i < md->nruns && (int32_t)n < max && md->runs[i].desc == desc; i++) {
        const qtm_run *r = &md->runs[i];
        uint32_t skip = i == run ? first - before - 1 : 0;
        uint32_t cnt = r->count - skip;
        uint64_t o = r->offset + (r->count ? r->bytes * skip / r->count : 0);
        uint64_t bytes = r->bytes - (r->count ? r->bytes * skip / r->count : 0);
        if (refs) {
            gaddr p = refs + n * rs;
            uint32_t k = 0;
            if (wide) {
                wr32(p, (uint32_t)(o >> 32));
                wr32(p + 4, (uint32_t)o);
                k = 8;
            } else {
                wr32(p, (uint32_t)o);
                k = 4;
            }
            wr32(p + k, md->type == fourcc("soun") ? (uint32_t)bytes : r->size * cnt);
            wr32(p + k + 4, r->duration);
            wr32(p + k + 8, cnt);
            wr16(p + k + 12, r->sync ? 0 : 1);
        }
        n++;
    }
    if (actual)
        wr32(actual, n);
    return noErr;
}

int16_t tb_GetMediaSampleReferences(int32_t theMedia, int32_t time, gaddr sampleTime, int32_t sampleDescriptionH,
                                    gaddr sampleDescriptionIndex, int32_t maxNumberOfEntries,
                                    gaddr actualNumberofEntries, int32_t sampleRefs)
{
    return get_references(theMedia, time, sampleTime, sampleDescriptionH, sampleDescriptionIndex, maxNumberOfEntries,
                          actualNumberofEntries, (gaddr)sampleRefs, false);
}

int16_t tb_GetMediaSampleReferences64(int32_t theMedia, int32_t time, gaddr sampleTime, int32_t sampleDescriptionH,
                                      gaddr sampleDescriptionIndex, int32_t maxNumberOfEntries,
                                      gaddr actualNumberofEntries, int32_t sampleRefs)
{
    return get_references(theMedia, time, sampleTime, sampleDescriptionH, sampleDescriptionIndex, maxNumberOfEntries,
                          actualNumberofEntries, (gaddr)sampleRefs, true);
}

int16_t tb_SetMediaPreferredChunkSize(int32_t theMedia, int32_t maxChunkSize)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    if (!md)
        return invalidMedia;
    md->chunk_size = maxChunkSize;
    return noErr;
}

int16_t tb_GetMediaPreferredChunkSize(int32_t theMedia, gaddr maxChunkSize)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    if (maxChunkSize)
        wr32(maxChunkSize, md ? (uint32_t)md->chunk_size : 0);
    return md ? noErr : invalidMedia;
}

int16_t tb_SetMediaShadowSync(int32_t theMedia, int32_t frameDiffSampleNum, int32_t syncSampleNum)
{
    qtm_movie *mv;
    qtm_media *md = media_or_error(theMedia, NULL, &mv);
    if (!md)
        return invalidMedia;
    uint32_t count = qtm_sample_count(md);
    if (frameDiffSampleNum < 1 || (uint32_t)frameDiffSampleNum > count || syncSampleNum < 1 ||
        (uint32_t)syncSampleNum > count)
        return qt_set_error(invalidSampleNum);
    for (int i = 0; i < md->nshadow; i++)
        if (md->shadow[2 * i] == (uint32_t)frameDiffSampleNum) {
            md->shadow[2 * i + 1] = (uint32_t)syncSampleNum;
            return noErr;
        }
    md->shadow = realloc(md->shadow, (size_t)(md->nshadow + 1) * 2 * sizeof *md->shadow);
    md->shadow[2 * md->nshadow] = (uint32_t)frameDiffSampleNum;
    md->shadow[2 * md->nshadow + 1] = (uint32_t)syncSampleNum;
    md->nshadow++;
    qt_movie_touch(mv);
    return noErr;
}

int16_t tb_GetMediaShadowSync(int32_t theMedia, int32_t frameDiffSampleNum, gaddr syncSampleNum)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    if (syncSampleNum)
        wr32(syncSampleNum, 0);
    if (!md)
        return invalidMedia;
    for (int i = 0; i < md->nshadow; i++)
        if (md->shadow[2 * i] == (uint32_t)frameDiffSampleNum) {
            if (syncSampleNum)
                wr32(syncSampleNum, md->shadow[2 * i + 1]);
            return noErr;
        }
    return noErr;
}

/* ---- Data sizes ---- */

/* The bytes of a media's samples whose times fall in [from, to) (media time). */
static uint64_t media_bytes(const qtm_media *md, int64_t from, int64_t to)
{
    uint64_t total = 0;
    int64_t at = 0;
    for (int i = 0; i < md->nruns; i++) {
        const qtm_run *r = &md->runs[i];
        int64_t span = (int64_t)r->count * r->duration, a = at > from ? at : from, b = at + span < to ? at + span : to;
        if (b > a && span > 0)
            total += (uint64_t)((__int128)r->bytes * (b - a) / span);
        at += span;
    }
    return total;
}

/* A track's bytes over a stretch of movie time: through its edits. */
static uint64_t track_bytes(const qtm_movie *mv, const qtm_track *t, int64_t from, int64_t dur)
{
    if (!t->media)
        return 0;
    uint64_t total = 0;
    int64_t at = 0, to = from + dur;
    for (int i = 0; i < t->nedits; i++) {
        const qtm_edit *e = &t->edits[i];
        int64_t a = at > from ? at : from, b = at + e->duration < to ? at + e->duration : to;
        if (b > a && e->media_time >= 0) {
            double rate = e->rate ? e->rate / 65536.0 : 1.0;
            int64_t ma = e->media_time + (int64_t)((a - at) * rate * t->media->scale / mv->scale);
            int64_t mb = e->media_time + (int64_t)((b - at) * rate * t->media->scale / mv->scale);
            total += media_bytes(t->media, ma, mb > ma ? mb : ma + 1);
        }
        at += e->duration;
    }
    return total;
}

int32_t tb_GetMovieDataSize(int32_t theMovie, int32_t startTime, int32_t duration)
{
    qtm_movie *mv = movie_or_error(theMovie);
    uint64_t n = 0;
    for (int i = 0; mv && i < mv->ntracks; i++)
        n += track_bytes(mv, mv->tracks[i], startTime, duration);
    return (int32_t)(n > 0x7FFFFFFF ? 0x7FFFFFFF : n);
}

int16_t tb_GetMovieDataSize64(int32_t theMovie, int32_t startTime, int32_t duration, gaddr dataSize)
{
    qtm_movie *mv = movie_or_error(theMovie);
    uint64_t n = 0;
    for (int i = 0; mv && i < mv->ntracks; i++)
        n += track_bytes(mv, mv->tracks[i], startTime, duration);
    if (dataSize) {
        wr32(dataSize, (uint32_t)(n >> 32));
        wr32(dataSize + 4, (uint32_t)n);
    }
    return mv ? noErr : invalidMovie;
}

int32_t tb_GetTrackDataSize(int32_t theTrack, int32_t startTime, int32_t duration)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    uint64_t n = t ? track_bytes(mv, t, startTime, duration) : 0;
    return (int32_t)(n > 0x7FFFFFFF ? 0x7FFFFFFF : n);
}

int16_t tb_GetTrackDataSize64(int32_t theTrack, int32_t startTime, int32_t duration, gaddr dataSize)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    uint64_t n = t ? track_bytes(mv, t, startTime, duration) : 0;
    if (dataSize) {
        wr32(dataSize, (uint32_t)(n >> 32));
        wr32(dataSize + 4, (uint32_t)n);
    }
    return t ? noErr : invalidTrack;
}

int32_t tb_GetMediaDataSize(int32_t theMedia, int32_t startTime, int32_t duration)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    uint64_t n = md ? media_bytes(md, startTime, (int64_t)startTime + duration) : 0;
    return (int32_t)(n > 0x7FFFFFFF ? 0x7FFFFFFF : n);
}

int16_t tb_GetMediaDataSize64(int32_t theMedia, int32_t startTime, int32_t duration, gaddr dataSize)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    uint64_t n = md ? media_bytes(md, startTime, (int64_t)startTime + duration) : 0;
    if (dataSize) {
        wr32(dataSize, (uint32_t)(n >> 32));
        wr32(dataSize + 4, (uint32_t)n);
    }
    return md ? noErr : invalidMedia;
}

/* MakeTrackTimeTable / MakeMediaTimeTable: for each step of time, how far
 * into each data reference's data the samples up to then reach. */
static int16_t time_table(const qtm_media *md, const qtm_track *t, int32_t movie_scale, gaddr offsets, int32_t startTime,
                          int32_t endTime, int32_t inc, int16_t first, int16_t last, gaddr skew)
{
    if (inc <= 0 || endTime < startTime)
        return paramErr;
    if (first < 1)
        first = 1;
    if (last < first || last > md->ndrefs)
        last = (int16_t)md->ndrefs;
    int32_t steps = (endTime - startTime) / inc + 1;
    int16_t nrefs = (int16_t)(last - first + 1);
    gaddr h = tb_NewHandle(steps * nrefs * 4, false, true);
    if (!h)
        return memFullErr;
    for (int32_t s = 0; s < steps; s++) {
        int64_t when = startTime + (int64_t)s * inc;
        int64_t mt = t ? qtm_track_to_media(t, when, movie_scale, NULL) : when;
        for (int16_t d = first; d <= last; d++) {
            uint64_t reach = 0;
            int64_t at = 0;
            for (int i = 0; i < md->nruns && mt >= 0; i++) {
                const qtm_run *r = &md->runs[i];
                if (qtm_desc_dref(md, r->desc) != d)
                    continue;
                if (at > mt)
                    break;
                uint64_t end = r->offset + r->bytes;
                if (end > reach)
                    reach = end;
                at += (int64_t)r->count * r->duration;
            }
            wr32(rd32(h) + (uint32_t)((s * nrefs + (d - first)) * 4), (uint32_t)reach);
        }
    }
    if (offsets)
        wr32(offsets, h);
    if (skew)
        wr32(skew, (uint32_t)(nrefs * 4));
    return noErr;
}

int16_t tb_MakeTrackTimeTable(int32_t trackH, gaddr offsets, int32_t startTime, int32_t endTime, int32_t timeIncrement,
                              int16_t firstDataRefIndex, int16_t lastDataRefIndex, gaddr retdataRefSkew)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(trackH, &mv);
    if (!t || !t->media)
        return invalidTrack;
    return qt_set_error(time_table(t->media, t, mv->scale, offsets, startTime, endTime, timeIncrement,
                                   firstDataRefIndex, lastDataRefIndex, retdataRefSkew));
}

int16_t tb_MakeMediaTimeTable(int32_t theMedia, gaddr offsets, int32_t startTime, int32_t endTime, int32_t timeIncrement,
                              int16_t firstDataRefIndex, int16_t lastDataRefIndex, gaddr retdataRefSkew)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    if (!md)
        return invalidMedia;
    return qt_set_error(time_table(md, NULL, 0, offsets, startTime, endTime, timeIncrement, firstDataRefIndex,
                                   lastDataRefIndex, retdataRefSkew));
}

int16_t tb_QTGetDataRefMaxFileOffset(int32_t movieH, int32_t dataRefType, int32_t dataRef, gaddr offset)
{
    qtm_movie *mv = qt_movie_model((gaddr)movieH);
    uint32_t n;
    uint8_t *b = qt_dref_bytes((gaddr)dataRef, &n);
    qt_src src;
    bool ok = qt_src_open((uint32_t)dataRefType, 0, b, n, mv ? qt_movie_path(mv) : NULL, false, &src);
    free(b);
    int64_t size = ok ? qt_src_size(&src) : -1;
    if (ok)
        qt_src_close(&src);
    if (offset)
        wr32(offset, size < 0 ? 0 : (uint32_t)size);
    return size < 0 ? couldNotResolveDataRef : noErr;
}

/* ---- Times worth stopping at ---- */

/* The next (or, going backwards, previous) time in a media of interest:
 * sample starts, sync samples; the duration of what starts there. */
static bool media_next(const qtm_media *md, int16_t flags, int64_t time, bool back, int64_t *it, int64_t *idur)
{
    bool edge = (flags & 0x4000) != 0, want_sync = (flags & 8) != 0;
    if (!(flags & (1 | 8 | 16)))
        return false;
    int64_t at = 0, best = -1, bdur = 0;
    for (int i = 0; i < md->nruns; i++) {
        const qtm_run *r = &md->runs[i];
        for (uint32_t k = 0; k < r->count; k++) {
            int64_t s = at + (int64_t)k * r->duration;
            if (want_sync && !r->sync)
                continue;
            bool ok = back ? (edge ? s <= time : s < time) : (edge ? s >= time : s > time);
            if (ok && (best < 0 || (back ? s > best : s < best))) {
                best = s;
                bdur = r->duration;
            }
            if (!back && best >= 0 && s > best)
                break;
        }
        at += (int64_t)r->count * r->duration;
    }
    if (best < 0)
        return false;
    *it = best;
    *idur = bdur;
    return true;
}

void tb_GetMediaNextInterestingTime(int32_t theMedia, int16_t interestingTimeFlags, int32_t time, int32_t rate,
                                    gaddr interestingTime, gaddr interestingDuration)
{
    qtm_media *md = media_or_error(theMedia, NULL, NULL);
    int64_t it = -1, dur = 0;
    if (md && rate && !media_next(md, interestingTimeFlags, time, rate < 0, &it, &dur))
        it = -1;
    if (interestingTime)
        wr32(interestingTime, (uint32_t)it);
    if (interestingDuration)
        wr32(interestingDuration, (uint32_t)dur);
}

/* In a track: edit boundaries and the media's times seen through the edits. */
static bool track_next(const qtm_movie *mv, const qtm_track *t, int16_t flags, int64_t time, bool back, int64_t *it,
                       int64_t *idur)
{
    bool edge = (flags & 0x4000) != 0;
    int64_t best = -1, bdur = 0, at = 0;
    for (int i = 0; i < t->nedits; i++) {
        const qtm_edit *e = &t->edits[i];
        int64_t ends[2] = { at, at + e->duration };
        if (flags & (2 | 4)) { /* edits */
            for (int k = 0; k < 2; k++) {
                int64_t s = ends[k];
                bool ok = back ? (edge ? s <= time : s < time) : (edge ? s >= time : s > time);
                if (ok && (best < 0 || (back ? s > best : s < best))) {
                    best = s;
                    bdur = k == 0 ? e->duration : 0;
                }
            }
        }
        if ((flags & (1 | 8 | 16)) && t->media && e->media_time >= 0 && e->duration > 0) {
            double rate = e->rate ? e->rate / 65536.0 : 1.0;
            int64_t mfrom = e->media_time, mto = e->media_time + (int64_t)(e->duration * rate * t->media->scale / mv->scale);
            int64_t mtime = mfrom + (int64_t)((time - at) * rate * t->media->scale / mv->scale);
            if (time < at)
                mtime = back ? mfrom - 1 : mfrom - (edge ? 0 : 1);
            if (time > at + e->duration)
                mtime = back ? mto + 1 : mto;
            int64_t m, md;
            if (media_next(t->media, flags | (back ? 0 : 0), mtime, back, &m, &md) && m >= mfrom && m < mto) {
                int64_t s = at + (int64_t)((m - mfrom) * mv->scale / t->media->scale / rate);
                bool ok = back ? (edge ? s <= time : s < time) : (edge ? s >= time : s > time);
                if (ok && (best < 0 || (back ? s > best : s < best))) {
                    best = s;
                    bdur = (int64_t)(md * mv->scale / t->media->scale / rate);
                }
            }
        }
        at += e->duration;
    }
    if (best < 0)
        return false;
    *it = best;
    *idur = bdur;
    return true;
}

void tb_GetTrackNextInterestingTime(int32_t theTrack, int16_t interestingTimeFlags, int32_t time, int32_t rate,
                                    gaddr interestingTime, gaddr interestingDuration)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    int64_t it = -1, dur = 0;
    if (t && rate && !track_next(mv, t, interestingTimeFlags, time, rate < 0, &it, &dur))
        it = -1;
    if (interestingTime)
        wr32(interestingTime, (uint32_t)it);
    if (interestingDuration)
        wr32(interestingDuration, (uint32_t)dur);
}

void tb_GetMovieNextInterestingTime(int32_t theMovie, int16_t interestingTimeFlags, int16_t numMediaTypes,
                                    gaddr whichMediaTypes, int32_t time, int32_t rate, gaddr interestingTime,
                                    gaddr interestingDuration)
{
    qtm_movie *mv = movie_or_error(theMovie);
    int64_t best = -1, bdur = 0;
    bool back = rate < 0;
    for (int i = 0; mv && rate && i < mv->ntracks; i++) {
        const qtm_track *t = mv->tracks[i];
        if (!(t->flags & 1))
            continue;
        if (numMediaTypes > 0 && whichMediaTypes) {
            bool match = false;
            for (int k = 0; k < numMediaTypes && !match; k++) {
                uint32_t w = rd32(whichMediaTypes + 4 * (uint32_t)k);
                uint32_t mt = t->media ? t->media->type : 0;
                match = w == mt || qt_media_has_characteristic(mt, w);
            }
            if (!match)
                continue;
        }
        int64_t it, d;
        if (track_next(mv, t, interestingTimeFlags, time, back, &it, &d) &&
            (best < 0 || (back ? it > best : it < best))) {
            best = it;
            bdur = d;
        }
    }
    if (interestingTime)
        wr32(interestingTime, (uint32_t)best);
    if (interestingDuration)
        wr32(interestingDuration, (uint32_t)bdur);
}

int32_t tb_TrackTimeToMediaTime(int32_t value, int32_t theTrack)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    return t ? (int32_t)qtm_track_to_media(t, value, mv->scale, NULL) : -1;
}

int32_t tb_GetTrackEditRate(int32_t theTrack, int32_t atTime)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    int32_t rate = 0;
    if (t && qtm_track_to_media(t, atTime, mv->scale, &rate) < 0) {
        int64_t at = 0;
        for (int i = 0; i < t->nedits; i++) { /* an empty stretch has its own rate */
            if (atTime < at + t->edits[i].duration)
                return t->edits[i].rate;
            at += t->edits[i].duration;
        }
        qt_set_error(timeNotInTrack);
        return 0;
    }
    return rate;
}

/* ---- Edits within a track ---- */

/* One track's edits as qt_edit.c works on them, and back. */
static void one_track(const qtm_movie *mv, const qtm_track *t, qt_edits *e)
{
    memset(e, 0, sizeof *e);
    e->scale = mv->scale;
    e->ntracks = 1;
    e->tracks = calloc(1, sizeof *e->tracks);
    e->tracks[0].movie_scale = mv->scale;
    e->tracks[0].media_scale = t->media ? t->media->scale : mv->scale;
    e->tracks[0].n = t->nedits;
    e->tracks[0].e = malloc((size_t)(t->nedits ? t->nedits : 1) * sizeof(qt_edit));
    for (int i = 0; i < t->nedits; i++)
        e->tracks[0].e[i] = (qt_edit){ t->edits[i].duration, t->edits[i].media_time, t->edits[i].rate };
}

static void back_to_track(qtm_movie *mv, qtm_track *t, const qt_edits *e)
{
    const qt_edit_track *et = &e->tracks[0];
    qtm_edit *n = malloc((size_t)(et->n ? et->n : 1) * sizeof *n);
    for (int i = 0; i < et->n; i++)
        n[i] = (qtm_edit){ et->e[i].len, et->e[i].media_time, et->e[i].rate };
    qtm_set_edits(t, n, et->n);
    free(n);
    t->modified = mac_now();
    qt_movie_touch(mv);
}

/* An edit put into a track's list at a movie time, splitting the one
 * there; past the end, an empty stretch first. */
static void insert_edit(qtm_track *t, int32_t movie_scale, int64_t at, qtm_edit e)
{
    qtm_edit *out = malloc((size_t)(t->nedits + 3) * sizeof *out);
    int n = 0;
    int64_t pos = 0;
    bool placed = false;
    for (int i = 0; i < t->nedits; i++) {
        qtm_edit x = t->edits[i];
        if (!placed && at >= pos && at < pos + x.duration) {
            int64_t k = at - pos;
            if (k > 0) {
                qtm_edit a = x;
                a.duration = k;
                out[n++] = a;
                x.duration -= k;
                if (x.media_time >= 0 && t->media) {
                    double rate = x.rate ? x.rate / 65536.0 : 1.0;
                    x.media_time += (int64_t)((double)k * t->media->scale / movie_scale * rate + 0.5);
                }
            }
            out[n++] = e;
            placed = true;
        }
        out[n++] = x;
        pos += t->edits[i].duration;
    }
    if (!placed) {
        if (at > pos)
            out[n++] = (qtm_edit){ at - pos, -1, 0x10000 };
        out[n++] = e;
    }
    qtm_set_edits(t, out, n);
    free(out);
}

int16_t tb_InsertMediaIntoTrack(int32_t theTrack, int32_t trackStart, int32_t mediaTime, int32_t mediaDuration,
                                int32_t mediaRate)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    if (!t)
        return invalidTrack;
    if (!t->media)
        return qt_set_error(invalidMedia);
    if (mediaTime < -1 || mediaDuration < 0)
        return qt_set_error(invalidTime);
    if (mediaRate <= 0)
        mediaRate = 0x10000;
    int64_t len = (int64_t)((double)mediaDuration * mv->scale / t->media->scale / (mediaRate / 65536.0) + 0.5);
    if (len <= 0)
        return noErr;
    int64_t dur = qtm_track_duration(t);
    if (trackStart < 0)
        trackStart = (int32_t)dur;
    insert_edit(t, mv->scale, trackStart, (qtm_edit){ len, mediaTime, mediaRate });
    t->modified = mac_now();
    qt_movie_touch(mv);
    return noErr;
}

int16_t tb_InsertEmptyTrackSegment(int32_t dstTrack, int32_t dstIn, int32_t dstDuration)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(dstTrack, &mv);
    if (!t)
        return invalidTrack;
    if (dstIn < 0 || dstDuration < 0)
        return qt_set_error(invalidTime);
    qt_edits e;
    one_track(mv, t, &e);
    bool ok = qt_edits_insert_empty(&e, dstIn, dstDuration);
    if (ok)
        back_to_track(mv, t, &e);
    qt_edits_free(&e);
    return qt_set_error(ok ? noErr : memFullErr);
}

int16_t tb_DeleteTrackSegment(int32_t theTrack, int32_t startTime, int32_t duration)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    if (!t)
        return invalidTrack;
    if (startTime < 0 || duration < 0)
        return qt_set_error(invalidTime);
    qt_edits e;
    one_track(mv, t, &e);
    bool ok = qt_edits_delete(&e, startTime, duration);
    if (ok)
        back_to_track(mv, t, &e);
    qt_edits_free(&e);
    return qt_set_error(ok ? noErr : memFullErr);
}

/* A stretch of a track made longer or shorter: its edits there play
 * faster or slower to fit. */
static void scale_edits(qtm_track *t, int64_t start, int64_t old_dur, int64_t new_dur)
{
    if (old_dur <= 0 || new_dur < 0)
        return;
    qtm_edit *out = malloc((size_t)(t->nedits * 3 + 1) * sizeof *out);
    int n = 0;
    int64_t at = 0, end = start + old_dur;
    for (int i = 0; i < t->nedits; i++) {
        qtm_edit e = t->edits[i];
        int64_t a = at, b = at + e.duration;
        double rate = e.rate ? e.rate / 65536.0 : 1.0;
        /* the parts before, inside and after the stretch */
        int64_t cuts[4] = { a, a > start ? a : start < b ? start : b, b < end ? b : end > a ? end : a, b };
        for (int k = 0; k < 3; k++) {
            int64_t x = cuts[k], y = cuts[k + 1];
            if (y <= x)
                continue;
            qtm_edit p = e;
            p.duration = y - x;
            if (e.media_time >= 0)
                p.media_time = e.media_time + (int64_t)((x - a) * rate * 1.0);
            if (k == 1) {
                int64_t d = (int64_t)((double)p.duration * new_dur / old_dur + 0.5);
                if (d > 0 && e.media_time >= 0)
                    p.rate = (int32_t)(e.rate * ((double)p.duration / d));
                p.duration = d;
            }
            if (p.duration > 0)
                out[n++] = p;
        }
        at = b;
    }
    qtm_set_edits(t, out, n);
    free(out);
}

int16_t tb_ScaleTrackSegment(int32_t theTrack, int32_t startTime, int32_t oldDuration, int32_t newDuration)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    if (!t)
        return invalidTrack;
    if (startTime < 0 || oldDuration <= 0 || newDuration < 0)
        return qt_set_error(invalidDuration);
    scale_edits(t, startTime, oldDuration, newDuration);
    qt_movie_touch(mv);
    return noErr;
}

int16_t tb_ScaleMovieSegment(int32_t theMovie, int32_t startTime, int32_t oldDuration, int32_t newDuration)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (!mv)
        return invalidMovie;
    if (startTime < 0 || oldDuration <= 0 || newDuration < 0)
        return qt_set_error(invalidDuration);
    for (int i = 0; i < mv->ntracks; i++)
        scale_edits(mv->tracks[i], startTime, oldDuration, newDuration);
    qt_movie_touch(mv);
    return noErr;
}

/* Another media's samples brought into this one by reference: its data
 * references added, its descriptions and runs copied after ours.  The
 * media time where they start. */
static int64_t adopt_samples(qtm_media *dst, const qtm_media *src, const char *src_path)
{
    int dref_map[256] = { 0 }, desc_map[256] = { 0 };
    for (int i = 0; i < src->ndrefs && i < 256; i++) {
        const qtm_dref *d = &src->drefs[i];
        if ((d->flags & 1) && src_path) { /* the source's own file: by alias now */
            gaddr a = qt_dref_for_path(src_path);
            uint32_t n;
            uint8_t *b = qt_dref_bytes(a, &n);
            dref_map[i] = qtm_add_dref(dst, fourcc("alis"), 0, b, n);
            free(b);
            if (a)
                tb_DisposeHandle(a);
        } else {
            dref_map[i] = qtm_add_dref(dst, d->type, d->flags, d->data, d->len);
        }
    }
    for (int i = 0; i < src->ndescs && i < 256; i++) {
        uint8_t *b = malloc(src->desc_len[i]);
        memcpy(b, src->descs[i], src->desc_len[i]);
        if (src->desc_len[i] >= 16) {
            int16_t old = (int16_t)(b[14] << 8 | b[15]);
            int16_t nw = old >= 1 && old <= 256 ? (int16_t)dref_map[old - 1] : 1;
            b[14] = (uint8_t)(nw >> 8);
            b[15] = (uint8_t)nw;
        }
        int d = qtm_find_desc(dst, b, src->desc_len[i]);
        desc_map[i] = d ? d : qtm_add_desc(dst, b, src->desc_len[i]);
        free(b);
    }
    int64_t start = qtm_media_samples_duration(dst);
    for (int i = 0; i < src->nruns; i++) {
        qtm_run r = src->runs[i];
        r.desc = r.desc >= 1 && r.desc <= 256 ? (uint32_t)desc_map[r.desc - 1] : 1;
        qtm_add_samples(dst, r.offset, r.bytes, r.count, r.size, r.duration, r.desc, r.sync, true);
    }
    dst->duration = qtm_media_samples_duration(dst);
    return start;
}

int16_t tb_InsertTrackSegment(int32_t srcTrack, int32_t dstTrack, int32_t srcIn, int32_t srcDuration, int32_t dstIn)
{
    qtm_movie *smv, *dmv;
    qtm_track *s = track_or_error(srcTrack, &smv), *d = track_or_error(dstTrack, &dmv);
    if (!s || !d)
        return invalidTrack;
    if (!s->media || !d->media)
        return qt_set_error(invalidMedia);
    if (s->media->type != d->media->type)
        return qt_set_error(mediaTypesDontMatch);
    if (srcIn < 0 || srcDuration < 0 || dstIn < 0)
        return qt_set_error(invalidTime);
    /* the source's edits, as times in the destination's media */
    int64_t shift = 0;
    bool same = s->media == d->media;
    if (!same && s->media->scale != d->media->scale)
        return qt_set_error(featureUnsupported);
    if (!same)
        shift = adopt_samples(d->media, s->media, qt_movie_path(smv));
    qt_edits se, de, part;
    one_track(smv, s, &se);
    one_track(dmv, d, &de);
    bool ok = qt_edits_copy(&se, srcIn, srcDuration, &part);
    for (int i = 0; ok && i < part.tracks[0].n; i++) {
        if (part.tracks[0].e[i].media_time >= 0)
            part.tracks[0].e[i].media_time += shift;
        if (smv->scale != dmv->scale)
            part.tracks[0].e[i].len = rescale(part.tracks[0].e[i].len, smv->scale, dmv->scale);
    }
    part.scale = de.scale;
    part.tracks[0].movie_scale = dmv->scale;
    ok = ok && qt_edits_insert(&de, &part, 0, qt_edits_duration(&part), dstIn);
    if (ok)
        back_to_track(dmv, d, &de);
    qt_edits_free(&se);
    qt_edits_free(&de);
    if (ok)
        qt_edits_free(&part);
    return qt_set_error(ok ? noErr : memFullErr);
}

/* A segment of one movie into another of different files or tracks: each
 * source track's segment into the destination track of its media type
 * (a new one if there isn't one). */
int16_t qt_insert_movie_segment(qtm_movie *src, qtm_movie *dst, int64_t src_in, int64_t len, int64_t dst_in)
{
    int16_t err = noErr;
    for (int i = 0; i < src->ntracks && !err; i++) {
        qtm_track *s = src->tracks[i];
        if (!s->media)
            continue;
        qtm_track *d = NULL;
        for (int k = 0; k < dst->ntracks && !d; k++)
            if (dst->tracks[k]->media && dst->tracks[k]->media->type == s->media->type &&
                dst->tracks[k]->media->scale == s->media->scale)
                d = dst->tracks[k];
        if (!d) {
            d = qtm_add_track(dst, s->width, s->height, s->volume, mac_now());
            memcpy(d->matrix, s->matrix, sizeof d->matrix);
            d->layer = s->layer;
            qtm_media *m = qtm_new_media(d, s->media->type, s->media->scale, mac_now());
            snprintf(m->handler_name, sizeof m->handler_name, "%s", s->media->handler_name);
            free(m->header);
            m->header = malloc(s->media->header_len ? s->media->header_len : 1);
            memcpy(m->header, s->media->header, s->media->header_len);
            m->header_len = s->media->header_len;
            if (dst_in > 0)
                qtm_set_edits(d, &(qtm_edit){ dst_in, -1, 0x10000 }, 1);
        }
        qt_movie_touch(dst);
        err = tb_InsertTrackSegment((int32_t)s->token, (int32_t)d->token, (int32_t)src_in, (int32_t)len,
                                    (int32_t)dst_in);
    }
    return err;
}

/* AddMovieSelection: the source's tracks added beside the movie's at its
 * selection (not inserted: nothing moves). */
void tb_AddMovieSelection(int32_t theMovie, int32_t src)
{
    qtm_movie *mv = movie_or_error(theMovie), *s = movie_or_error(src);
    if (!mv || !s)
        return;
    for (int i = 0; i < s->ntracks; i++) {
        gaddr out = tb_NewPtr(4, false, true);
        if (tb_AddClonedTrackToMovie((int32_t)s->tracks[i]->token, theMovie, 2 /* kQTCloneDontCopyEdits */, out) ==
            noErr) {
            qtm_track *t = qt_track_model(rd32(out), NULL);
            if (t)
                qtm_set_edits(t, NULL, 0);
            tb_InsertTrackSegment((int32_t)s->tracks[i]->token, (int32_t)rd32(out), 0, (int32_t)s->duration,
                                  mv->sel_time);
        }
        tb_DisposePtr(out);
    }
    qt_movie_touch(mv);
}

/* ---- Settings copied, tracks added ---- */

static void copy_track_settings(const qtm_track *s, qtm_track *d)
{
    memcpy(d->matrix, s->matrix, sizeof d->matrix);
    d->layer = s->layer;
    d->volume = s->volume;
    d->width = s->width;
    d->height = s->height;
    d->flags = (d->flags & 1) | (s->flags & 0xE);
    if (d->clip)
        tb_DisposeRgn(d->clip);
    d->clip = copy_handle(s->clip);
    free(d->matte_atom);
    d->matte_atom = NULL;
    d->matte_len = 0;
    if (s->matte_atom) {
        d->matte_atom = malloc(s->matte_len);
        memcpy(d->matte_atom, s->matte_atom, s->matte_len);
        d->matte_len = s->matte_len;
    }
    d->load_time = s->load_time;
    d->load_duration = s->load_duration;
    d->load_flags = s->load_flags;
    d->load_hints = s->load_hints;
}

int16_t tb_CopyTrackSettings(int32_t srcTrack, int32_t dstTrack)
{
    qtm_movie *dm;
    qtm_track *s = track_or_error(srcTrack, NULL), *d = track_or_error(dstTrack, &dm);
    if (!s || !d)
        return invalidTrack;
    copy_track_settings(s, d);
    qt_movie_touch(dm);
    return noErr;
}

int16_t tb_CopyMovieSettings(int32_t srcMovie, int32_t dstMovie)
{
    qtm_movie *s = movie_or_error(srcMovie), *d = movie_or_error(dstMovie);
    if (!s || !d)
        return invalidMovie;
    memcpy(d->matrix, s->matrix, sizeof d->matrix);
    d->pref_rate = s->pref_rate;
    d->pref_volume = s->pref_volume;
    d->preview_time = s->preview_time;
    d->preview_duration = s->preview_duration;
    d->poster_time = s->poster_time;
    d->language = s->language;
    if (d->ctab)
        tb_DisposeHandle(d->ctab);
    d->ctab = copy_handle(s->ctab);
    gaddr clip = (gaddr)tb_GetMovieClipRgn(srcMovie);
    tb_SetMovieClipRgn(dstMovie, (int32_t)clip);
    if (clip)
        tb_DisposeRgn(clip);
    qt_movie_touch(d);
    return noErr;
}

static qtm_track *track_like(const qtm_track *s, qtm_movie *dm)
{
    qtm_track *d = qtm_add_track(dm, s->width, s->height, s->volume, mac_now());
    copy_track_settings(s, d);
    d->flags = s->flags;
    d->alternate = s->alternate;
    return d;
}

static qtm_media *media_like(const qtm_media *s, qtm_track *d)
{
    qtm_media *m = qtm_new_media(d, s->type, s->scale, mac_now());
    m->language = s->language;
    m->quality = s->quality;
    snprintf(m->handler_name, sizeof m->handler_name, "%s", s->handler_name);
    free(m->header);
    m->header = malloc(s->header_len ? s->header_len : 1);
    memcpy(m->header, s->header, s->header_len);
    m->header_len = s->header_len;
    return m;
}

int16_t tb_AddEmptyTrackToMovie(int32_t srcTrack, int32_t dstMovie, int32_t dataRef, int32_t dataRefType,
                                gaddr dstTrack)
{
    qtm_track *s = track_or_error(srcTrack, NULL);
    qtm_movie *dm = movie_or_error(dstMovie);
    if (dstTrack)
        wr32(dstTrack, 0);
    if (!s)
        return invalidTrack;
    if (!dm)
        return invalidMovie;
    qtm_track *d = track_like(s, dm);
    if (s->media) {
        qtm_media *m = media_like(s->media, d);
        first_dref(dm, m, dataRef, dataRefType);
    }
    if (dstTrack)
        wr32(dstTrack, d->token);
    qt_movie_touch(dm);
    return noErr;
}

int16_t tb_AddClonedTrackToMovie(int32_t srcTrack, int32_t dstMovie, int32_t flags, gaddr dstTrack)
{
    qtm_movie *sm;
    qtm_track *s = track_or_error(srcTrack, &sm);
    qtm_movie *dm = movie_or_error(dstMovie);
    if (dstTrack)
        wr32(dstTrack, 0);
    if (!s)
        return invalidTrack;
    if (!dm)
        return invalidMovie;
    qtm_track *d = track_like(s, dm);
    if (s->media) {
        qtm_media *m = media_like(s->media, d);
        adopt_samples(m, s->media, qt_movie_path(sm));
        if (!(flags & 2)) /* kQTCloneDontCopyEdits */
            qtm_set_edits(d, s->edits, s->nedits);
    }
    if (dstTrack)
        wr32(dstTrack, d->token);
    qt_movie_touch(dm);
    return noErr;
}

/* ---- Edit states: a movie's or track's edits kept, to go back to ---- */

#define MAX_STATES 64
typedef struct {
    gaddr token;
    gaddr owner;       /* the Movie or Track */
    bool movie;
    int ntracks;
    struct {
        uint32_t id;
        qtm_edit *e;
        int n;
    } *tracks;
    int32_t sel_time, sel_duration;
} edit_state;

static edit_state states[MAX_STATES];

static edit_state *new_state(gaddr owner, bool movie)
{
    for (int i = 0; i < MAX_STATES; i++)
        if (!states[i].token) {
            edit_state *s = &states[i];
            memset(s, 0, sizeof *s);
            s->token = tb_NewHandle(4, false, true);
            s->owner = owner;
            s->movie = movie;
            return s;
        }
    return NULL;
}

static void keep_track_edits(edit_state *s, const qtm_track *t)
{
    s->tracks = realloc(s->tracks, (size_t)(s->ntracks + 1) * sizeof *s->tracks);
    s->tracks[s->ntracks].id = t->id;
    s->tracks[s->ntracks].n = t->nedits;
    s->tracks[s->ntracks].e = malloc((size_t)(t->nedits ? t->nedits : 1) * sizeof(qtm_edit));
    memcpy(s->tracks[s->ntracks].e, t->edits, (size_t)t->nedits * sizeof(qtm_edit));
    s->ntracks++;
}

static edit_state *state_of(gaddr token)
{
    for (int i = 0; token && i < MAX_STATES; i++)
        if (states[i].token == token)
            return &states[i];
    return NULL;
}

static void free_state(edit_state *s)
{
    for (int i = 0; i < s->ntracks; i++)
        free(s->tracks[i].e);
    free(s->tracks);
    tb_DisposeHandle(s->token);
    memset(s, 0, sizeof *s);
}

int32_t tb_NewMovieEditState(int32_t theMovie)
{
    qtm_movie *mv = movie_or_error(theMovie);
    edit_state *s = mv ? new_state((gaddr)theMovie, true) : NULL;
    if (!s)
        return 0;
    for (int i = 0; i < mv->ntracks; i++)
        keep_track_edits(s, mv->tracks[i]);
    s->sel_time = mv->sel_time;
    s->sel_duration = mv->sel_duration;
    return (int32_t)s->token;
}

int16_t tb_UseMovieEditState(int32_t theMovie, int32_t toState)
{
    qtm_movie *mv = movie_or_error(theMovie);
    edit_state *s = state_of((gaddr)toState);
    if (!mv)
        return invalidMovie;
    if (!s || !s->movie)
        return qt_set_error(invalidEditState);
    if (s->owner != (gaddr)theMovie)
        return qt_set_error(nonMatchingEditState);
    for (int i = 0; i < s->ntracks; i++) {
        qtm_track *t = qtm_track_by_id(mv, s->tracks[i].id);
        if (t)
            qtm_set_edits(t, s->tracks[i].e, s->tracks[i].n);
    }
    mv->sel_time = s->sel_time;
    mv->sel_duration = s->sel_duration;
    qt_movie_touch(mv);
    return noErr;
}

int16_t tb_DisposeMovieEditState(int32_t state)
{
    edit_state *s = state_of((gaddr)state);
    if (!s)
        return qt_set_error(invalidEditState);
    free_state(s);
    return noErr;
}

int32_t tb_NewTrackEditState(int32_t theTrack)
{
    qtm_track *t = track_or_error(theTrack, NULL);
    edit_state *s = t ? new_state((gaddr)theTrack, false) : NULL;
    if (!s)
        return 0;
    keep_track_edits(s, t);
    return (int32_t)s->token;
}

int16_t tb_UseTrackEditState(int32_t theTrack, int32_t state)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    edit_state *s = state_of((gaddr)state);
    if (!t)
        return invalidTrack;
    if (!s || s->movie)
        return qt_set_error(invalidEditState);
    if (s->owner != (gaddr)theTrack)
        return qt_set_error(nonMatchingEditState);
    qtm_set_edits(t, s->tracks[0].e, s->tracks[0].n);
    qt_movie_touch(mv);
    return noErr;
}

int16_t tb_DisposeTrackEditState(int32_t state) { return tb_DisposeMovieEditState(state); }

/* ---- Points and regions ---- */

static qrect box_of(int32_t theMovie)
{
    gaddr r = tb_NewPtr(8, false, true);
    tb_GetMovieBox((gaddr)theMovie, r);
    qrect q = qrect_get(r);
    tb_DisposePtr(r);
    return q;
}

static gaddr rect_rgn(qrect q)
{
    gaddr rgn = tb_NewRgn(), r = tb_NewPtr(8, false, false);
    qrect_put(r, q);
    tb_RectRgn(rgn, r);
    tb_DisposePtr(r);
    return rgn;
}

/* A track's box in the movie's display (its bounds through the movie's
 * natural bounds to its box). */
static qrect track_display_rect(int32_t theMovie, const qtm_track *t)
{
    int16_t top, left, bottom, right;
    qtm_track_bounds(t, &top, &left, &bottom, &right);
    qrect box = box_of(theMovie);
    gaddr nr = tb_NewPtr(8, false, true);
    tb_GetMovieNaturalBoundsRect(theMovie, nr);
    qrect nat = qrect_get(nr);
    tb_DisposePtr(nr);
    int nw = nat.right - nat.left, nh = nat.bottom - nat.top, bw = box.right - box.left, bh = box.bottom - box.top;
    if (nw <= 0 || nh <= 0)
        return (qrect){ (int16_t)(box.top + top), (int16_t)(box.left + left), (int16_t)(box.top + bottom),
                        (int16_t)(box.left + right) };
    return (qrect){ (int16_t)(box.top + top * bh / nh), (int16_t)(box.left + left * bw / nw),
                    (int16_t)(box.top + bottom * bh / nh), (int16_t)(box.left + right * bw / nw) };
}

int8_t tb_PtInMovie(int32_t theMovie, int32_t pt)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (!mv)
        return 0;
    int v = (int16_t)(pt >> 16), h = (int16_t)pt;
    for (int i = 0; i < mv->ntracks; i++) {
        const qtm_track *t = mv->tracks[i];
        if (!(t->flags & 1) || !t->width || !t->height)
            continue;
        qrect r = track_display_rect(theMovie, t);
        if (v >= r.top && v < r.bottom && h >= r.left && h < r.right)
            return 1;
    }
    return 0;
}

int8_t tb_PtInTrack(int32_t theTrack, int32_t pt)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    if (!t)
        return 0;
    int v = (int16_t)(pt >> 16), h = (int16_t)pt;
    int16_t top, left, bottom, right;
    qtm_track_bounds(t, &top, &left, &bottom, &right);
    return v >= top && v < bottom && h >= left && h < right;
}

int32_t tb_GetTrackClipRgn(int32_t theTrack)
{
    qtm_track *t = track_or_error(theTrack, NULL);
    return t && t->clip ? (int32_t)copy_handle(t->clip) : 0;
}

void tb_SetTrackClipRgn(int32_t theTrack, int32_t theClip)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    if (!t)
        return;
    if (t->clip)
        tb_DisposeRgn(t->clip);
    t->clip = theClip ? copy_handle((gaddr)theClip) : 0;
    qt_movie_touch(mv);
}

int32_t tb_GetMovieDisplayBoundsRgn(int32_t theMovie)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (!mv)
        return 0;
    gaddr rgn = tb_NewRgn();
    for (int i = 0; i < mv->ntracks; i++) {
        const qtm_track *t = mv->tracks[i];
        if (!(t->flags & 1) || !t->width || !t->height)
            continue;
        gaddr r = rect_rgn(track_display_rect(theMovie, t));
        tb_UnionRgn(rgn, r, rgn);
        tb_DisposeRgn(r);
    }
    gaddr dclip = (gaddr)tb_GetMovieDisplayClipRgn(theMovie);
    if (dclip) {
        tb_SectRgn(rgn, dclip, rgn);
        tb_DisposeRgn(dclip);
    }
    return (int32_t)rgn;
}

int32_t tb_GetTrackDisplayBoundsRgn(int32_t theTrack)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    if (!t)
        return 0;
    return (int32_t)rect_rgn(track_display_rect((int32_t)qt_movie_handle(mv), t));
}

int32_t tb_GetMovieBoundsRgn(int32_t theMovie)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (!mv)
        return 0;
    gaddr rgn = tb_NewRgn();
    for (int i = 0; i < mv->ntracks; i++) {
        const qtm_track *t = mv->tracks[i];
        if (!(t->flags & 1) || !t->width || !t->height)
            continue;
        int16_t top, left, bottom, right;
        qtm_track_bounds(t, &top, &left, &bottom, &right);
        gaddr r = rect_rgn((qrect){ top, left, bottom, right });
        tb_UnionRgn(rgn, r, rgn);
        tb_DisposeRgn(r);
    }
    gaddr clip = (gaddr)tb_GetMovieClipRgn(theMovie);
    if (clip) {
        tb_SectRgn(rgn, clip, rgn);
        tb_DisposeRgn(clip);
    }
    return (int32_t)rgn;
}

int32_t tb_GetTrackMovieBoundsRgn(int32_t theTrack)
{
    qtm_track *t = track_or_error(theTrack, NULL);
    if (!t)
        return 0;
    int16_t top, left, bottom, right;
    qtm_track_bounds(t, &top, &left, &bottom, &right);
    return (int32_t)rect_rgn((qrect){ top, left, bottom, right });
}

int32_t tb_GetTrackBoundsRgn(int32_t theTrack)
{
    qtm_track *t = track_or_error(theTrack, NULL);
    if (!t)
        return 0;
    gaddr rgn = rect_rgn((qrect){ 0, 0, (int16_t)(t->height >> 16), (int16_t)(t->width >> 16) });
    if (t->clip)
        tb_SectRgn(rgn, t->clip, rgn);
    return (int32_t)rgn;
}

/* The tracks showing something in a stretch of time. */
static bool active_in(const qtm_track *t, int64_t time, int64_t dur)
{
    int64_t at = 0;
    for (int i = 0; i < t->nedits; i++) {
        const qtm_edit *e = &t->edits[i];
        if (e->media_time >= 0 && at < time + (dur > 0 ? dur : 1) && at + e->duration > time)
            return true;
        at += e->duration;
    }
    return false;
}

int32_t tb_GetMovieSegmentDisplayBoundsRgn(int32_t theMovie, int32_t time, int32_t duration)
{
    qtm_movie *mv = movie_or_error(theMovie);
    if (!mv)
        return 0;
    gaddr rgn = tb_NewRgn();
    for (int i = 0; i < mv->ntracks; i++) {
        const qtm_track *t = mv->tracks[i];
        if (!(t->flags & 1) || !t->width || !t->height || !active_in(t, time, duration))
            continue;
        gaddr r = rect_rgn(track_display_rect(theMovie, t));
        tb_UnionRgn(rgn, r, rgn);
        tb_DisposeRgn(r);
    }
    return (int32_t)rgn;
}

int32_t tb_GetTrackSegmentDisplayBoundsRgn(int32_t theTrack, int32_t time, int32_t duration)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    if (!t)
        return 0;
    if (!active_in(t, time, duration))
        return (int32_t)tb_NewRgn();
    return (int32_t)rect_rgn(track_display_rect((int32_t)qt_movie_handle(mv), t));
}

int16_t tb_InvalidateMovieRegion(int32_t theMovie, int32_t invalidRgn)
{
    (void)invalidRgn;
    qtm_movie *mv = movie_or_error(theMovie);
    if (!mv)
        return invalidMovie;
    tb_UpdateMovie((gaddr)theMovie);
    return noErr;
}

/* ---- Mattes: a PixMap of ours per track, copies given out ---- */

int32_t tb_GetTrackMatte(int32_t theTrack)
{
    qtm_track *t = track_or_error(theTrack, NULL);
    return t && t->matte ? (int32_t)qt_copy_pixmap(t->matte) : 0;
}

void tb_SetTrackMatte(int32_t theTrack, int32_t theMatte)
{
    qtm_movie *mv;
    qtm_track *t = track_or_error(theTrack, &mv);
    if (!t)
        return;
    if (t->matte)
        qt_dispose_pixmap(t->matte);
    t->matte = theMatte ? qt_copy_pixmap((gaddr)theMatte) : 0;
    free(t->matte_atom);
    t->matte_atom = NULL;
    t->matte_len = 0;
    qt_movie_touch(mv);
}

void tb_DisposeMatte(int32_t theMatte)
{
    if (theMatte)
        qt_dispose_pixmap((gaddr)theMatte);
}

/* ---- Chapters ---- */

static qtm_track *chapter_track(qtm_movie *mv)
{
    for (int i = 0; i < mv->ntracks; i++)
        for (int r = 0; r < mv->tracks[i]->ntrefs; r++)
            if (mv->tracks[i]->trefs[r].type == fourcc("chap") && mv->tracks[i]->trefs[r].n) {
                qtm_track *t = qtm_track_by_id(mv, mv->tracks[i]->trefs[r].ids[0]);
                if (t && t->media && t->media->type == fourcc("text"))
                    return t;
            }
    return NULL;
}

/* A chapter: its sample's text (as a Pascal string) and movie time. */
static bool chapter(qtm_movie *mv, qtm_track *t, uint32_t n, uint8_t name[256], int64_t *when)
{
    qtm_media *md = t->media;
    uint64_t off, bytes;
    uint32_t desc;
    if (!qtm_sample_info(md, n, &off, &bytes, &desc, NULL, NULL))
        return false;
    int64_t st;
    qtm_sample_time(md, n, &st, NULL);
    int16_t di = qtm_desc_dref(md, desc);
    if (di < 1 || di > md->ndrefs)
        return false;
    const qtm_dref *d = &md->drefs[di - 1];
    qt_src src;
    uint8_t b[258] = { 0 };
    uint32_t want = bytes < sizeof b ? (uint32_t)bytes : (uint32_t)sizeof b;
    if (!qt_src_open(d->type, d->flags, d->data, d->len, qt_movie_path(mv), false, &src))
        return false;
    bool ok = want >= 2 && qt_src_read(&src, off, b, want);
    qt_src_close(&src);
    if (!ok)
        return false;
    uint32_t tl = (uint32_t)(b[0] << 8 | b[1]);
    if (tl > want - 2)
        tl = want - 2;
    if (tl > 255)
        tl = 255;
    name[0] = (uint8_t)tl;
    memcpy(name + 1, b + 2, tl);
    /* the media time in the movie, through the first edit that shows it */
    *when = -1;
    int64_t edge = 0;
    for (int e = 0; e < t->nedits && *when < 0; e++) {
        const qtm_edit *x = &t->edits[e];
        int64_t mlen = x->duration * md->scale / mv->scale;
        if (x->media_time >= 0 && st >= x->media_time && st < x->media_time + mlen)
            *when = edge + (st - x->media_time) * mv->scale / md->scale;
        edge += x->duration;
    }
    return *when >= 0;
}

bool qt_ind_chapter(qtm_movie *mv, int32_t index, gaddr time_out, gaddr name_out)
{
    qtm_track *t = chapter_track(mv);
    uint8_t name[256];
    int64_t when;
    if (!t || index < 1 || !chapter(mv, t, (uint32_t)index, name, &when))
        return false;
    if (time_out)
        wr32(time_out, (uint32_t)when);
    if (name_out)
        gput(name_out, name, 1u + name[0]);
    return true;
}

bool qt_chapter_time(qtm_movie *mv, gaddr name, gaddr record)
{
    qtm_track *t = chapter_track(mv);
    if (!t || !name)
        return false;
    uint8_t want[256];
    gget(want, name, 1u + rd8(name));
    uint32_t count = qtm_sample_count(t->media);
    for (uint32_t n = 1; n <= count; n++) {
        uint8_t got[256];
        int64_t when;
        if (chapter(mv, t, n, got, &when) && got[0] == want[0] && !memcmp(got + 1, want + 1, got[0])) {
            if (record) {
                wr32(record, (uint32_t)((uint64_t)when >> 32));
                wr32(record + 4, (uint32_t)when);
                wr32(record + 8, (uint32_t)mv->scale);
                wr32(record + 12, 0);
            }
            return true;
        }
    }
    return false;
}

/* ---- A movie going away ---- */

void qt_movie_forget(qtm_movie *mv)
{
    for (int i = 0; i < mv->ntracks; i++) {
        qtm_track *t = mv->tracks[i];
        if (t->media && t->media->handler)
            components_close(t->media->handler);
        if (t->matte)
            qt_dispose_pixmap(t->matte);
        for (int s = 0; s < MAX_STATES; s++)
            if (states[s].token && states[s].owner == t->token)
                free_state(&states[s]);
    }
    gaddr h = qt_movie_handle(mv);
    for (int s = 0; s < MAX_STATES; s++)
        if (states[s].token && states[s].owner == h)
            free_state(&states[s]);
}

void qt_movies_reset(void) { memset(states, 0, sizeof states); }
