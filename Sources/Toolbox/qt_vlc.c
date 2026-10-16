/*
 * Movie decoding through libvlc, loaded at run time.
 *
 * Nothing links against VLC: libvlc is opened with dlopen when a movie is
 * first played (BLUEBOX_LIBVLC, then VLC.app, then the usual library
 * paths), so the project builds and runs without it, and movies then play
 * as their bare timing.  libvlc decodes into a buffer of ours (32-bit RGB)
 * and plays the sound itself; the Movie Toolbox draws the latest frame when
 * the application gives it time.
 */
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qt_vlc.h"

typedef struct libvlc_instance_t libvlc_instance_t;
typedef struct libvlc_media_t libvlc_media_t;
typedef struct libvlc_media_player_t libvlc_media_player_t;

typedef void *(*lock_cb)(void *opaque, void **planes);
typedef void (*unlock_cb)(void *opaque, void *picture, void *const *planes);
typedef void (*display_cb)(void *opaque, void *picture);
typedef unsigned (*setup_cb)(void **opaque, char *chroma, unsigned *width, unsigned *height, unsigned *pitches,
                             unsigned *lines);
typedef void (*cleanup_cb)(void *opaque);

static struct {
    libvlc_instance_t *(*new_)(int, const char *const *);
    libvlc_media_t *(*media_new_path)(libvlc_instance_t *, const char *);
    void (*media_add_option)(libvlc_media_t *, const char *);
    void (*media_release)(libvlc_media_t *);
    libvlc_media_player_t *(*player_new_from_media)(libvlc_media_t *);
    void (*player_release)(libvlc_media_player_t *);
    int (*play)(libvlc_media_player_t *);
    void (*set_pause)(libvlc_media_player_t *, int);
    void (*stop)(libvlc_media_player_t *);
    int64_t (*get_time)(libvlc_media_player_t *);
    void (*set_time)(libvlc_media_player_t *, int64_t);
    int (*get_state)(libvlc_media_player_t *);
    int (*set_rate)(libvlc_media_player_t *, float);
    int (*audio_set_volume)(libvlc_media_player_t *, int);
    void (*video_set_callbacks)(libvlc_media_player_t *, lock_cb, unlock_cb, display_cb, void *);
    void (*video_set_format_callbacks)(libvlc_media_player_t *, setup_cb, cleanup_cb);
} vlc;

static libvlc_instance_t *instance;
static int load_state; /* 0 untried, 1 loaded, -1 unavailable */

enum { stateEnded = 6, stateError = 7 };

static void *open_library(void)
{
    const char *env = getenv("BLUEBOX_LIBVLC");
    if (env && *env)
        return dlopen(env, RTLD_NOW | RTLD_LOCAL);
    const char *home = getenv("HOME");
    char path[1024];
    static const char *const app_libs[] = { "/Applications/VLC.app/Contents/MacOS/lib/libvlc.dylib",
                                             "%s/Applications/VLC.app/Contents/MacOS/lib/libvlc.dylib" };
    for (size_t i = 0; i < sizeof app_libs / sizeof app_libs[0]; i++) {
        snprintf(path, sizeof path, app_libs[i], home ? home : "");
        /* libvlc finds libvlccore through VLC's own @rpath: load it first. */
        char core[1024];
        snprintf(core, sizeof core, "%.*s/libvlccore.dylib", (int)(strrchr(path, '/') - path), path);
        dlopen(core, RTLD_NOW | RTLD_GLOBAL);
        void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (h) {
            /* VLC.app keeps its plugins beside the library. */
            char plugins[1024];
            snprintf(plugins, sizeof plugins, "%.*s/plugins", (int)(strstr(path, "/lib/libvlc") - path), path);
            setenv("VLC_PLUGIN_PATH", plugins, 0);
            return h;
        }
    }
    static const char *const names[] = { "libvlc.dylib", "/opt/homebrew/lib/libvlc.dylib", "/usr/local/lib/libvlc.dylib",
                                         "libvlc.so.5", "libvlc.so" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        void *h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (h)
            return h;
    }
    return NULL;
}

bool qt_vlc_available(void)
{
    if (load_state)
        return load_state > 0;
    load_state = -1;
    void *h = open_library();
    if (!h)
        return false;
#define SYM(field, name)                                       \
    do {                                                       \
        *(void **)&vlc.field = dlsym(h, name);                 \
        if (!vlc.field)                                        \
            return false;                                      \
    } while (0)
    SYM(new_, "libvlc_new");
    SYM(media_new_path, "libvlc_media_new_path");
    SYM(media_add_option, "libvlc_media_add_option");
    SYM(media_release, "libvlc_media_release");
    SYM(player_new_from_media, "libvlc_media_player_new_from_media");
    SYM(player_release, "libvlc_media_player_release");
    SYM(play, "libvlc_media_player_play");
    SYM(set_pause, "libvlc_media_player_set_pause");
    SYM(stop, "libvlc_media_player_stop");
    SYM(get_time, "libvlc_media_player_get_time");
    SYM(set_time, "libvlc_media_player_set_time");
    SYM(get_state, "libvlc_media_player_get_state");
    SYM(set_rate, "libvlc_media_player_set_rate");
    SYM(audio_set_volume, "libvlc_audio_set_volume");
    SYM(video_set_callbacks, "libvlc_video_set_callbacks");
    SYM(video_set_format_callbacks, "libvlc_video_set_format_callbacks");
#undef SYM
    static const char *const args[] = { "--quiet", "--no-video-title-show", "--no-stats", "--no-snapshot-preview" };
    instance = vlc.new_((int)(sizeof args / sizeof args[0]), args);
    if (!instance)
        return false;
    load_state = 1;
    return true;
}

/* ---------------------------------------------------------------------- */

struct qt_vlc_player {
    libvlc_media_player_t *mp;
    pthread_mutex_t lock;
    uint8_t *decode;   /* where libvlc writes (BGRX) */
    uint8_t *frame;    /* the last complete frame (RGBA, or indices) */
    unsigned w, h;
    bool want_indices; /* palette indices, not colours, for indexed video */
    bool indexed;      /* frames are indices (one byte each) */
    uint32_t serial;   /* bumped per frame */
    bool started;
};

static unsigned setup(void **opaque, char *chroma, unsigned *width, unsigned *height, unsigned *pitches,
                      unsigned *lines)
{
    qt_vlc_player *p = *opaque;
    /* Palette indices as they are, when asked for and offered. */
    bool indexed = p->want_indices && !memcmp(chroma, "RGBP", 4);
    if (!indexed)
        memcpy(chroma, "RV32", 4);
    unsigned bpp = indexed ? 1 : 4;
    pthread_mutex_lock(&p->lock);
    p->w = *width;
    p->h = *height;
    p->indexed = indexed;
    free(p->decode);
    free(p->frame);
    p->decode = calloc((size_t)p->w * p->h, bpp);
    p->frame = calloc((size_t)p->w * p->h, bpp);
    pthread_mutex_unlock(&p->lock);
    pitches[0] = p->w * bpp;
    lines[0] = p->h;
    return 1;
}

static void cleanup(void *opaque) {}

static void *lock(void *opaque, void **planes)
{
    qt_vlc_player *p = opaque;
    planes[0] = p->decode;
    return NULL;
}

static void unlock(void *opaque, void *picture, void *const *planes) {}

static void display(void *opaque, void *picture)
{
    qt_vlc_player *p = opaque;
    pthread_mutex_lock(&p->lock);
    size_t n = (size_t)p->w * p->h;
    if (p->indexed) {
        memcpy(p->frame, p->decode, n);
        p->serial++;
        pthread_mutex_unlock(&p->lock);
        return;
    }
    const uint8_t *s = p->decode;
    uint8_t *d = p->frame;
    for (size_t i = 0; i < n; i++, s += 4, d += 4) { /* BGRX in memory to RGBA */
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = 255;
    }
    p->serial++;
    pthread_mutex_unlock(&p->lock);
}

qt_vlc_player *qt_vlc_open(const char *path, bool indices)
{
    if (!qt_vlc_available())
        return NULL;
    libvlc_media_t *m = vlc.media_new_path(instance, path);
    if (!m)
        return NULL;
    vlc.media_add_option(m, ":no-video-title-show");
    qt_vlc_player *p = calloc(1, sizeof *p);
    p->want_indices = indices;
    pthread_mutex_init(&p->lock, NULL);
    p->mp = vlc.player_new_from_media(m);
    vlc.media_release(m);
    if (!p->mp) {
        free(p);
        return NULL;
    }
    vlc.video_set_format_callbacks(p->mp, setup, cleanup);
    vlc.video_set_callbacks(p->mp, lock, unlock, display, p);
    return p;
}

void qt_vlc_close(qt_vlc_player *p)
{
    if (!p)
        return;
    vlc.stop(p->mp);
    vlc.player_release(p->mp);
    free(p->decode);
    free(p->frame);
    pthread_mutex_destroy(&p->lock);
    free(p);
}

void qt_vlc_play(qt_vlc_player *p)
{
    if (!p->started) {
        vlc.play(p->mp);
        p->started = true;
    } else {
        vlc.set_pause(p->mp, 0);
    }
}

void qt_vlc_pause(qt_vlc_player *p)
{
    if (p->started)
        vlc.set_pause(p->mp, 1);
}

/* Back to the start: stopped, so the next play begins at 0. */
void qt_vlc_rewind(qt_vlc_player *p)
{
    vlc.stop(p->mp);
    p->started = false;
}

void qt_vlc_seek(qt_vlc_player *p, int64_t ms)
{
    if (!p->started && ms == 0)
        return;
    if (!p->started) { /* libvlc seeks only while playing */
        vlc.play(p->mp);
        vlc.set_pause(p->mp, 1);
        p->started = true;
    }
    vlc.set_time(p->mp, ms);
}

int64_t qt_vlc_time(qt_vlc_player *p)
{
    int64_t t = p->started ? vlc.get_time(p->mp) : 0;
    return t < 0 ? 0 : t;
}

bool qt_vlc_ended(qt_vlc_player *p)
{
    int s = vlc.get_state(p->mp);
    return s == stateEnded || s == stateError;
}

void qt_vlc_set_rate(qt_vlc_player *p, float rate) { vlc.set_rate(p->mp, rate); }
void qt_vlc_set_volume(qt_vlc_player *p, int percent) { vlc.audio_set_volume(p->mp, percent); }

bool qt_vlc_frame(qt_vlc_player *p, uint32_t *serial, uint8_t **rgba, int *w, int *h, bool *indexed)
{
    pthread_mutex_lock(&p->lock);
    bool fresh = p->frame && p->serial != *serial;
    if (fresh) {
        size_t n = (size_t)p->w * p->h * (p->indexed ? 1 : 4);
        *indexed = p->indexed;
        *rgba = realloc(*rgba, n);
        memcpy(*rgba, p->frame, n);
        *w = (int)p->w;
        *h = (int)p->h;
        *serial = p->serial;
    }
    pthread_mutex_unlock(&p->lock);
    return fresh;
}
