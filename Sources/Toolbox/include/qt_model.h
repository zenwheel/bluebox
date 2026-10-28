/* The Movie Toolbox's movies, tracks and media, and their 'moov' atoms (qt_model.c). */
#ifndef QT_MODEL_H
#define QT_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mem.h"

/* A data reference: where samples are.  As in a 'dref' entry: its type
 * ('alis', 'rsrc', 'url ', 'hndl', 'ptr ', ...), flags (1: this file, the
 * one the movie came from) and its bytes (an alias record, a URL, ...). */
typedef struct {
    uint32_t type;
    uint32_t flags;
    uint8_t *data;
    uint32_t len;
    uint32_t attributes; /* SetMediaDataRefAttributes's */
} qtm_dref;

/* Samples, a run of them at a time: count samples of size bytes each (for
 * sound, frames, whose bytes in the file follow from the format), each
 * lasting duration in the media's time; bytes of data from offset.  A run
 * that starts a chunk says so; a chunk's runs share their description. */
typedef struct {
    uint64_t offset;
    uint64_t bytes;
    uint32_t count, size, duration;
    uint32_t desc;        /* the sample description, 1-based */
    bool sync, chunk;     /* key frames; the run begins a chunk */
    int32_t comp_offset;  /* display offset (ctts), 0 mostly */
} qtm_run;

typedef struct {
    int64_t duration;     /* in the movie's time scale */
    int64_t media_time;   /* in the media's; -1 an empty stretch */
    int32_t rate;         /* Fixed */
} qtm_edit;

typedef struct {
    uint32_t type;        /* 'vide', 'soun', 'text', ... */
    uint32_t handler_type, manufacturer, handler_flags, handler_mask; /* hdlr */
    char handler_name[64];
    gaddr token;          /* the Media the program holds */
    int32_t scale;
    int64_t duration;
    uint32_t created, modified;
    int16_t language, quality;
    qtm_dref *drefs;
    int ndrefs;
    int16_t default_dref; /* where new samples go, 1-based */
    uint8_t **descs;      /* sample descriptions, whole (from their size field) */
    uint32_t *desc_len;
    int ndescs;
    qtm_run *runs;
    int nruns, cap;
    gaddr udata;
    uint8_t *header;      /* the media information header (vmhd, smhd, gmhd...), whole */
    uint32_t header_len;
    uint8_t *extra;       /* minf and mdia atoms of no interest here, kept as they are */
    uint32_t extra_len;
    uint8_t *stbl_extra;  /* and the sample table's */
    uint32_t stbl_extra_len;
    gaddr property_atom;  /* SetMediaPropertyAtom's container (a copy) */
    gaddr input_map;
    int32_t chunk_size;   /* SetMediaPreferredChunkSize's */
    int32_t play_hints;
    gaddr handler;        /* GetMediaHandler's instance, 0 until asked */
    bool editing;         /* between BeginMediaEdits and EndMediaEdits */
    uint32_t *shadow;     /* stsh: frame difference sample, sync sample pairs */
    int nshadow;
} qtm_media;

typedef struct {
    uint32_t type;
    uint32_t *ids;
    int n;
} qtm_tref;

typedef struct {
    gaddr token;          /* the Track the program holds */
    uint32_t id;
    uint32_t flags;       /* tkhd: 1 enabled, 2 in movie, 4 in preview, 8 in poster */
    uint32_t created, modified;
    int16_t layer, alternate, volume;
    int32_t matrix[9];
    int32_t width, height; /* Fixed */
    qtm_edit *edits;
    int nedits;
    qtm_media *media;     /* NULL until NewTrackMedia */
    gaddr udata;
    qtm_tref *trefs;
    int ntrefs;
    gaddr clip, matte;    /* region; matte: a PixMapHandle of ours */
    uint8_t *matte_atom;  /* the 'matt' atom as read */
    uint32_t matte_len;
    int32_t load_time, load_duration, load_flags, load_hints;
    uint8_t *extra;       /* trak atoms of no interest here */
    uint32_t extra_len;
    gaddr port, gdh;      /* SetTrackGWorld's */
    int32_t gworld_proc, gworld_refcon;
    gaddr sound_localization;
} qtm_track;

typedef struct {
    int32_t scale;
    int64_t duration;
    uint32_t created, modified;
    int32_t pref_rate;
    int16_t pref_volume;
    int32_t matrix[9];
    int32_t preview_time, preview_duration, poster_time;
    int32_t sel_time, sel_duration, current_time;
    uint32_t next_track_id;
    qtm_track **tracks;
    int ntracks;
    gaddr udata;
    gaddr ctab;           /* SetMovieColorTable's (a copy) */
    gaddr clip;           /* 'clip' atom's region */
    uint8_t *extra;       /* moov atoms of no interest here */
    uint32_t extra_len;
    uint8_t *self_alias;  /* the alias of the file the movie came from, for 'self' references */
    uint32_t self_alias_len;
    /* what the calls set that the file doesn't keep */
    bool preview_mode, auto_alternates_off;
    int32_t active_start, active_duration; /* -1: the whole movie */
    int32_t play_hints;
    int16_t language;
    gaddr progress_proc;
    int32_t progress_refcon;
    gaddr uncover_proc, cover_proc;
    int32_t cover_refcon;
    gaddr draw_proc;
    int32_t draw_flags, draw_refcon;
    uint32_t default_dref_type, anchor_dref_type;
    uint8_t *default_dref, *anchor_dref;
    uint32_t default_dref_len, anchor_dref_len;
    gaddr property_atom;
    gaddr video_output;
    int16_t poster_box[4];   /* top, left, bottom, right */
    gaddr wired_procs[8];    /* AddMovieExecuteWiredActionsProc's, with their refcons */
    int32_t wired_refcons[8];
} qtm_movie;

/* A movie from a whole 'moov' atom; NULL if it isn't one. */
qtm_movie *qtm_parse(const uint8_t *moov, uint32_t len);
/* An empty movie (NewMovie). */
qtm_movie *qtm_new(int32_t scale, uint32_t now);
void qtm_free(qtm_movie *m);

/* The 'moov' atom (malloc'd).  Chunk offsets are where the runs say,
 * unless virtual: then offset o in data reference d of track t (both
 * 1-based) is t << 48 | d << 40 | o, so one reader can find every sample
 * wherever it is (qt_native plays it so). */
uint8_t *qtm_serialize(const qtm_movie *m, bool virtual_offsets, uint32_t *len);

/* Tracks and media. */
qtm_track *qtm_add_track(qtm_movie *m, int32_t width, int32_t height, int16_t volume, uint32_t now);
void qtm_remove_track(qtm_movie *m, qtm_track *t);
qtm_media *qtm_new_media(qtm_track *t, uint32_t type, int32_t scale, uint32_t now);
void qtm_free_media(qtm_media *md);
qtm_track *qtm_track_by_id(const qtm_movie *m, uint32_t id);

/* Durations: a track's (its edits', in movie time), the movie's (its
 * tracks' longest), a media's (its samples'). */
int64_t qtm_track_duration(const qtm_track *t);
void qtm_update_duration(qtm_movie *m);
int64_t qtm_media_samples_duration(const qtm_media *md);

/* Samples: their number; the sample (1-based) at a media time, with its
 * start and duration; a sample's start and duration; its data's place
 * (offset, bytes, description, data reference index), sync flag. */
uint32_t qtm_sample_count(const qtm_media *md);
uint32_t qtm_sync_count(const qtm_media *md);
uint32_t qtm_sample_at(const qtm_media *md, int64_t t, int64_t *start, int64_t *dur);
bool qtm_sample_time(const qtm_media *md, uint32_t n, int64_t *start, int64_t *dur);
bool qtm_sample_info(const qtm_media *md, uint32_t n, uint64_t *offset, uint64_t *bytes, uint32_t *desc, bool *sync,
                     int *run);
/* Samples added: count of size each (bytes in all), each of duration, at
 * offset in the description's data reference.  A new chunk if asked or
 * if they don't follow the last on. */
void qtm_add_samples(qtm_media *md, uint64_t offset, uint64_t bytes, uint32_t count, uint32_t size, uint32_t duration,
                     uint32_t desc, bool sync, bool new_chunk);
/* A sample description added (whole bytes), its index. */
int qtm_add_desc(qtm_media *md, const uint8_t *d, uint32_t len);
int qtm_find_desc(const qtm_media *md, const uint8_t *d, uint32_t len);
/* A data reference's index for a description (the field after its type and reserved bytes). */
int16_t qtm_desc_dref(const qtm_media *md, uint32_t desc);
int qtm_add_dref(qtm_media *md, uint32_t type, uint32_t flags, const uint8_t *data, uint32_t len);

/* Edits: media time for a track time (-1 for none). */
int64_t qtm_track_to_media(const qtm_track *t, int64_t movie_time, int32_t movie_scale, int32_t *rate);
/* Track edits replaced (the media's time ranges in movie time). */
void qtm_set_edits(qtm_track *t, const qtm_edit *e, int n);

/* The matrix's effect on a point, and a track's box in the movie. */
void qtm_identity(int32_t m[9]);
void qtm_track_bounds(const qtm_track *t, int16_t *top, int16_t *left, int16_t *bottom, int16_t *right);

#endif
