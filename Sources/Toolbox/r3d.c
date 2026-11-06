/*
 * r3d.h's frames and textures as the machine keeps them (saved with it),
 * each standing for the host's (r3d_metal.m, in a slot of the same
 * number, never saved): what a fresh process needs to make the host's
 * again, so that a program drawing with QuickDraw 3D or RAVE hibernates.
 *
 *   a frame      its size; and, hibernated between r3d_begin and r3d_end,
 *                what it had drawn so far (its colours and depths, read
 *                back for the snapshot), which the host's new frame starts
 *                from
 *   a texture    its size and whether it is mipmapped; for the snapshot
 *                its pixels, read back from the host (level 0: the host
 *                makes the others from it again), unless its maker can
 *                make it again from the guest's own memory
 *                (r3d_texture_derived: QuickDraw 3D's texture cache)
 *
 * Resumed, the host's frame or texture is made again the first time it
 * is used, and what the snapshot held of it let go of.
 */
#include <string.h>

#include "r3d.h"
#include "tb_internal.h"

enum { MAX_FRAMES = R3D_MAX_FRAMES, MAX_TEXTURES = R3D_MAX_TEXTURES };

struct r3d_frame {
    int slot;
    int width, height;
    bool begun;          /* r3d_begin'd, r3d_end not yet */
    uint32_t clear_argb; /* r3d_begin's */
    float clear_z;
    uint32_t *color;     /* hibernated mid-frame: what it had drawn */
    float *depth;
};

typedef struct {
    bool live, mipmap, derived;
    int width, height;
    uint32_t *pixels; /* hibernated: level 0 */
} r3d_tex;

static r3d_frame *frames[MAX_FRAMES];
static r3d_tex texs[MAX_TEXTURES];

bool r3d_available(void) { return r3dh_available(); }

/* The host's frame, made again (after resuming) from what was kept. */
static bool host_frame(r3d_frame *f)
{
    if (!f)
        return false;
    if (r3dh_frame_live(f->slot))
        return true;
    if (!r3dh_frame_make(f->slot, f->width, f->height))
        return false;
    if (f->begun) {
        r3dh_begin(f->slot, f->color, f->clear_argb, f->clear_z);
        if (f->depth)
            r3dh_write(f->slot, NULL, f->depth);
    }
    bb_free(f->color);
    bb_free(f->depth);
    f->color = NULL;
    f->depth = NULL;
    return true;
}

static void host_texture(int t)
{
    if (t <= 0 || t >= MAX_TEXTURES || !texs[t].live || r3dh_texture_live(t))
        return;
    if (texs[t].pixels && r3dh_texture_make(t, texs[t].width, texs[t].height, texs[t].pixels, texs[t].mipmap)) {
        bb_free(texs[t].pixels);
        texs[t].pixels = NULL;
    }
}

r3d_frame *r3d_frame_new(int width, int height)
{
    if (!r3dh_available() || width <= 0 || height <= 0 || width > 16384 || height > 16384)
        return NULL;
    int slot = -1;
    for (int i = 0; i < MAX_FRAMES && slot < 0; i++)
        if (!frames[i])
            slot = i;
    if (slot < 0)
        return NULL;
    r3d_frame *f = bb_calloc(1, sizeof *f, BB_TAG(r3d_frame));
    if (!f)
        return NULL;
    f->slot = slot;
    f->width = width;
    f->height = height;
    if (!r3dh_frame_make(slot, width, height)) {
        bb_free(f);
        return NULL;
    }
    frames[slot] = f;
    return f;
}

void r3d_frame_free(r3d_frame *f)
{
    if (!f)
        return;
    r3dh_frame_drop(f->slot);
    frames[f->slot] = NULL;
    bb_free(f->color);
    bb_free(f->depth);
    bb_free(f);
}

int r3d_frame_width(const r3d_frame *f) { return f ? f->width : 0; }
int r3d_frame_height(const r3d_frame *f) { return f ? f->height : 0; }

void r3d_begin(r3d_frame *f, const uint32_t *pixels, uint32_t clear_argb, float clear_z)
{
    if (!host_frame(f))
        return;
    f->begun = true;
    f->clear_argb = clear_argb;
    f->clear_z = clear_z;
    r3dh_begin(f->slot, pixels, clear_argb, clear_z);
}

void r3d_clear(r3d_frame *f, int x0, int y0, int x1, int y1, bool color, uint32_t argb, bool depth, float z)
{
    if (host_frame(f))
        r3dh_clear(f->slot, x0, y0, x1, y1, color, argb, depth, z);
}

void r3d_draw(r3d_frame *f, const r3d_state *s, int primitive, const r3d_vertex *v, int count)
{
    if (!host_frame(f) || count <= 0)
        return;
    host_texture(s->texture);
    r3dh_draw(f->slot, s, primitive, v, count);
}

void r3d_end(r3d_frame *f, uint32_t *pixels)
{
    if (!host_frame(f))
        return;
    f->begun = false;
    r3dh_end(f->slot, pixels);
}

void r3d_read(r3d_frame *f, uint32_t *pixels, float *depth)
{
    if (host_frame(f))
        r3dh_read(f->slot, pixels, depth);
}

void r3d_write(r3d_frame *f, const uint32_t *pixels, const float *depth)
{
    if (host_frame(f))
        r3dh_write(f->slot, pixels, depth);
}

int r3d_texture_new(int width, int height, const uint32_t *pixels, bool mipmap)
{
    if (!r3dh_available() || width <= 0 || height <= 0 || width > 16384 || height > 16384)
        return 0;
    for (int t = 1; t < MAX_TEXTURES; t++)
        if (!texs[t].live) {
            if (!r3dh_texture_make(t, width, height, pixels, mipmap))
                return 0;
            texs[t] = (r3d_tex){ true, mipmap, false, width, height, NULL };
            return t;
        }
    return 0;
}

void r3d_texture_free(int texture)
{
    if (texture <= 0 || texture >= MAX_TEXTURES)
        return;
    r3dh_texture_drop(texture);
    bb_free(texs[texture].pixels);
    memset(&texs[texture], 0, sizeof texs[texture]);
}

void r3d_texture_derived(int texture)
{
    if (texture > 0 && texture < MAX_TEXTURES)
        texs[texture].derived = true;
}

/* ---- Hibernating ---- */

void r3d_before_hibernate(void)
{
    for (int i = 0; i < MAX_FRAMES; i++) {
        r3d_frame *f = frames[i];
        if (!f || !f->begun || !r3dh_frame_live(i) || f->color)
            continue;
        size_t n = (size_t)f->width * (size_t)f->height;
        f->color = bb_malloc(n * 4, BB_BYTES);
        f->depth = bb_malloc(n * 4, BB_BYTES);
        if (f->color && f->depth) {
            r3dh_read(i, f->color, f->depth);
        } else {
            bb_free(f->color);
            bb_free(f->depth);
            f->color = NULL;
            f->depth = NULL;
        }
    }
    for (int t = 1; t < MAX_TEXTURES; t++) {
        r3d_tex *x = &texs[t];
        if (!x->live || x->derived || x->pixels || !r3dh_texture_live(t))
            continue;
        x->pixels = bb_malloc((size_t)x->width * (size_t)x->height * 4, BB_BYTES);
        if (x->pixels)
            r3dh_texture_read(t, x->pixels);
    }
}

/* Saved (the process goes on): what was read back for the snapshot let go
 * of where the host still has it. */
void r3d_after_hibernate(void)
{
    for (int i = 0; i < MAX_FRAMES; i++) {
        r3d_frame *f = frames[i];
        if (f && r3dh_frame_live(i)) {
            bb_free(f->color);
            bb_free(f->depth);
            f->color = NULL;
            f->depth = NULL;
        }
    }
    for (int t = 1; t < MAX_TEXTURES; t++)
        if (texs[t].pixels && r3dh_texture_live(t)) {
            bb_free(texs[t].pixels);
            texs[t].pixels = NULL;
        }
}

/* Resumed: whatever the host has is not this snapshot's (a test resumes in
 * the process that saved), so all of it is made again, on first use. */
void r3d_after_resume(void)
{
    r3dh_drop_all();
    int frames_n = 0, textures_n = 0;
    for (int i = 0; i < MAX_FRAMES; i++)
        frames_n += frames[i] != NULL;
    for (int t = 1; t < MAX_TEXTURES; t++)
        textures_n += texs[t].live;
    if (frames_n || textures_n)
        LOGF(LOG_IDLE, "resume: 3D renderer: %d frame%s, %d texture%s to make again\n", frames_n,
             frames_n == 1 ? "" : "s", textures_n, textures_n == 1 ? "" : "s");
}

/* What the snapshot holds for the renderer, for the log and the tests. */
size_t r3d_saved_bytes(void)
{
    size_t n = 0;
    for (int i = 0; i < MAX_FRAMES; i++)
        if (frames[i] && frames[i]->color)
            n += (size_t)frames[i]->width * (size_t)frames[i]->height * 8;
    for (int t = 1; t < MAX_TEXTURES; t++)
        if (texs[t].pixels)
            n += (size_t)texs[t].width * (size_t)texs[t].height * 4;
    return n;
}
