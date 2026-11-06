/*
 * Apple's OpenGL for Mac OS 9 (OpenGLLibrary 1.2, with OpenGLUtility's
 * glu calls), on the host's OpenGL: macOS's OpenGL.framework, a legacy
 * (2.1, fixed-function) context per AGL context, current on the emulator
 * thread.  Each gl call goes straight to the host's; what differs is the
 * guest's byte order and its memory:
 *
 *  - Arguments come as the PowerPC calling convention passes them: integers
 *    and pointers in r3 on, floating point in f1 on, each float taking a
 *    general register's slot and each double two, the slots past r10 on
 *    the stack.
 *  - Arrays of shorts, ints, floats and doubles (matrices, vectors, gets,
 *    texture names) are byte-swapped on the way in and out.  Pixels go to
 *    and from guest memory directly, the host told to swap their bytes
 *    (GL_UNPACK_SWAP_BYTES and GL_PACK_SWAP_BYTES are always the opposite
 *    of what the program set).
 *  - Vertex arrays stay the program's; a draw converts the elements it
 *    uses into host arrays, and glArrayElement becomes immediate-mode
 *    calls.  Selection and feedback buffers are the host's, copied back by
 *    glRenderMode.
 *
 * A context renders into a framebuffer object (a colour renderbuffer, and
 * a depth and stencil one when its pixel format has them) the size of its
 * drawable: front and back are the one colour buffer, which AGL copies to
 * the drawable on aglSwapBuffers (and on glFlush when single-buffered).
 * Without a host OpenGL (Linux) there are no gl exports and no
 * OpenGLLibrary.
 *
 * Hibernating: a context's state is the host's, read back into the
 * machine before a snapshot (opengl_hib.c); its display lists the host
 * cannot give back, so each call compiled into one is recorded as it is
 * compiled, and compiled again from the record.  A resumed program's
 * contexts are made again at their first use (rebuild): sharing as they
 * did, their textures and lists, their framebuffers' contents, then their
 * state.  GLU's quadrics are made again with their settings.
 */
#include <stdlib.h>
#include <string.h>

#include "hostgl.h"
#include "tb_protos_gen.h"

#ifdef __APPLE__
#include "hostgl_internal.h"

/* ---------------------------------------------------------------------- */
/* Contexts */

static hostgl_ctx *cur;
enum { MAX_CONTEXTS = 64 };
static hostgl_ctx *contexts[MAX_CONTEXTS]; /* every one made and not destroyed */
BB_HOST static CGLPixelFormatObj pixel_format;
BB_HOST static int available = -1; /* with pixel_format: this process's */

static CGLPixelFormatObj choose_format(bool accelerated)
{
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_Legacy,
        kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8,
        kCGLPFAAllowOfflineRenderers,
        accelerated ? kCGLPFAAccelerated : (CGLPixelFormatAttribute)0,
        (CGLPixelFormatAttribute)0,
    };
    CGLPixelFormatObj pf = NULL;
    GLint n = 0;
    if (CGLChoosePixelFormat(attrs, &pf, &n) != kCGLNoError)
        return NULL;
    return pf;
}

bool hostgl_available(void)
{
    if (available < 0) {
        pixel_format = choose_format(true);
        if (!pixel_format)
            pixel_format = choose_format(false); /* a software renderer, in a virtual machine */
        available = pixel_format != NULL;
    }
    return available;
}

static bool rebuild(hostgl_ctx *h);

/* Make h current for a while (AGL works on contexts that aren't), then
 * put the program's back.  False when h has no host context and none can
 * be made (resumed where the host has no OpenGL). */
static bool use(hostgl_ctx *h)
{
    if (!h->cgl && !rebuild(h))
        return false;
    if (CGLGetCurrentContext() != h->cgl)
        CGLSetCurrentContext(h->cgl);
    return true;
}
static void unuse(void) { CGLSetCurrentContext(cur ? cur->cgl : NULL); }

/* The framebuffer object and its renderbuffers, in h's new host context. */
static void make_framebuffer(hostgl_ctx *h)
{
    glGenFramebuffersEXT(1, &h->fbo);
    glGenRenderbuffersEXT(1, &h->color);
    h->depth = 0;
    if (h->depth_bits || h->stencil_bits)
        glGenRenderbuffersEXT(1, &h->depth);
    glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_TRUE);
    glPixelStorei(GL_PACK_SWAP_BYTES, GL_TRUE);
}

hostgl_ctx *hostgl_create(hostgl_ctx *share, int depth_bits, int stencil_bits)
{
    if (!hostgl_available())
        return NULL;
    int slot = -1;
    for (int i = 0; i < MAX_CONTEXTS && slot < 0; i++)
        if (!contexts[i])
            slot = i;
    if (slot < 0 || (share && !use(share)))
        return NULL;
    hostgl_ctx *h = bb_calloc(1, sizeof *h, BB_TAG(hostgl_ctx));
    if (!h)
        return NULL;
    if (CGLCreateContext(pixel_format, share ? share->cgl : NULL, &h->cgl) != kCGLNoError) {
        bb_free(h);
        unuse();
        return NULL;
    }
    h->group = share ? share->group : bb_calloc(1, sizeof *h->group, BB_TAG(glgroup));
    if (!h->group) {
        CGLDestroyContext(h->cgl);
        bb_free(h);
        unuse();
        return NULL;
    }
    h->group->refs++;
    h->depth_bits = depth_bits;
    h->stencil_bits = stencil_bits;
    h->draw_buffer = h->read_buffer = GL_BACK;
    h->render_mode = GL_RENDER;
    use(h);
    make_framebuffer(h);
    unuse();
    hostgl_resize(h, 1, 1);
    h->sized = false;
    contexts[slot] = h;
    return h;
}

static void free_arrays(arrays *a)
{
    bb_free(a->vertex.host);
    bb_free(a->normal.host);
    bb_free(a->color.host);
    bb_free(a->index.host);
    bb_free(a->edge.host);
    for (int i = 0; i < MAX_UNITS; i++)
        bb_free(a->tex[i].host);
}

static void free_group(glgroup *g)
{
    for (int i = 0; i < g->nlists; i++)
        bb_free(g->lists[i].ops);
    bb_free(g->lists);
    bb_free(g->tex);
    glh_free_textures(g);
    bb_free(g);
}

void hostgl_destroy(hostgl_ctx *h)
{
    if (!h)
        return;
    for (int i = 0; i < MAX_CONTEXTS; i++)
        if (contexts[i] == h)
            contexts[i] = NULL;
    if (cur == h)
        cur = NULL;
    if (h->cgl) {
        use(h);
        glDeleteFramebuffersEXT(1, &h->fbo);
        glDeleteRenderbuffersEXT(1, &h->color);
        if (h->depth)
            glDeleteRenderbuffersEXT(1, &h->depth);
        unuse();
        CGLDestroyContext(h->cgl);
    }
    if (--h->group->refs <= 0)
        free_group(h->group);
    glh_free(h);
    free_arrays(&h->a);
    bb_free(h->compiling.ops);
    bb_free(h->select);
    bb_free(h->feedback);
    bb_free(h);
}

void hostgl_set_current(hostgl_ctx *h)
{
    cur = h;
    CGLSetCurrentContext(h ? h->cgl : NULL); /* made again (resumed) at its first call */
}

void hostgl_resize(hostgl_ctx *h, int width, int height)
{
    if (width < 1)
        width = 1;
    if (height < 1)
        height = 1;
    if (width > 4096)
        width = 4096;
    if (height > 4096)
        height = 4096;
    if (h->sized && width == h->width && height == h->height)
        return;
    if (!use(h))
        return;
    GLint rb = 0;
    glGetIntegerv(GL_RENDERBUFFER_BINDING_EXT, &rb);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, h->fbo);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, h->color);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, width, height);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, h->color);
    if (h->depth) {
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, h->depth);
        glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, h->stencil_bits ? GL_DEPTH24_STENCIL8_EXT : GL_DEPTH_COMPONENT24,
                                 width, height);
        if (h->depth_bits)
            glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, h->depth);
        if (h->stencil_bits)
            glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, h->depth);
    }
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, (GLuint)rb);
    glDrawBuffer(h->draw_buffer == GL_NONE ? GL_NONE : GL_COLOR_ATTACHMENT0_EXT);
    glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
    if (!h->sized) {
        glViewport(0, 0, width, height);
        glScissor(0, 0, width, height);
    }
    h->width = width;
    h->height = height;
    h->sized = true;
    unuse();
}

static uint8_t *frame;
static size_t frame_cap;

void hostgl_read_frame(hostgl_ctx *h, int x, int y, int width, int height, gaddr dst, uint32_t row_bytes)
{
    if (width <= 0 || height <= 0)
        return;
    size_t need = (size_t)width * (size_t)height * 4;
    if (need > frame_cap) {
        uint8_t *n = bb_realloc(frame, need, BB_BYTES);
        if (!n)
            return;
        frame = n;
        frame_cap = need;
    }
    if (!use(h))
        return;
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_SWAP_BYTES, GL_TRUE);
    glPixelStorei(GL_PACK_LSB_FIRST, GL_FALSE);
    /* A, R, G, B in memory: the guest's 32-bit xRGB. */
    glReadPixels(x, y, width, height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, frame);
    glPopClientAttrib();
    unuse();
    for (int r = 0; r < height; r++)
        memcpy(gptr(dst + (uint32_t)r * row_bytes), frame + (size_t)(height - 1 - r) * (size_t)width * 4,
               (size_t)width * 4);
}

void hostgl_copy_state(hostgl_ctx *src, hostgl_ctx *dst, uint32_t mask)
{
    if (src && dst && use(src) && use(dst))
        CGLCopyContext(src->cgl, dst->cgl, mask);
    unuse();
}

static void glyph_recorded(uint32_t list, int width, int height, int row_bytes, float xorig, float yorig, float xmove,
                           const uint8_t *bits);

void hostgl_glyph_list(uint32_t list, int width, int height, int row_bytes, float xorig, float yorig, float xmove,
                       const uint8_t *bits)
{
    if (!cur || !use(cur))
        return;
    uint8_t *flipped = bb_malloc((size_t)(row_bytes * height) + 1, BB_BYTES);
    if (!flipped)
        return;
    for (int r = 0; r < height; r++)
        memcpy(flipped + r * row_bytes, bits + (height - 1 - r) * row_bytes, (size_t)row_bytes);
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, row_bytes * 8);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
    glNewList(list, GL_COMPILE);
    glBitmap(width, height, xorig, yorig, xmove, 0, flipped);
    glEndList();
    glPopClientAttrib();
    glyph_recorded(list, width, height, row_bytes, xorig, yorig, xmove, flipped);
    bb_free(flipped);
}

/* ---------------------------------------------------------------------- */
/* Arguments and guest memory */

static uint32_t slot(ppc_cpu *c, int s) { return s < 8 ? c->gpr[3 + s] : rd32(c->gpr[1] + 24 + 4 * (uint32_t)s); }

/* The arguments of a call by its signature: i an integer, enum or pointer
 * (into iv), f a float and d a double (into fv). */
static void args_get(ppc_cpu *c, const char *sig, uint32_t *iv, double *fv)
{
    int g = 0, f = 0;
    for (int k = 0; sig[k]; k++) {
        if (sig[k] == 'f' || sig[k] == 'd') {
            fv[k] = ppc_fpr(c, 1 + f++);
            g += sig[k] == 'd' ? 2 : 1;
        } else {
            iv[k] = slot(c, g++);
        }
    }
}

static double get_double(gaddr a)
{
    uint64_t b = (uint64_t)rd32(a) << 32 | rd32(a + 4);
    double d;
    memcpy(&d, &b, 8);
    return d;
}
static void put_double(gaddr a, double d)
{
    uint64_t b;
    memcpy(&b, &d, 8);
    wr32(a, (uint32_t)(b >> 32));
    wr32(a + 4, (uint32_t)b);
}
static float get_float(gaddr a)
{
    uint32_t b = rd32(a);
    float f;
    memcpy(&f, &b, 4);
    return f;
}
static void put_float(gaddr a, float f)
{
    uint32_t b;
    memcpy(&b, &f, 4);
    wr32(a, b);
}

/* n values of `size` bytes each from guest memory, in host order. */
static void gread(void *dst, gaddr src, int n, size_t size)
{
    const uint8_t *s = gptr(src);
    switch (size) {
    case 1: memcpy(dst, s, (size_t)n); break;
    case 2:
        for (int i = 0; i < n; i++) {
            uint16_t v;
            memcpy(&v, s + 2 * i, 2);
            v = __builtin_bswap16(v);
            memcpy((uint8_t *)dst + 2 * i, &v, 2);
        }
        break;
    case 4:
        for (int i = 0; i < n; i++) {
            uint32_t v;
            memcpy(&v, s + 4 * i, 4);
            v = __builtin_bswap32(v);
            memcpy((uint8_t *)dst + 4 * i, &v, 4);
        }
        break;
    case 8:
        for (int i = 0; i < n; i++) {
            uint64_t v;
            memcpy(&v, s + 8 * i, 8);
            v = __builtin_bswap64(v);
            memcpy((uint8_t *)dst + 8 * i, &v, 8);
        }
        break;
    }
}
/* And back. */
static void gwrite(gaddr dst, const void *src, int n, size_t size)
{
    uint8_t *d = gptr(dst);
    for (int i = 0; i < n; i++)
        for (size_t b = 0; b < size; b++)
            d[(size_t)i * size + b] = ((const uint8_t *)src)[(size_t)i * size + size - 1 - b];
}

static void *host_ptr(gaddr a) { return a ? gptr(a) : NULL; }

/* A buffer of at least n bytes, kept for the next call. */
static void *scratch(size_t n)
{
    static void *buf;
    static size_t cap;
    if (n > cap) {
        void *p = bb_realloc(buf, n, BB_BYTES);
        if (!p)
            return NULL;
        buf = p;
        cap = n;
    }
    return buf;
}

/* A string in guest memory, made once (the system heap's). */
static gaddr guest_string(gaddr *slot_, const char *s)
{
    if (!*slot_) {
        size_t n = strlen(s) + 1;
        *slot_ = tb_NewPtr((int32_t)n, true, false);
        if (*slot_)
            gput(*slot_, s, (uint32_t)n);
    }
    return *slot_;
}

/* ---------------------------------------------------------------------- */
/* The calls */

#define ENTER()                                                                                                       \
    do {                                                                                                              \
        if (!cur) {                                                                                                   \
            c->gpr[3] = 0;                                                                                            \
            c->pc = c->lr;                                                                                            \
            return;                                                                                                   \
        }                                                                                                             \
        if (!use(cur)) {                                                                                              \
            c->gpr[3] = 0;                                                                                            \
            c->pc = c->lr;                                                                                            \
            return;                                                                                                   \
        }                                                                                                             \
    } while (0)
#define DONE() (c->pc = c->lr)
#define I(k) ((GLint)iv[k])
#define U(k) ((GLuint)iv[k])
#define F(k) ((GLfloat)fv[k])
#define D(k) (fv[k])
#define P(k) host_ptr(iv[k])

/* X(name, signature, the host call): calls with nothing to convert. */
#define DEFINE(name, sig, call)                                                                                       \
    static void w_##name(ppc_cpu *c)                                                                                  \
    {                                                                                                                 \
        uint32_t iv[16];                                                                                              \
        double fv[16];                                                                                                \
        ENTER();                                                                                                      \
        args_get(c, sig, iv, fv);                                                                                     \
        hook(OP_##name, sig, iv, fv);                                                                                 \
        call;                                                                                                         \
        DONE();                                                                                                       \
    }
/* XV(name, n, type): a vector of n; XT the same after a texture unit;
 * XR glRect's two corners. */
#define DEFINE_V(name, n, T)                                                                                          \
    static void w_##name(ppc_cpu *c)                                                                                  \
    {                                                                                                                 \
        ENTER();                                                                                                      \
        T t[4];                                                                                                       \
        gread(t, c->gpr[3], n, sizeof(T));                                                                            \
        if (cur->recording)                                                                                           \
            record(OP_##name, NULL, 0, t, n * sizeof(T));                                                             \
        name(t);                                                                                                      \
        DONE();                                                                                                       \
    }
#define DEFINE_T(name, host, n, T)                                                                                    \
    static void w_##name(ppc_cpu *c)                                                                                  \
    {                                                                                                                 \
        ENTER();                                                                                                      \
        T t[4];                                                                                                       \
        gread(t, c->gpr[4], n, sizeof(T));                                                                            \
        if (cur->recording)                                                                                           \
            record(OP_##name, &c->gpr[3], 1, t, n * sizeof(T));                                                       \
        host((GLenum)c->gpr[3], t);                                                                                   \
        DONE();                                                                                                       \
    }
#define DEFINE_R(name, T)                                                                                             \
    static void w_##name(ppc_cpu *c)                                                                                  \
    {                                                                                                                 \
        ENTER();                                                                                                      \
        T ab[4];                                                                                                      \
        gread(ab, c->gpr[3], 2, sizeof(T));                                                                           \
        gread(ab + 2, c->gpr[4], 2, sizeof(T));                                                                       \
        if (cur->recording)                                                                                           \
            record(OP_##name, NULL, 0, ab, sizeof ab);                                                                \
        name(ab, ab + 2);                                                                                             \
        DONE();                                                                                                       \
    }
#define EXPORT(name, ...) { #name, w_##name },
#define EXPORT_V(name, ...) { #name, w_##name },

/* Vertices, colours, normals, texture coordinates, raster positions. */
#define GL_IMMEDIATE(X) \
    X(glVertex2d, "dd", glVertex2d(D(0), D(1))) \
    X(glVertex2f, "ff", glVertex2f(F(0), F(1))) \
    X(glVertex2i, "ii", glVertex2i(I(0), I(1))) \
    X(glVertex2s, "ii", glVertex2s((GLshort)I(0), (GLshort)I(1))) \
    X(glVertex3d, "ddd", glVertex3d(D(0), D(1), D(2))) \
    X(glVertex3f, "fff", glVertex3f(F(0), F(1), F(2))) \
    X(glVertex3i, "iii", glVertex3i(I(0), I(1), I(2))) \
    X(glVertex3s, "iii", glVertex3s((GLshort)I(0), (GLshort)I(1), (GLshort)I(2))) \
    X(glVertex4d, "dddd", glVertex4d(D(0), D(1), D(2), D(3))) \
    X(glVertex4f, "ffff", glVertex4f(F(0), F(1), F(2), F(3))) \
    X(glVertex4i, "iiii", glVertex4i(I(0), I(1), I(2), I(3))) \
    X(glVertex4s, "iiii", glVertex4s((GLshort)I(0), (GLshort)I(1), (GLshort)I(2), (GLshort)I(3))) \
    X(glTexCoord1d, "d", glTexCoord1d(D(0))) \
    X(glTexCoord1f, "f", glTexCoord1f(F(0))) \
    X(glTexCoord1i, "i", glTexCoord1i(I(0))) \
    X(glTexCoord1s, "i", glTexCoord1s((GLshort)I(0))) \
    X(glTexCoord2d, "dd", glTexCoord2d(D(0), D(1))) \
    X(glTexCoord2f, "ff", glTexCoord2f(F(0), F(1))) \
    X(glTexCoord2i, "ii", glTexCoord2i(I(0), I(1))) \
    X(glTexCoord2s, "ii", glTexCoord2s((GLshort)I(0), (GLshort)I(1))) \
    X(glTexCoord3d, "ddd", glTexCoord3d(D(0), D(1), D(2))) \
    X(glTexCoord3f, "fff", glTexCoord3f(F(0), F(1), F(2))) \
    X(glTexCoord3i, "iii", glTexCoord3i(I(0), I(1), I(2))) \
    X(glTexCoord3s, "iii", glTexCoord3s((GLshort)I(0), (GLshort)I(1), (GLshort)I(2))) \
    X(glTexCoord4d, "dddd", glTexCoord4d(D(0), D(1), D(2), D(3))) \
    X(glTexCoord4f, "ffff", glTexCoord4f(F(0), F(1), F(2), F(3))) \
    X(glTexCoord4i, "iiii", glTexCoord4i(I(0), I(1), I(2), I(3))) \
    X(glTexCoord4s, "iiii", glTexCoord4s((GLshort)I(0), (GLshort)I(1), (GLshort)I(2), (GLshort)I(3))) \
    X(glColor3b, "iii", glColor3b((GLbyte)I(0), (GLbyte)I(1), (GLbyte)I(2))) \
    X(glColor3d, "ddd", glColor3d(D(0), D(1), D(2))) \
    X(glColor3f, "fff", glColor3f(F(0), F(1), F(2))) \
    X(glColor3i, "iii", glColor3i(I(0), I(1), I(2))) \
    X(glColor3s, "iii", glColor3s((GLshort)I(0), (GLshort)I(1), (GLshort)I(2))) \
    X(glColor3ub, "iii", glColor3ub((GLubyte)U(0), (GLubyte)U(1), (GLubyte)U(2))) \
    X(glColor3ui, "iii", glColor3ui(U(0), U(1), U(2))) \
    X(glColor3us, "iii", glColor3us((GLushort)U(0), (GLushort)U(1), (GLushort)U(2))) \
    X(glColor4b, "iiii", glColor4b((GLbyte)I(0), (GLbyte)I(1), (GLbyte)I(2), (GLbyte)I(3))) \
    X(glColor4d, "dddd", glColor4d(D(0), D(1), D(2), D(3))) \
    X(glColor4f, "ffff", glColor4f(F(0), F(1), F(2), F(3))) \
    X(glColor4i, "iiii", glColor4i(I(0), I(1), I(2), I(3))) \
    X(glColor4s, "iiii", glColor4s((GLshort)I(0), (GLshort)I(1), (GLshort)I(2), (GLshort)I(3))) \
    X(glColor4ub, "iiii", glColor4ub((GLubyte)U(0), (GLubyte)U(1), (GLubyte)U(2), (GLubyte)U(3))) \
    X(glColor4ui, "iiii", glColor4ui(U(0), U(1), U(2), U(3))) \
    X(glColor4us, "iiii", glColor4us((GLushort)U(0), (GLushort)U(1), (GLushort)U(2), (GLushort)U(3))) \
    X(glNormal3b, "iii", glNormal3b((GLbyte)I(0), (GLbyte)I(1), (GLbyte)I(2))) \
    X(glNormal3d, "ddd", glNormal3d(D(0), D(1), D(2))) \
    X(glNormal3f, "fff", glNormal3f(F(0), F(1), F(2))) \
    X(glNormal3i, "iii", glNormal3i(I(0), I(1), I(2))) \
    X(glNormal3s, "iii", glNormal3s((GLshort)I(0), (GLshort)I(1), (GLshort)I(2))) \
    X(glIndexd, "d", glIndexd(D(0))) \
    X(glIndexf, "f", glIndexf(F(0))) \
    X(glIndexi, "i", glIndexi(I(0))) \
    X(glIndexs, "i", glIndexs((GLshort)I(0))) \
    X(glIndexub, "i", glIndexub((GLubyte)U(0))) \
    X(glRasterPos2d, "dd", glRasterPos2d(D(0), D(1))) \
    X(glRasterPos2f, "ff", glRasterPos2f(F(0), F(1))) \
    X(glRasterPos2i, "ii", glRasterPos2i(I(0), I(1))) \
    X(glRasterPos2s, "ii", glRasterPos2s((GLshort)I(0), (GLshort)I(1))) \
    X(glRasterPos3d, "ddd", glRasterPos3d(D(0), D(1), D(2))) \
    X(glRasterPos3f, "fff", glRasterPos3f(F(0), F(1), F(2))) \
    X(glRasterPos3i, "iii", glRasterPos3i(I(0), I(1), I(2))) \
    X(glRasterPos3s, "iii", glRasterPos3s((GLshort)I(0), (GLshort)I(1), (GLshort)I(2))) \
    X(glRasterPos4d, "dddd", glRasterPos4d(D(0), D(1), D(2), D(3))) \
    X(glRasterPos4f, "ffff", glRasterPos4f(F(0), F(1), F(2), F(3))) \
    X(glRasterPos4i, "iiii", glRasterPos4i(I(0), I(1), I(2), I(3))) \
    X(glRasterPos4s, "iiii", glRasterPos4s((GLshort)I(0), (GLshort)I(1), (GLshort)I(2), (GLshort)I(3))) \
    X(glEvalCoord1d, "d", glEvalCoord1d(D(0))) \
    X(glEvalCoord1f, "f", glEvalCoord1f(F(0))) \
    X(glEvalCoord2d, "dd", glEvalCoord2d(D(0), D(1))) \
    X(glEvalCoord2f, "ff", glEvalCoord2f(F(0), F(1))) \
    X(glMultiTexCoord1dARB, "id", glMultiTexCoord1d(U(0), D(1))) \
    X(glMultiTexCoord1fARB, "if", glMultiTexCoord1f(U(0), F(1))) \
    X(glMultiTexCoord1iARB, "ii", glMultiTexCoord1i(U(0), I(1))) \
    X(glMultiTexCoord1sARB, "ii", glMultiTexCoord1s(U(0), (GLshort)I(1))) \
    X(glMultiTexCoord2dARB, "idd", glMultiTexCoord2d(U(0), D(1), D(2))) \
    X(glMultiTexCoord2fARB, "iff", glMultiTexCoord2f(U(0), F(1), F(2))) \
    X(glMultiTexCoord2iARB, "iii", glMultiTexCoord2i(U(0), I(1), I(2))) \
    X(glMultiTexCoord2sARB, "iii", glMultiTexCoord2s(U(0), (GLshort)I(1), (GLshort)I(2))) \
    X(glMultiTexCoord3dARB, "iddd", glMultiTexCoord3d(U(0), D(1), D(2), D(3))) \
    X(glMultiTexCoord3fARB, "ifff", glMultiTexCoord3f(U(0), F(1), F(2), F(3))) \
    X(glMultiTexCoord3iARB, "iiii", glMultiTexCoord3i(U(0), I(1), I(2), I(3))) \
    X(glMultiTexCoord3sARB, "iiii", glMultiTexCoord3s(U(0), (GLshort)I(1), (GLshort)I(2), (GLshort)I(3))) \
    X(glMultiTexCoord4dARB, "idddd", glMultiTexCoord4d(U(0), D(1), D(2), D(3), D(4))) \
    X(glMultiTexCoord4fARB, "iffff", glMultiTexCoord4f(U(0), F(1), F(2), F(3), F(4))) \
    X(glMultiTexCoord4iARB, "iiiii", glMultiTexCoord4i(U(0), I(1), I(2), I(3), I(4))) \
    X(glMultiTexCoord4sARB, "iiiii", glMultiTexCoord4s(U(0), (GLshort)I(1), (GLshort)I(2), (GLshort)I(3), (GLshort)I(4))) \
    X(glRectd, "dddd", glRectd(D(0), D(1), D(2), D(3))) \
    X(glRectf, "ffff", glRectf(F(0), F(1), F(2), F(3))) \
    X(glRecti, "iiii", glRecti(I(0), I(1), I(2), I(3))) \
    X(glRects, "iiii", glRects((GLshort)I(0), (GLshort)I(1), (GLshort)I(2), (GLshort)I(3))) \
    X(glEdgeFlag, "i", glEdgeFlag((GLboolean)U(0)))

#define GL_IMMEDIATE_V(XV, XT, XR) \
    XV(glVertex2dv, 2, GLdouble) \
    XV(glVertex2fv, 2, GLfloat) \
    XV(glVertex2iv, 2, GLint) \
    XV(glVertex2sv, 2, GLshort) \
    XV(glVertex3dv, 3, GLdouble) \
    XV(glVertex3fv, 3, GLfloat) \
    XV(glVertex3iv, 3, GLint) \
    XV(glVertex3sv, 3, GLshort) \
    XV(glVertex4dv, 4, GLdouble) \
    XV(glVertex4fv, 4, GLfloat) \
    XV(glVertex4iv, 4, GLint) \
    XV(glVertex4sv, 4, GLshort) \
    XV(glTexCoord1dv, 1, GLdouble) \
    XV(glTexCoord1fv, 1, GLfloat) \
    XV(glTexCoord1iv, 1, GLint) \
    XV(glTexCoord1sv, 1, GLshort) \
    XV(glTexCoord2dv, 2, GLdouble) \
    XV(glTexCoord2fv, 2, GLfloat) \
    XV(glTexCoord2iv, 2, GLint) \
    XV(glTexCoord2sv, 2, GLshort) \
    XV(glTexCoord3dv, 3, GLdouble) \
    XV(glTexCoord3fv, 3, GLfloat) \
    XV(glTexCoord3iv, 3, GLint) \
    XV(glTexCoord3sv, 3, GLshort) \
    XV(glTexCoord4dv, 4, GLdouble) \
    XV(glTexCoord4fv, 4, GLfloat) \
    XV(glTexCoord4iv, 4, GLint) \
    XV(glTexCoord4sv, 4, GLshort) \
    XV(glColor3bv, 3, GLbyte) \
    XV(glColor3dv, 3, GLdouble) \
    XV(glColor3fv, 3, GLfloat) \
    XV(glColor3iv, 3, GLint) \
    XV(glColor3sv, 3, GLshort) \
    XV(glColor3ubv, 3, GLubyte) \
    XV(glColor3uiv, 3, GLuint) \
    XV(glColor3usv, 3, GLushort) \
    XV(glColor4bv, 4, GLbyte) \
    XV(glColor4dv, 4, GLdouble) \
    XV(glColor4fv, 4, GLfloat) \
    XV(glColor4iv, 4, GLint) \
    XV(glColor4sv, 4, GLshort) \
    XV(glColor4ubv, 4, GLubyte) \
    XV(glColor4uiv, 4, GLuint) \
    XV(glColor4usv, 4, GLushort) \
    XV(glNormal3bv, 3, GLbyte) \
    XV(glNormal3dv, 3, GLdouble) \
    XV(glNormal3fv, 3, GLfloat) \
    XV(glNormal3iv, 3, GLint) \
    XV(glNormal3sv, 3, GLshort) \
    XV(glIndexdv, 1, GLdouble) \
    XV(glIndexfv, 1, GLfloat) \
    XV(glIndexiv, 1, GLint) \
    XV(glIndexsv, 1, GLshort) \
    XV(glIndexubv, 1, GLubyte) \
    XV(glRasterPos2dv, 2, GLdouble) \
    XV(glRasterPos2fv, 2, GLfloat) \
    XV(glRasterPos2iv, 2, GLint) \
    XV(glRasterPos2sv, 2, GLshort) \
    XV(glRasterPos3dv, 3, GLdouble) \
    XV(glRasterPos3fv, 3, GLfloat) \
    XV(glRasterPos3iv, 3, GLint) \
    XV(glRasterPos3sv, 3, GLshort) \
    XV(glRasterPos4dv, 4, GLdouble) \
    XV(glRasterPos4fv, 4, GLfloat) \
    XV(glRasterPos4iv, 4, GLint) \
    XV(glRasterPos4sv, 4, GLshort) \
    XV(glEvalCoord1dv, 1, GLdouble) \
    XV(glEvalCoord1fv, 1, GLfloat) \
    XV(glEvalCoord2dv, 2, GLdouble) \
    XV(glEvalCoord2fv, 2, GLfloat) \
    XV(glEdgeFlagv, 1, GLboolean) \
    XT(glMultiTexCoord1dvARB, glMultiTexCoord1dv, 1, GLdouble) \
    XT(glMultiTexCoord1fvARB, glMultiTexCoord1fv, 1, GLfloat) \
    XT(glMultiTexCoord1ivARB, glMultiTexCoord1iv, 1, GLint) \
    XT(glMultiTexCoord1svARB, glMultiTexCoord1sv, 1, GLshort) \
    XT(glMultiTexCoord2dvARB, glMultiTexCoord2dv, 2, GLdouble) \
    XT(glMultiTexCoord2fvARB, glMultiTexCoord2fv, 2, GLfloat) \
    XT(glMultiTexCoord2ivARB, glMultiTexCoord2iv, 2, GLint) \
    XT(glMultiTexCoord2svARB, glMultiTexCoord2sv, 2, GLshort) \
    XT(glMultiTexCoord3dvARB, glMultiTexCoord3dv, 3, GLdouble) \
    XT(glMultiTexCoord3fvARB, glMultiTexCoord3fv, 3, GLfloat) \
    XT(glMultiTexCoord3ivARB, glMultiTexCoord3iv, 3, GLint) \
    XT(glMultiTexCoord3svARB, glMultiTexCoord3sv, 3, GLshort) \
    XT(glMultiTexCoord4dvARB, glMultiTexCoord4dv, 4, GLdouble) \
    XT(glMultiTexCoord4fvARB, glMultiTexCoord4fv, 4, GLfloat) \
    XT(glMultiTexCoord4ivARB, glMultiTexCoord4iv, 4, GLint) \
    XT(glMultiTexCoord4svARB, glMultiTexCoord4sv, 4, GLshort) \
    XR(glRectdv, GLdouble) \
    XR(glRectfv, GLfloat) \
    XR(glRectiv, GLint) \
    XR(glRectsv, GLshort)

/* State, matrices, lists, lighting, texturing, pixels with nothing to
 * swap or whose pixels the host swaps itself. */
#define GL_STATE(X) \
    X(glBegin, "i", glBegin(U(0))) \
    X(glEnd, "", glEnd()) \
    X(glEnable, "i", glEnable(U(0))) \
    X(glDisable, "i", glDisable(U(0))) \
    X(glHint, "ii", glHint(U(0), U(1))) \
    X(glClear, "i", glClear(U(0))) \
    X(glClearColor, "ffff", glClearColor(F(0), F(1), F(2), F(3))) \
    X(glClearDepth, "d", glClearDepth(D(0))) \
    X(glClearStencil, "i", glClearStencil(I(0))) \
    X(glClearIndex, "f", glClearIndex(F(0))) \
    X(glClearAccum, "ffff", glClearAccum(F(0), F(1), F(2), F(3))) \
    X(glAccum, "if", glAccum(U(0), F(1))) \
    X(glColorMask, "iiii", glColorMask((GLboolean)U(0), (GLboolean)U(1), (GLboolean)U(2), (GLboolean)U(3))) \
    X(glDepthMask, "i", glDepthMask((GLboolean)U(0))) \
    X(glStencilMask, "i", glStencilMask(U(0))) \
    X(glIndexMask, "i", glIndexMask(U(0))) \
    X(glDepthFunc, "i", glDepthFunc(U(0))) \
    X(glDepthRange, "dd", glDepthRange(D(0), D(1))) \
    X(glAlphaFunc, "if", glAlphaFunc(U(0), F(1))) \
    X(glBlendFunc, "ii", glBlendFunc(U(0), U(1))) \
    X(glBlendColor, "ffff", glBlendColor(F(0), F(1), F(2), F(3))) \
    X(glBlendColorEXT, "ffff", glBlendColor(F(0), F(1), F(2), F(3))) \
    X(glBlendEquation, "i", glBlendEquation(U(0))) \
    X(glBlendEquationEXT, "i", glBlendEquation(U(0))) \
    X(glLogicOp, "i", glLogicOp(U(0))) \
    X(glStencilFunc, "iii", glStencilFunc(U(0), I(1), U(2))) \
    X(glStencilOp, "iii", glStencilOp(U(0), U(1), U(2))) \
    X(glCullFace, "i", glCullFace(U(0))) \
    X(glFrontFace, "i", glFrontFace(U(0))) \
    X(glPolygonMode, "ii", glPolygonMode(U(0), U(1))) \
    X(glPolygonOffset, "ff", glPolygonOffset(F(0), F(1))) \
    X(glPolygonStipple, "i", glPolygonStipple(P(0))) \
    X(glGetPolygonStipple, "i", glGetPolygonStipple(P(0))) \
    X(glLineWidth, "f", glLineWidth(F(0))) \
    X(glLineStipple, "ii", glLineStipple(I(0), (GLushort)U(1))) \
    X(glPointSize, "f", glPointSize(F(0))) \
    X(glShadeModel, "i", glShadeModel(U(0))) \
    X(glScissor, "iiii", glScissor(I(0), I(1), I(2), I(3))) \
    X(glViewport, "iiii", glViewport(I(0), I(1), I(2), I(3))) \
    X(glPixelZoom, "ff", glPixelZoom(F(0), F(1))) \
    X(glPixelStoref, "if", pixel_store(U(0), (GLint)F(1))) \
    X(glPixelStorei, "ii", pixel_store(U(0), I(1))) \
    X(glPixelTransferf, "if", glPixelTransferf(U(0), F(1))) \
    X(glPixelTransferi, "ii", glPixelTransferi(U(0), I(1))) \
    X(glMatrixMode, "i", glMatrixMode(U(0))) \
    X(glLoadIdentity, "", glLoadIdentity()) \
    X(glPushMatrix, "", glPushMatrix()) \
    X(glPopMatrix, "", glPopMatrix()) \
    X(glRotated, "dddd", glRotated(D(0), D(1), D(2), D(3))) \
    X(glRotatef, "ffff", glRotatef(F(0), F(1), F(2), F(3))) \
    X(glScaled, "ddd", glScaled(D(0), D(1), D(2))) \
    X(glScalef, "fff", glScalef(F(0), F(1), F(2))) \
    X(glTranslated, "ddd", glTranslated(D(0), D(1), D(2))) \
    X(glTranslatef, "fff", glTranslatef(F(0), F(1), F(2))) \
    X(glFrustum, "dddddd", glFrustum(D(0), D(1), D(2), D(3), D(4), D(5))) \
    X(glOrtho, "dddddd", glOrtho(D(0), D(1), D(2), D(3), D(4), D(5))) \
    X(glNewList, "ii", glNewList(U(0), U(1))) \
    X(glEndList, "", glEndList()) \
    X(glCallList, "i", glCallList(U(0))) \
    X(glDeleteLists, "ii", glDeleteLists(U(0), I(1))) \
    X(glListBase, "i", glListBase(U(0))) \
    X(glLightf, "iif", glLightf(U(0), U(1), F(2))) \
    X(glLighti, "iii", glLighti(U(0), U(1), I(2))) \
    X(glLightModelf, "if", glLightModelf(U(0), F(1))) \
    X(glLightModeli, "ii", glLightModeli(U(0), I(1))) \
    X(glMaterialf, "iif", glMaterialf(U(0), U(1), F(2))) \
    X(glMateriali, "iii", glMateriali(U(0), U(1), I(2))) \
    X(glColorMaterial, "ii", glColorMaterial(U(0), U(1))) \
    X(glFogf, "if", glFogf(U(0), F(1))) \
    X(glFogi, "ii", glFogi(U(0), I(1))) \
    X(glTexEnvf, "iif", glTexEnvf(U(0), U(1), F(2))) \
    X(glTexEnvi, "iii", glTexEnvi(U(0), U(1), I(2))) \
    X(glTexParameterf, "iif", glTexParameterf(U(0), U(1), F(2))) \
    X(glTexParameteri, "iii", glTexParameteri(U(0), U(1), I(2))) \
    X(glTexGend, "iid", glTexGend(U(0), U(1), D(2))) \
    X(glTexGenf, "iif", glTexGenf(U(0), U(1), F(2))) \
    X(glTexGeni, "iii", glTexGeni(U(0), U(1), I(2))) \
    X(glTexImage1D, "iiiiiiii", glTexImage1D(U(0), I(1), I(2), I(3), I(4), U(5), U(6), P(7))) \
    X(glTexImage2D, "iiiiiiiii", glTexImage2D(U(0), I(1), I(2), I(3), I(4), I(5), U(6), U(7), P(8))) \
    X(glTexImage3D, "iiiiiiiiii", glTexImage3D(U(0), I(1), I(2), I(3), I(4), I(5), I(6), U(7), U(8), P(9))) \
    X(glTexImage3DEXT, "iiiiiiiiii", glTexImage3D(U(0), I(1), I(2), I(3), I(4), I(5), I(6), U(7), U(8), P(9))) \
    X(glTexSubImage1D, "iiiiiii", glTexSubImage1D(U(0), I(1), I(2), I(3), U(4), U(5), P(6))) \
    X(glTexSubImage2D, "iiiiiiiii", glTexSubImage2D(U(0), I(1), I(2), I(3), I(4), I(5), U(6), U(7), P(8))) \
    X(glTexSubImage3D, "iiiiiiiiiii", glTexSubImage3D(U(0), I(1), I(2), I(3), I(4), I(5), I(6), I(7), U(8), U(9), P(10))) \
    X(glTexSubImage3DEXT, "iiiiiiiiiii", glTexSubImage3D(U(0), I(1), I(2), I(3), I(4), I(5), I(6), I(7), U(8), U(9), P(10))) \
    X(glCopyTexImage1D, "iiiiiii", glCopyTexImage1D(U(0), I(1), U(2), I(3), I(4), I(5), I(6))) \
    X(glCopyTexImage2D, "iiiiiiii", glCopyTexImage2D(U(0), I(1), U(2), I(3), I(4), I(5), I(6), I(7))) \
    X(glCopyTexSubImage1D, "iiiiii", glCopyTexSubImage1D(U(0), I(1), I(2), I(3), I(4), I(5))) \
    X(glCopyTexSubImage2D, "iiiiiiii", glCopyTexSubImage2D(U(0), I(1), I(2), I(3), I(4), I(5), I(6), I(7))) \
    X(glCopyTexSubImage3D, "iiiiiiiii", glCopyTexSubImage3D(U(0), I(1), I(2), I(3), I(4), I(5), I(6), I(7), I(8))) \
    X(glCopyTexSubImage3DEXT, "iiiiiiiii", glCopyTexSubImage3D(U(0), I(1), I(2), I(3), I(4), I(5), I(6), I(7), I(8))) \
    X(glGetTexImage, "iiiii", glGetTexImage(U(0), I(1), U(2), U(3), P(4))) \
    X(glBindTexture, "ii", glBindTexture(U(0), U(1))) \
    X(glBindTextureEXT, "ii", glBindTexture(U(0), U(1))) \
    X(glActiveTextureARB, "i", glActiveTexture(U(0))) \
    X(glDrawPixels, "iiiii", draw_pixels(I(0), I(1), U(2), U(3), P(4))) \
    X(glReadPixels, "iiiiiii", glReadPixels(I(0), I(1), I(2), I(3), U(4), U(5), P(6))) \
    X(glCopyPixels, "iiiii", glCopyPixels(I(0), I(1), I(2), I(3), U(4))) \
    X(glBitmap, "iiffffi", glBitmap(I(0), I(1), F(2), F(3), F(4), F(5), P(6))) \
    X(glInitNames, "", glInitNames()) \
    X(glLoadName, "i", glLoadName(U(0))) \
    X(glPushName, "i", glPushName(U(0))) \
    X(glPopName, "", glPopName()) \
    X(glPassThrough, "f", glPassThrough(F(0))) \
    X(glPushAttrib, "i", glPushAttrib(U(0))) \
    X(glPopAttrib, "", glPopAttrib()) \
    X(glMapGrid1d, "idd", glMapGrid1d(I(0), D(1), D(2))) \
    X(glMapGrid1f, "iff", glMapGrid1f(I(0), F(1), F(2))) \
    X(glMapGrid2d, "iddidd", glMapGrid2d(I(0), D(1), D(2), I(3), D(4), D(5))) \
    X(glMapGrid2f, "iffiff", glMapGrid2f(I(0), F(1), F(2), I(3), F(4), F(5))) \
    X(glEvalMesh1, "iii", glEvalMesh1(U(0), I(1), I(2))) \
    X(glEvalMesh2, "iiiii", glEvalMesh2(U(0), I(1), I(2), I(3), I(4))) \
    X(glEvalPoint1, "i", glEvalPoint1(I(0))) \
    X(glEvalPoint2, "ii", glEvalPoint2(I(0), I(1))) \
    X(glLockArraysEXT, "ii", (void)0) \
    X(glUnlockArraysEXT, "", (void)0) \
    X(gluOrtho2D, "dddd", gluOrtho2D(D(0), D(1), D(2), D(3))) \
    X(gluPerspective, "dddd", gluPerspective(D(0), D(1), D(2), D(3))) \
    X(gluLookAt, "ddddddddd", gluLookAt(D(0), D(1), D(2), D(3), D(4), D(5), D(6), D(7), D(8)))

/* The calls that return something. */
#define GL_RESULTS(X) \
    X(glIsEnabled, "i", glIsEnabled(U(0))) \
    X(glIsList, "i", glIsList(U(0))) \
    X(glIsTexture, "i", glIsTexture(U(0))) \
    X(glIsTextureEXT, "i", glIsTexture(U(0))) \
    X(glGenLists, "i", glGenLists(I(0))) \
    X(glGetError, "", get_error()) \
    X(gluBuild1DMipmaps, "iiiiii", gluBuild1DMipmaps(U(0), I(1), I(2), U(3), U(4), P(5))) \
    X(gluBuild2DMipmaps, "iiiiiii", gluBuild2DMipmaps(U(0), I(1), I(2), I(3), U(4), U(5), P(6))) \
    X(gluScaleImage, "iiiiiiiii", gluScaleImage(U(0), I(1), I(2), U(3), P(4), I(5), I(6), U(7), P(8)))

/* Lights, materials, texturing and fog, from vectors in guest memory;
 * the gets of them. */
#define GL_PARAMS(X, X1, XG) \
    X(glLightfv, GLfloat) X(glLightiv, GLint) X(glMaterialfv, GLfloat) X(glMaterialiv, GLint) \
    X(glTexEnvfv, GLfloat) X(glTexEnviv, GLint) X(glTexParameterfv, GLfloat) X(glTexParameteriv, GLint) \
    X(glTexGendv, GLdouble) X(glTexGenfv, GLfloat) X(glTexGeniv, GLint) \
    X1(glFogfv, GLfloat) X1(glFogiv, GLint) X1(glLightModelfv, GLfloat) X1(glLightModeliv, GLint) \
    XG(glGetLightfv, GLfloat) XG(glGetLightiv, GLint) XG(glGetMaterialfv, GLfloat) XG(glGetMaterialiv, GLint) \
    XG(glGetTexEnvfv, GLfloat) XG(glGetTexEnviv, GLint) XG(glGetTexParameterfv, GLfloat) \
    XG(glGetTexParameteriv, GLint) XG(glGetTexGendv, GLdouble) XG(glGetTexGenfv, GLfloat) XG(glGetTexGeniv, GLint)

/* Matrices from guest memory. */
#define GL_MATRICES(X) \
    X(glLoadMatrixf, glLoadMatrixf, GLfloat) X(glLoadMatrixd, glLoadMatrixd, GLdouble) \
    X(glMultMatrixf, glMultMatrixf, GLfloat) X(glMultMatrixd, glMultMatrixd, GLdouble) \
    X(glLoadTransposeMatrixfARB, glLoadTransposeMatrixf, GLfloat) \
    X(glLoadTransposeMatrixdARB, glLoadTransposeMatrixd, GLdouble) \
    X(glMultTransposeMatrixfARB, glMultTransposeMatrixf, GLfloat) \
    X(glMultTransposeMatrixdARB, glMultTransposeMatrixd, GLdouble)

/* GLU's quadrics: their settings and what they draw. */
#define Q() GLUquadric *q = quadric(iv[0]); if (q)
#define GLU_QUADRICS(X) \
    X(gluQuadricDrawStyle, "ii", Q() gluQuadricDrawStyle(q, U(1))) \
    X(gluQuadricNormals, "ii", Q() gluQuadricNormals(q, U(1))) \
    X(gluQuadricOrientation, "ii", Q() gluQuadricOrientation(q, U(1))) \
    X(gluQuadricTexture, "ii", Q() gluQuadricTexture(q, (GLboolean)U(1))) \
    X(gluSphere, "idii", Q() gluSphere(q, D(1), I(2), I(3))) \
    X(gluCylinder, "idddii", Q() gluCylinder(q, D(1), D(2), D(3), I(4), I(5))) \
    X(gluDisk, "iddii", Q() gluDisk(q, D(1), D(2), I(3), I(4))) \
    X(gluPartialDisk, "iddiidd", Q() gluPartialDisk(q, D(1), D(2), I(3), I(4), D(5), D(6)))

/* Every call a display list can record, by its name. */
enum {
    OP_NONE,
#define OP_ENUM(name, ...) OP_##name,
#define OP_NO(...)
    GL_IMMEDIATE(OP_ENUM) GL_IMMEDIATE_V(OP_ENUM, OP_ENUM, OP_ENUM) GL_STATE(OP_ENUM) GL_RESULTS(OP_ENUM)
    GL_PARAMS(OP_ENUM, OP_ENUM, OP_NO) GL_MATRICES(OP_ENUM) GLU_QUADRICS(OP_ENUM)
#undef OP_ENUM
#undef OP_NO
    OP_glClipPlane, OP_glCallLists, OP_glDrawBuffer, OP_glPixelMapfv, OP_glPixelMapuiv, OP_glPixelMapusv,
    OP_glMap1d, OP_glMap1f, OP_glMap2d, OP_glMap2f,
    OP_arrays,  /* glDrawArrays, glDrawElements: the arrays' elements as they were */
    OP_element, /* glArrayElement: the element's values */
    OP_COUNT
};

static void hook(int op, const char *sig, const uint32_t *iv, const double *fv);
static void record(int op, const uint32_t *words, int nwords, const void *data, uint32_t nbytes);
static GLenum get_error(void);
static void lists_made(GLuint first, GLsizei range);

#define DEFINE_RESULT(name, sig, expr)                                                                                \
    static void w_##name(ppc_cpu *c)                                                                                  \
    {                                                                                                                 \
        uint32_t iv[16];                                                                                              \
        double fv[16];                                                                                                \
        ENTER();                                                                                                      \
        args_get(c, sig, iv, fv);                                                                                     \
        hook(OP_##name, sig, iv, fv);                                                                                 \
        c->gpr[3] = (uint32_t)(expr);                                                                                 \
        if (OP_##name == OP_glGenLists)                                                                               \
            lists_made(c->gpr[3], I(0));                                                                              \
        DONE();                                                                                                       \
    }

/* glDrawPixels: untextured, whatever texturing is on, as the era's
 * accelerated renderers drew pixels (sixtyforce draws its frame rate
 * counter with a texture still enabled). */
static void draw_pixels(GLsizei w, GLsizei h, GLenum format, GLenum type, const void *pixels)
{
    GLboolean t1 = glIsEnabled(GL_TEXTURE_1D), t2 = glIsEnabled(GL_TEXTURE_2D), t3 = glIsEnabled(GL_TEXTURE_3D);
    if (t1 || t2 || t3) {
        glPushAttrib(GL_ENABLE_BIT);
        glDisable(GL_TEXTURE_1D);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_TEXTURE_3D);
    }
    glDrawPixels(w, h, format, type, pixels);
    if (t1 || t2 || t3)
        glPopAttrib();
}

/* glPixelStore: the program's swapping is the opposite of the host's, and
 * Apple's client storage (keeping the program's pixels) has no meaning
 * across the byte order. */
static void pixel_store(GLenum pname, GLint v)
{
    if (pname == GL_UNPACK_SWAP_BYTES || pname == GL_PACK_SWAP_BYTES)
        glPixelStorei(pname, !v);
    else if (pname != GL_UNPACK_CLIENT_STORAGE_APPLE)
        glPixelStorei(pname, v);
}

GL_IMMEDIATE(DEFINE)
GL_IMMEDIATE_V(DEFINE_V, DEFINE_T, DEFINE_R)
GL_STATE(DEFINE)
GL_RESULTS(DEFINE_RESULT)

/* ---- Flushing: AGL shows the frame of a context with one buffer ---- */

static bool front_drawn(void)
{
    return cur->draw_buffer == GL_FRONT || cur->draw_buffer == GL_FRONT_LEFT || cur->draw_buffer == GL_FRONT_RIGHT ||
           cur->draw_buffer == GL_FRONT_AND_BACK;
}

static void w_glFlush(ppc_cpu *c)
{
    ENTER();
    glFlush();
    agl_flushed(front_drawn());
    DONE();
}
static void w_glFinish(ppc_cpu *c)
{
    ENTER();
    glFinish();
    agl_flushed(front_drawn());
    DONE();
}

/* Front and back are the one colour buffer. */
static void w_glDrawBuffer(ppc_cpu *c)
{
    ENTER();
    cur->draw_buffer = (GLenum)c->gpr[3];
    uint32_t host = cur->draw_buffer == GL_NONE ? GL_NONE : GL_COLOR_ATTACHMENT0_EXT;
    if (cur->recording)
        record(OP_glDrawBuffer, &host, 1, NULL, 0);
    glDrawBuffer(host);
    DONE();
}
static void w_glReadBuffer(ppc_cpu *c)
{
    ENTER();
    cur->read_buffer = (GLenum)c->gpr[3];
    DONE();
}

/* ---- Vectors and matrices from guest memory ---- */

/* How many values a light, material, fog, texture or light-model
 * parameter has. */
static int param_count(GLenum pname)
{
    switch (pname) {
    case GL_AMBIENT: case GL_DIFFUSE: case GL_SPECULAR: case GL_EMISSION: case GL_POSITION:
    case GL_AMBIENT_AND_DIFFUSE: case GL_TEXTURE_BORDER_COLOR: case GL_TEXTURE_ENV_COLOR: case GL_FOG_COLOR:
    case GL_LIGHT_MODEL_AMBIENT: case GL_OBJECT_PLANE: case GL_EYE_PLANE:
        return 4;
    case GL_SPOT_DIRECTION: case GL_COLOR_INDEXES:
        return 3;
    default:
        return 1;
    }
}

#define DEFINE_PV(name, T)                                                                                            \
    static void w_##name(ppc_cpu *c)                                                                                  \
    {                                                                                                                 \
        ENTER();                                                                                                      \
        T t[4];                                                                                                       \
        int n = param_count((GLenum)c->gpr[4]);                                                                       \
        gread(t, c->gpr[5], n, sizeof(T));                                                                            \
        if (cur->recording)                                                                                           \
            record(OP_##name, &c->gpr[3], 2, t, (uint32_t)n * sizeof(T));                                             \
        name((GLenum)c->gpr[3], (GLenum)c->gpr[4], t);                                                                \
        DONE();                                                                                                       \
    }
#define DEFINE_PV1(name, T) /* no target: fog and the light model */                                                   \
    static void w_##name(ppc_cpu *c)                                                                                  \
    {                                                                                                                 \
        ENTER();                                                                                                      \
        T t[4];                                                                                                       \
        int n = param_count((GLenum)c->gpr[3]);                                                                       \
        gread(t, c->gpr[4], n, sizeof(T));                                                                            \
        if (cur->recording)                                                                                           \
            record(OP_##name, &c->gpr[3], 1, t, (uint32_t)n * sizeof(T));                                             \
        name((GLenum)c->gpr[3], t);                                                                                   \
        DONE();                                                                                                       \
    }
#define DEFINE_GETPV(name, T)                                                                                         \
    static void w_##name(ppc_cpu *c)                                                                                  \
    {                                                                                                                 \
        ENTER();                                                                                                      \
        T t[4] = { 0 };                                                                                               \
        name((GLenum)c->gpr[3], (GLenum)c->gpr[4], t);                                                                \
        if (c->gpr[5])                                                                                                \
            gwrite(c->gpr[5], t, param_count((GLenum)c->gpr[4]), sizeof(T));                                          \
        DONE();                                                                                                       \
    }
GL_PARAMS(DEFINE_PV, DEFINE_PV1, DEFINE_GETPV)

#define DEFINE_MATRIX(name, host, T)                                                                                  \
    static void w_##name(ppc_cpu *c)                                                                                  \
    {                                                                                                                 \
        ENTER();                                                                                                      \
        T m[16];                                                                                                      \
        gread(m, c->gpr[3], 16, sizeof(T));                                                                           \
        if (cur->recording)                                                                                           \
            record(OP_##name, NULL, 0, m, sizeof m);                                                                  \
        host(m);                                                                                                      \
        DONE();                                                                                                       \
    }
GL_MATRICES(DEFINE_MATRIX)

static void w_glClipPlane(ppc_cpu *c)
{
    ENTER();
    GLdouble e[4];
    gread(e, c->gpr[4], 4, 8);
    if (cur->recording)
        record(OP_glClipPlane, &c->gpr[3], 1, e, sizeof e);
    glClipPlane((GLenum)c->gpr[3], e);
    DONE();
}
static void w_glGetClipPlane(ppc_cpu *c)
{
    ENTER();
    GLdouble e[4] = { 0 };
    glGetClipPlane((GLenum)c->gpr[3], e);
    gwrite(c->gpr[4], e, 4, 8);
    DONE();
}

static void w_glGetTexLevelParameterfv(ppc_cpu *c)
{
    ENTER();
    GLfloat v = 0;
    glGetTexLevelParameterfv((GLenum)c->gpr[3], (GLint)c->gpr[4], (GLenum)c->gpr[5], &v);
    put_float(c->gpr[6], v);
    DONE();
}
static void w_glGetTexLevelParameteriv(ppc_cpu *c)
{
    ENTER();
    GLint v = 0;
    glGetTexLevelParameteriv((GLenum)c->gpr[3], (GLint)c->gpr[4], (GLenum)c->gpr[5], &v);
    wr32(c->gpr[6], (uint32_t)v);
    DONE();
}

/* ---- Texture and list names ---- */

/* The texture names a group has made or bound, for hibernating: sorted. */
static int name_at(const glgroup *g, uint32_t name, bool *found)
{
    int lo = 0, hi = g->ntex;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (g->tex[mid] < name)
            lo = mid + 1;
        else
            hi = mid;
    }
    *found = lo < g->ntex && g->tex[lo] == name;
    return lo;
}
static void name_add(glgroup *g, uint32_t name)
{
    bool found;
    int at = name_at(g, name, &found);
    if (found || !name)
        return;
    if (g->ntex == g->captex) {
        int cap = g->captex ? g->captex * 2 : 64;
        uint32_t *n = bb_realloc(g->tex, (size_t)cap * 4, BB_BYTES);
        if (!n)
            return;
        g->tex = n;
        g->captex = cap;
    }
    memmove(g->tex + at + 1, g->tex + at, (size_t)(g->ntex - at) * 4);
    g->tex[at] = name;
    g->ntex++;
}
static void name_remove(glgroup *g, uint32_t name)
{
    bool found;
    int at = name_at(g, name, &found);
    if (!found)
        return;
    memmove(g->tex + at, g->tex + at + 1, (size_t)(g->ntex - at - 1) * 4);
    g->ntex--;
}

/* ---- Display lists, recorded ----
 *
 * The host compiles a list, and keeps it where no snapshot can reach; so
 * that a resumed program has its lists, each call compiled into one is
 * recorded as well, as the host was given it (its arguments in host
 * order, the guest memory it read copied), and compiled again from the
 * record in the new context (replay).  A record is an op (u16), a pad, the
 * payload's length (u32) and the payload, padded to 8 bytes. */

static size_t type_size(GLenum t);

static int list_at(const glgroup *g, uint32_t name, bool *found)
{
    int lo = 0, hi = g->nlists;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (g->lists[mid].name < name)
            lo = mid + 1;
        else
            hi = mid;
    }
    *found = lo < g->nlists && g->lists[lo].name == name;
    return lo;
}

/* The group's list `name`, made (empty) if it has none. */
static gllist *list_get(glgroup *g, uint32_t name)
{
    bool found;
    int at = list_at(g, name, &found);
    if (found)
        return &g->lists[at];
    if (g->nlists == g->caplists) {
        int cap = g->caplists ? g->caplists * 2 : 64;
        gllist *n = bb_realloc(g->lists, (size_t)cap * sizeof *n, BB_TAG(gllist));
        if (!n)
            return NULL;
        g->lists = n;
        g->caplists = cap;
    }
    memmove(g->lists + at + 1, g->lists + at, (size_t)(g->nlists - at) * sizeof *g->lists);
    g->nlists++;
    g->lists[at] = (gllist){ name, NULL, 0, 0 };
    return &g->lists[at];
}

/* glGenLists: empty lists, as the host has them. */
static void lists_made(GLuint first, GLsizei range)
{
    for (GLsizei i = 0; first && i < range; i++)
        list_get(cur->group, first + (GLuint)i);
}

static void lists_deleted(GLuint first, GLsizei range)
{
    glgroup *g = cur->group;
    for (int i = 0; i < g->nlists;) {
        if (g->lists[i].name >= first && g->lists[i].name - first < (GLuint)range) {
            bb_free(g->lists[i].ops);
            memmove(g->lists + i, g->lists + i + 1, (size_t)(g->nlists - i - 1) * sizeof *g->lists);
            g->nlists--;
        } else {
            i++;
        }
    }
}

static void list_begin(GLuint name, GLenum mode)
{
    if (cur->recording || !name || (mode != GL_COMPILE && mode != GL_COMPILE_AND_EXECUTE))
        return; /* errors: the host compiles nothing */
    cur->recording = true;
    cur->list_mode = mode;
    cur->compiling.name = name;
    cur->compiling.len = 0;
}

/* glEndList: the record replaces what the list had. */
static void list_end(void)
{
    if (!cur->recording)
        return;
    cur->recording = false;
    gllist *l = list_get(cur->group, cur->compiling.name);
    if (!l)
        return;
    bb_free(l->ops);
    l->ops = cur->compiling.ops;
    l->len = cur->compiling.len;
    l->cap = cur->compiling.cap;
    cur->compiling = (gllist){ 0, NULL, 0, 0 };
}

static uint8_t *rec_room(gllist *l, uint32_t n)
{
    if (l->len + n > l->cap) {
        uint32_t cap = l->cap ? l->cap : 256;
        while (cap < l->len + n)
            cap *= 2;
        uint8_t *p = bb_realloc(l->ops, cap, BB_BYTES);
        if (!p)
            return NULL;
        l->ops = p;
        l->cap = cap;
    }
    uint8_t *at = l->ops + l->len;
    l->len += n;
    return at;
}

/* An op's record, its payload put in parts (rec_put) and closed. */
static uint32_t rec_open(gllist *l, int op)
{
    uint32_t at = l->len;
    uint8_t *h = rec_room(l, 8);
    if (h) {
        uint16_t o = (uint16_t)op, z = 0;
        memcpy(h, &o, 2);
        memcpy(h + 2, &z, 2);
        memset(h + 4, 0, 4);
    }
    return at;
}
static void rec_put(gllist *l, const void *data, uint32_t n)
{
    uint8_t *p = n ? rec_room(l, n) : NULL;
    if (p)
        memcpy(p, data, n);
}
static void rec_u32(gllist *l, uint32_t v) { rec_put(l, &v, 4); }
static void rec_close(gllist *l, uint32_t at)
{
    if (!l->ops || at + 8 > l->len)
        return;
    uint32_t len = l->len - at - 8;
    memcpy(l->ops + at + 4, &len, 4);
    uint32_t pad = (8 - (l->len & 7)) & 7;
    uint8_t *p = pad ? rec_room(l, pad) : NULL;
    if (p)
        memset(p, 0, pad);
}

/* An op of words then bytes. */
static void record(int op, const uint32_t *words, int nwords, const void *data, uint32_t nbytes)
{
    gllist *l = &cur->compiling;
    uint32_t at = rec_open(l, op);
    rec_put(l, words, 4 * (uint32_t)nwords);
    rec_put(l, data, nbytes);
    rec_close(l, at);
}

/* A call's arguments by its signature: integers as words, floats as
 * floats and doubles as doubles. */
static void rec_args(gllist *l, const char *sig, const uint32_t *iv, const double *fv)
{
    for (int k = 0; sig[k]; k++) {
        if (sig[k] == 'f') {
            float f = (float)fv[k];
            rec_put(l, &f, 4);
        } else if (sig[k] == 'd') {
            rec_put(l, &fv[k], 8);
        } else {
            rec_u32(l, iv[k]);
        }
    }
}

static void record_map(int op, const char *sig, const uint32_t *iv, const double *fv, const void *data, uint32_t n)
{
    gllist *l = &cur->compiling;
    uint32_t at = rec_open(l, op);
    rec_args(l, sig, iv, fv);
    rec_u32(l, n);
    rec_put(l, data, n);
    rec_close(l, at);
}

/* ---- The pixels a call reads, as the unpacking state has them ---- */

enum { EXTRA_PIXELS = 1, EXTRA_QUADRIC = 2 };
static const GLenum unpack_names[8] = {
    GL_UNPACK_SWAP_BYTES, GL_UNPACK_LSB_FIRST, GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_ROWS,
    GL_UNPACK_SKIP_PIXELS, GL_UNPACK_ALIGNMENT, GL_UNPACK_IMAGE_HEIGHT, GL_UNPACK_SKIP_IMAGES,
};

static int format_components(GLenum f)
{
    switch (f) {
    case GL_LUMINANCE_ALPHA: return 2;
    case GL_RGB: case GL_BGR: return 3;
    case GL_RGBA: case GL_BGRA: case GL_ABGR_EXT: return 4;
    default: return 1;
    }
}

/* Bytes per element (a component, or a packed pixel: then n is 1). */
static size_t element_size(GLenum type, int *n)
{
    switch (type) {
    case GL_UNSIGNED_BYTE_3_3_2: case GL_UNSIGNED_BYTE_2_3_3_REV: *n = 1; return 1;
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_5_6_5_REV: case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_4_4_4_4_REV: case GL_UNSIGNED_SHORT_5_5_5_1: case GL_UNSIGNED_SHORT_1_5_5_5_REV:
        *n = 1;
        return 2;
    case GL_UNSIGNED_INT_8_8_8_8: case GL_UNSIGNED_INT_8_8_8_8_REV: case GL_UNSIGNED_INT_10_10_10_2:
    case GL_UNSIGNED_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_24_8_EXT:
        *n = 1;
        return 4;
    default: return type_size(type);
    }
}

/* What an image of w by h by d read with the unpacking state u spans,
 * from its pointer (the OpenGL specification's unpacking, 3.6.4). */
static size_t image_bytes(int w, int h, int d, GLenum format, GLenum type, const GLint *u)
{
    if (w <= 0 || h <= 0 || d <= 0)
        return 0;
    size_t l = u[2] > 0 ? (size_t)u[2] : (size_t)w, a = u[5] > 0 ? (size_t)u[5] : 1;
    size_t rows = u[6] > 0 ? (size_t)u[6] : (size_t)h;
    int n = format_components(format);
    if (type == GL_BITMAP) {
        size_t k = a * (((size_t)n * l + 8 * a - 1) / (8 * a));
        size_t bits = (size_t)u[4] + (size_t)w * (size_t)n;
        return (size_t)u[3] * k + ((size_t)h - 1) * k + (bits + 7) / 8;
    }
    size_t s = element_size(type, &n);
    if (!s)
        return 0;
    size_t k = s >= a ? (size_t)n * l * s : a * (((size_t)n * l * s + a - 1) / a);
    size_t start = (size_t)u[4] * (size_t)n * s + (size_t)u[3] * k + (size_t)u[7] * k * rows;
    return start + (((size_t)d - 1) * rows + ((size_t)h - 1)) * k + (size_t)w * (size_t)n * s;
}

/* The calls that read an image from the program's memory: where, and
 * its size and layout. */
static bool image_args(int op, const uint32_t *iv, gaddr *ptr, int *w, int *h, int *d, GLenum *f, GLenum *t)
{
    *h = *d = 1;
    switch (op) {
    case OP_glTexImage1D: *ptr = iv[7], *w = I(3), *f = U(5), *t = U(6); return true;
    case OP_glTexImage2D: *ptr = iv[8], *w = I(3), *h = I(4), *f = U(6), *t = U(7); return true;
    case OP_glTexImage3D: case OP_glTexImage3DEXT:
        *ptr = iv[9], *w = I(3), *h = I(4), *d = I(5), *f = U(7), *t = U(8);
        return true;
    case OP_glTexSubImage1D: *ptr = iv[6], *w = I(3), *f = U(4), *t = U(5); return true;
    case OP_glTexSubImage2D: *ptr = iv[8], *w = I(4), *h = I(5), *f = U(6), *t = U(7); return true;
    case OP_glTexSubImage3D: case OP_glTexSubImage3DEXT:
        *ptr = iv[10], *w = I(5), *h = I(6), *d = I(7), *f = U(8), *t = U(9);
        return true;
    case OP_glDrawPixels: *ptr = iv[4], *w = I(0), *h = I(1), *f = U(2), *t = U(3); return true;
    case OP_glBitmap: *ptr = iv[6], *w = I(0), *h = I(1), *f = GL_COLOR_INDEX, *t = GL_BITMAP; return true;
    case OP_glPolygonStipple: *ptr = iv[0], *w = *h = 32, *f = GL_COLOR_INDEX, *t = GL_BITMAP; return true;
    case OP_gluBuild1DMipmaps: *ptr = iv[5], *w = I(2), *f = U(3), *t = U(4); return true;
    case OP_gluBuild2DMipmaps: *ptr = iv[6], *w = I(2), *h = I(3), *f = U(4), *t = U(5); return true;
    default: return false;
    }
}

static void rec_pixels(gllist *l, const GLint *unpack, const void *data, size_t n)
{
    rec_u32(l, EXTRA_PIXELS);
    rec_put(l, unpack, 32);
    rec_u32(l, (uint32_t)n);
    rec_put(l, data, (uint32_t)n);
}

/* GLU's quadrics: their settings (draw style, normals, orientation,
 * texture), kept for each (a display list draws one as it was set). */
enum { MAX_QUADRICS = 64 };
static uint32_t quadric_settings[MAX_QUADRICS][4];
static int quadric_index(gaddr q);

/* What each call does as it compiles into a list, before the host call. */
static bool compiled(int op)
{
    switch (op) {
    case OP_glNewList: case OP_glEndList: case OP_glDeleteLists: case OP_glGetPolygonStipple:
    case OP_glGetTexImage: case OP_glReadPixels: case OP_glPixelStoref: case OP_glPixelStorei:
    case OP_glLockArraysEXT: case OP_glUnlockArraysEXT: case OP_glIsEnabled: case OP_glIsList: case OP_glIsTexture:
    case OP_glIsTextureEXT: case OP_glGenLists: case OP_glGetError: case OP_gluScaleImage:
    case OP_gluQuadricDrawStyle: case OP_gluQuadricNormals: case OP_gluQuadricOrientation:
    case OP_gluQuadricTexture:
        return false;
    default: return true;
    }
}

static void hook(int op, const char *sig, const uint32_t *iv, const double *fv)
{
    switch (op) {
    case OP_glBegin: case OP_glEnd:
        if (!cur->recording || cur->list_mode == GL_COMPILE_AND_EXECUTE)
            cur->in_begin = op == OP_glBegin;
        break;
    case OP_glBindTexture: case OP_glBindTextureEXT: name_add(cur->group, iv[1]); break;
    case OP_glNewList: list_begin(iv[0], iv[1]); return;
    case OP_glEndList: list_end(); return;
    case OP_glDeleteLists: lists_deleted(iv[0], (GLsizei)iv[1]); return;
    case OP_gluQuadricDrawStyle: case OP_gluQuadricNormals: case OP_gluQuadricOrientation:
    case OP_gluQuadricTexture: {
        int q = quadric_index(iv[0]);
        if (q >= 0)
            quadric_settings[q][op - OP_gluQuadricDrawStyle] = iv[1];
        return;
    }
    }
    if (!cur->recording || !compiled(op))
        return;
    gllist *l = &cur->compiling;
    uint32_t at = rec_open(l, op);
    rec_args(l, sig, iv, fv);
    gaddr ptr = 0;
    int w, h, d;
    GLenum f, t;
    if (image_args(op, iv, &ptr, &w, &h, &d, &f, &t) && ptr) {
        GLint u[8];
        for (int k = 0; k < 8; k++)
            glGetIntegerv(unpack_names[k], &u[k]);
        rec_pixels(l, u, gptr(ptr), image_bytes(w, h, d, f, t, u));
    }
    if (op >= OP_gluSphere && op <= OP_gluPartialDisk) {
        int q = quadric_index(iv[0]);
        rec_u32(l, EXTRA_QUADRIC);
        static const uint32_t defaults[4] = { GLU_FILL, GLU_SMOOTH, GLU_OUTSIDE, GL_FALSE };
        rec_put(l, q >= 0 ? quadric_settings[q] : defaults, 16);
    }
    rec_close(l, at);
}

/* aglUseFont's glyph: its list one glBitmap of the bits given. */
static void glyph_recorded(uint32_t list, int width, int height, int row_bytes, float xorig, float yorig, float xmove,
                           const uint8_t *bits)
{
    gllist *dst = list_get(cur->group, list);
    if (!dst)
        return;
    gllist l = { list, NULL, 0, 0 };
    uint32_t iv[7] = { (uint32_t)width, (uint32_t)height, 0, 0, 0, 0, 0 };
    double fv[7] = { 0, 0, xorig, yorig, xmove, 0, 0 };
    uint32_t at = rec_open(&l, OP_glBitmap);
    rec_args(&l, "iiffffi", iv, fv);
    GLint u[8] = { 0, 0, row_bytes * 8, 0, 0, 1, 0, 0 };
    rec_pixels(&l, u, bits, (size_t)row_bytes * (size_t)height);
    rec_close(&l, at);
    bb_free(dst->ops);
    *dst = l;
}

/* glGetError: one the host had when hibernating comes first. */
static GLenum get_error(void)
{
    GLenum e = cur->pending_error;
    if (e) {
        cur->pending_error = GL_NO_ERROR;
        return e;
    }
    return glGetError();
}

static void w_glGenTextures(ppc_cpu *c)
{
    ENTER();
    GLsizei n = (GLsizei)c->gpr[3];
    GLuint *t = n > 0 ? scratch((size_t)n * 4) : NULL;
    if (t) {
        glGenTextures(n, t);
        gwrite(c->gpr[4], t, n, 4);
        for (GLsizei i = 0; i < n; i++)
            name_add(cur->group, t[i]);
    }
    DONE();
}
static void w_glDeleteTextures(ppc_cpu *c)
{
    ENTER();
    GLsizei n = (GLsizei)c->gpr[3];
    GLuint *t = n > 0 ? scratch((size_t)n * 4) : NULL;
    if (t) {
        gread(t, c->gpr[4], n, 4);
        glDeleteTextures(n, t);
        for (GLsizei i = 0; i < n; i++)
            name_remove(cur->group, t[i]);
    }
    DONE();
}
static void w_glAreTexturesResident(ppc_cpu *c)
{
    ENTER();
    GLsizei n = (GLsizei)c->gpr[3];
    GLuint *t = n > 0 ? scratch((size_t)n * 4) : NULL;
    GLboolean r = GL_TRUE;
    if (t) {
        gread(t, c->gpr[4], n, 4);
        r = glAreTexturesResident(n, t, host_ptr(c->gpr[5]));
    }
    c->gpr[3] = r;
    DONE();
}
static void w_glPrioritizeTextures(ppc_cpu *c)
{
    ENTER();
    GLsizei n = (GLsizei)c->gpr[3];
    GLuint *t = n > 0 ? scratch((size_t)n * 8) : NULL;
    if (t) {
        GLclampf *p = (GLclampf *)(t + n);
        gread(t, c->gpr[4], n, 4);
        gread(p, c->gpr[5], n, 4);
        glPrioritizeTextures(n, t, p);
    }
    DONE();
}

static void w_glCallLists(ppc_cpu *c)
{
    ENTER();
    GLsizei n = (GLsizei)c->gpr[3];
    GLenum type = (GLenum)c->gpr[4];
    gaddr lists = c->gpr[5];
    size_t size = type == GL_SHORT || type == GL_UNSIGNED_SHORT ? 2
                  : type == GL_INT || type == GL_UNSIGNED_INT || type == GL_FLOAT ? 4
                                                                                   : 0;
    if (n > 0 && lists) {
        if (!size) { /* bytes, and GL_2_BYTES and the like, which are big-endian by definition */
            size_t bytes = type == GL_2_BYTES ? 2 : type == GL_3_BYTES ? 3 : type == GL_4_BYTES ? 4 : 1;
            if (cur->recording)
                record(OP_glCallLists, &c->gpr[3], 2, gptr(lists), (uint32_t)(bytes * (size_t)n));
            glCallLists(n, type, gptr(lists));
        } else {
            void *t = scratch((size_t)n * size);
            if (t) {
                gread(t, lists, n, size);
                if (cur->recording)
                    record(OP_glCallLists, &c->gpr[3], 2, t, (uint32_t)(size * (size_t)n));
                glCallLists(n, type, t);
            }
        }
    }
    DONE();
}

/* ---- Pixel maps and evaluators ---- */

static GLint pixel_map_size(GLenum map)
{
    GLint n = 0;
    glGetIntegerv(map + (GL_PIXEL_MAP_I_TO_I_SIZE - GL_PIXEL_MAP_I_TO_I), &n);
    return n;
}

#define DEFINE_PIXELMAP(name, getname, T)                                                                             \
    static void w_##name(ppc_cpu *c)                                                                                  \
    {                                                                                                                 \
        ENTER();                                                                                                      \
        GLsizei n = (GLsizei)c->gpr[4];                                                                               \
        T *t = n > 0 ? scratch((size_t)n * sizeof(T)) : NULL;                                                         \
        if (t) {                                                                                                      \
            gread(t, c->gpr[5], n, sizeof(T));                                                                        \
            if (cur->recording)                                                                                       \
                record(OP_##name, &c->gpr[3], 2, t, (uint32_t)((size_t)n * sizeof(T)));                               \
            name((GLenum)c->gpr[3], n, t);                                                                            \
        }                                                                                                             \
        DONE();                                                                                                       \
    }                                                                                                                 \
    static void w_##getname(ppc_cpu *c)                                                                               \
    {                                                                                                                 \
        ENTER();                                                                                                      \
        GLint n = pixel_map_size((GLenum)c->gpr[3]);                                                                  \
        T *t = n > 0 ? scratch((size_t)n * sizeof(T)) : NULL;                                                         \
        if (t) {                                                                                                      \
            getname((GLenum)c->gpr[3], t);                                                                            \
            gwrite(c->gpr[4], t, n, sizeof(T));                                                                       \
        }                                                                                                             \
        DONE();                                                                                                       \
    }
DEFINE_PIXELMAP(glPixelMapfv, glGetPixelMapfv, GLfloat)
DEFINE_PIXELMAP(glPixelMapuiv, glGetPixelMapuiv, GLuint)
DEFINE_PIXELMAP(glPixelMapusv, glGetPixelMapusv, GLushort)

static void record_map(int op, const char *sig, const uint32_t *iv, const double *fv, const void *data, uint32_t n);

/* The values of one point of an evaluator's map. */
static int map_dims(GLenum target)
{
    switch (target) {
    case GL_MAP1_INDEX: case GL_MAP2_INDEX: case GL_MAP1_TEXTURE_COORD_1: case GL_MAP2_TEXTURE_COORD_1: return 1;
    case GL_MAP1_TEXTURE_COORD_2: case GL_MAP2_TEXTURE_COORD_2: return 2;
    case GL_MAP1_VERTEX_3: case GL_MAP2_VERTEX_3: case GL_MAP1_NORMAL: case GL_MAP2_NORMAL:
    case GL_MAP1_TEXTURE_COORD_3: case GL_MAP2_TEXTURE_COORD_3:
        return 3;
    default: return 4;
    }
}

#define DEFINE_MAP(name, T, sig1, sig2, host1, host2)                                                                 \
    static void w_##name##1##host1(ppc_cpu *c)                                                                        \
    {                                                                                                                 \
        uint32_t iv[16];                                                                                              \
        double fv[16];                                                                                                \
        ENTER();                                                                                                      \
        args_get(c, sig1, iv, fv);                                                                                    \
        GLint stride = I(3), order = I(4);                                                                            \
        int n = order > 0 && stride > 0 ? (order - 1) * stride + map_dims(U(0)) : 0;                                  \
        T *t = n > 0 ? scratch((size_t)n * sizeof(T)) : NULL;                                                         \
        if (t) {                                                                                                      \
            gread(t, iv[5], n, sizeof(T));                                                                            \
            if (cur->recording)                                                                                       \
                record_map(OP_##name##1##host1, sig1, iv, fv, t, (uint32_t)((size_t)n * sizeof(T)));                  \
            glMap1##host1(U(0), (T)D(1), (T)D(2), stride, order, t);                                                  \
        }                                                                                                             \
        DONE();                                                                                                       \
    }                                                                                                                 \
    static void w_##name##2##host2(ppc_cpu *c)                                                                        \
    {                                                                                                                 \
        uint32_t iv[16];                                                                                              \
        double fv[16];                                                                                                \
        ENTER();                                                                                                      \
        args_get(c, sig2, iv, fv);                                                                                    \
        GLint us = I(3), uo = I(4), vs = I(7), vo = I(8);                                                             \
        int n = uo > 0 && vo > 0 && us > 0 && vs > 0 ? (uo - 1) * us + (vo - 1) * vs + map_dims(U(0)) : 0;            \
        T *t = n > 0 ? scratch((size_t)n * sizeof(T)) : NULL;                                                         \
        if (t) {                                                                                                      \
            gread(t, iv[9], n, sizeof(T));                                                                            \
            if (cur->recording)                                                                                       \
                record_map(OP_##name##2##host2, sig2, iv, fv, t, (uint32_t)((size_t)n * sizeof(T)));                  \
            glMap2##host2(U(0), (T)D(1), (T)D(2), us, uo, (T)D(5), (T)D(6), vs, vo, t);                               \
        }                                                                                                             \
        DONE();                                                                                                       \
    }
DEFINE_MAP(glMap, GLdouble, "iddiii", "iddiiddiii", d, d)
DEFINE_MAP(glMap, GLfloat, "iffiii", "iffiiffiii", f, f)

/* glGetMap: GL_ORDER is one or two values, GL_DOMAIN two or four, GL_COEFF
 * the whole map. */
static int map_values(GLenum target, GLenum query)
{
    bool two = target >= GL_MAP2_COLOR_4 && target <= GL_MAP2_VERTEX_4;
    if (query == GL_ORDER)
        return two ? 2 : 1;
    if (query == GL_DOMAIN)
        return two ? 4 : 2;
    GLint order[2] = { 0, 1 };
    glGetMapiv(target, GL_ORDER, order);
    return order[0] * (two ? order[1] : 1) * map_dims(target);
}
#define DEFINE_GETMAP(name, T)                                                                                        \
    static void w_##name(ppc_cpu *c)                                                                                  \
    {                                                                                                                 \
        ENTER();                                                                                                      \
        int n = map_values((GLenum)c->gpr[3], (GLenum)c->gpr[4]);                                                     \
        T *t = n > 0 ? scratch((size_t)n * sizeof(T)) : NULL;                                                         \
        if (t) {                                                                                                      \
            name((GLenum)c->gpr[3], (GLenum)c->gpr[4], t);                                                            \
            gwrite(c->gpr[5], t, n, sizeof(T));                                                                       \
        }                                                                                                             \
        DONE();                                                                                                       \
    }
DEFINE_GETMAP(glGetMapdv, GLdouble)
DEFINE_GETMAP(glGetMapfv, GLfloat)
DEFINE_GETMAP(glGetMapiv, GLint)

/* ---- Gets ---- */

static garray *array_of(GLenum which);

/* How many values glGet answers for pname. */
static int get_count(GLenum pname)
{
    switch (pname) {
    case GL_MODELVIEW_MATRIX: case GL_PROJECTION_MATRIX: case GL_TEXTURE_MATRIX: case GL_COLOR_MATRIX:
    case GL_TRANSPOSE_MODELVIEW_MATRIX: case GL_TRANSPOSE_PROJECTION_MATRIX: case GL_TRANSPOSE_TEXTURE_MATRIX:
    case GL_TRANSPOSE_COLOR_MATRIX:
        return 16;
    case GL_VIEWPORT: case GL_SCISSOR_BOX: case GL_COLOR_CLEAR_VALUE: case GL_COLOR_WRITEMASK: case GL_CURRENT_COLOR:
    case GL_CURRENT_TEXTURE_COORDS: case GL_CURRENT_RASTER_COLOR: case GL_CURRENT_RASTER_POSITION:
    case GL_CURRENT_RASTER_TEXTURE_COORDS: case GL_FOG_COLOR: case GL_LIGHT_MODEL_AMBIENT: case GL_ACCUM_CLEAR_VALUE:
    case GL_BLEND_COLOR: case GL_MAP2_GRID_DOMAIN:
        return 4;
    case GL_CURRENT_NORMAL:
        return 3;
    case GL_DEPTH_RANGE: case GL_MAX_VIEWPORT_DIMS: case GL_POLYGON_MODE: case GL_LINE_WIDTH_RANGE:
    case GL_POINT_SIZE_RANGE: case GL_ALIASED_LINE_WIDTH_RANGE: case GL_ALIASED_POINT_SIZE_RANGE:
    case GL_MAP1_GRID_DOMAIN: case GL_MAP2_GRID_SEGMENTS:
        return 2;
    default:
        return 1;
    }
}

/* Values the program sees differently from the host: the buffers it drew
 * to, its own byte swapping, its arrays, and the era's texture size. */
static bool get_fixed(GLenum pname, double *v)
{
    GLint i = 0;
    garray *a;
    switch (pname) {
    case GL_DRAW_BUFFER: *v = cur->draw_buffer; return true;
    case GL_READ_BUFFER: *v = cur->read_buffer; return true;
    case GL_UNPACK_SWAP_BYTES: case GL_PACK_SWAP_BYTES:
        glGetIntegerv(pname, &i);
        *v = !i;
        return true;
    case GL_MAX_TEXTURE_SIZE:
        glGetIntegerv(pname, &i);
        *v = i > 2048 ? 2048 : i;
        return true;
    case GL_DOUBLEBUFFER: *v = 1; return true;
    case GL_VERTEX_ARRAY_SIZE: case GL_COLOR_ARRAY_SIZE: case GL_TEXTURE_COORD_ARRAY_SIZE:
        a = array_of(pname == GL_VERTEX_ARRAY_SIZE ? GL_VERTEX_ARRAY : pname == GL_COLOR_ARRAY_SIZE ? GL_COLOR_ARRAY
                                                                                                   : GL_TEXTURE_COORD_ARRAY);
        *v = a->size;
        return true;
    case GL_VERTEX_ARRAY_TYPE: case GL_NORMAL_ARRAY_TYPE: case GL_COLOR_ARRAY_TYPE: case GL_INDEX_ARRAY_TYPE:
    case GL_TEXTURE_COORD_ARRAY_TYPE:
        a = array_of(pname == GL_VERTEX_ARRAY_TYPE   ? GL_VERTEX_ARRAY
                     : pname == GL_NORMAL_ARRAY_TYPE ? GL_NORMAL_ARRAY
                     : pname == GL_COLOR_ARRAY_TYPE  ? GL_COLOR_ARRAY
                     : pname == GL_INDEX_ARRAY_TYPE  ? GL_INDEX_ARRAY
                                                     : GL_TEXTURE_COORD_ARRAY);
        *v = a->type;
        return true;
    case GL_VERTEX_ARRAY_STRIDE: case GL_NORMAL_ARRAY_STRIDE: case GL_COLOR_ARRAY_STRIDE: case GL_INDEX_ARRAY_STRIDE:
    case GL_TEXTURE_COORD_ARRAY_STRIDE: case GL_EDGE_FLAG_ARRAY_STRIDE:
        a = array_of(pname == GL_VERTEX_ARRAY_STRIDE   ? GL_VERTEX_ARRAY
                     : pname == GL_NORMAL_ARRAY_STRIDE ? GL_NORMAL_ARRAY
                     : pname == GL_COLOR_ARRAY_STRIDE  ? GL_COLOR_ARRAY
                     : pname == GL_INDEX_ARRAY_STRIDE  ? GL_INDEX_ARRAY
                     : pname == GL_EDGE_FLAG_ARRAY_STRIDE ? GL_EDGE_FLAG_ARRAY
                                                          : GL_TEXTURE_COORD_ARRAY);
        *v = a->stride;
        return true;
    default:
        return false;
    }
}

#define DEFINE_GET(name, T, put)                                                                                      \
    static void w_##name(ppc_cpu *c)                                                                                  \
    {                                                                                                                 \
        ENTER();                                                                                                      \
        GLenum pname = (GLenum)c->gpr[3];                                                                             \
        gaddr out = c->gpr[4];                                                                                        \
        T v[64];                                                                                                      \
        memset(v, 0, sizeof v);                                                                                       \
        double fixed;                                                                                                 \
        int n = get_count(pname);                                                                                     \
        if (get_fixed(pname, &fixed))                                                                                 \
            v[0] = (T)fixed;                                                                                          \
        else                                                                                                          \
            name(pname, v);                                                                                           \
        for (int k = 0; out && k < n; k++)                                                                            \
            put;                                                                                                      \
        DONE();                                                                                                       \
    }
DEFINE_GET(glGetBooleanv, GLboolean, wr8(out + (uint32_t)k, v[k] != 0))
DEFINE_GET(glGetIntegerv, GLint, wr32(out + 4 * (uint32_t)k, (uint32_t)v[k]))
DEFINE_GET(glGetFloatv, GLfloat, put_float(out + 4 * (uint32_t)k, v[k]))
DEFINE_GET(glGetDoublev, GLdouble, put_double(out + 8 * (uint32_t)k, v[k]))

/* Strings: Mac OS 9's OpenGL 1.2.1 and the extensions there are calls for. */
static gaddr strings[8];

static void w_glGetString(ppc_cpu *c)
{
    ENTER();
    switch (c->gpr[3]) {
    case GL_VENDOR: c->gpr[3] = guest_string(&strings[0], "Apple"); break;
    case GL_RENDERER: c->gpr[3] = guest_string(&strings[1], "Blue Box OpenGL"); break;
    case GL_VERSION: c->gpr[3] = guest_string(&strings[2], "1.2.1"); break;
    case GL_EXTENSIONS:
        c->gpr[3] = guest_string(&strings[3], "GL_ARB_multitexture GL_ARB_transpose_matrix GL_ARB_texture_env_add "
                                              "GL_EXT_bgra GL_EXT_blend_color GL_EXT_blend_minmax "
                                              "GL_EXT_blend_subtract GL_EXT_compiled_vertex_array "
                                              "GL_EXT_packed_pixels GL_APPLE_packed_pixels GL_EXT_rescale_normal "
                                              "GL_EXT_texture_env_add GL_SGIS_texture_edge_clamp "
                                              "GL_EXT_texture3D GL_SGIS_generate_mipmap");
        break;
    default: c->gpr[3] = 0; break;
    }
    DONE();
}

/* ---- Selection and feedback ---- */

static void w_glSelectBuffer(ppc_cpu *c)
{
    ENTER();
    GLsizei n = (GLsizei)c->gpr[3];
    bb_free(cur->select);
    cur->select = n > 0 ? bb_calloc((size_t)n, 4, BB_BYTES) : NULL;
    cur->select_n = cur->select ? n : 0;
    cur->select_to = c->gpr[4];
    if (cur->select)
        glSelectBuffer(n, cur->select);
    DONE();
}
static void w_glFeedbackBuffer(ppc_cpu *c)
{
    ENTER();
    GLsizei n = (GLsizei)c->gpr[3];
    bb_free(cur->feedback);
    cur->feedback = n > 0 ? bb_calloc((size_t)n, 4, BB_BYTES) : NULL;
    cur->feedback_n = cur->feedback ? n : 0;
    cur->feedback_to = c->gpr[5];
    cur->feedback_type = (GLenum)c->gpr[4];
    if (cur->feedback)
        glFeedbackBuffer(n, (GLenum)c->gpr[4], cur->feedback);
    DONE();
}
/* Leaving selection or feedback mode: what the host wrote, to the program. */
static void w_glRenderMode(ppc_cpu *c)
{
    ENTER();
    GLenum was = cur->render_mode;
    GLint r = glRenderMode((GLenum)c->gpr[3]);
    GLenum err = glGetError();
    if (err == GL_NO_ERROR)
        cur->render_mode = (GLenum)c->gpr[3];
    if (was == GL_SELECT && cur->select && cur->select_to) {
        int used = 0;
        for (int hit = 0; hit < (r < 0 ? 0 : r) && used < cur->select_n; hit++)
            used += 3 + (int)cur->select[used];
        if (r < 0)
            used = cur->select_n;
        gwrite(cur->select_to, cur->select, used < cur->select_n ? used : cur->select_n, 4);
    } else if (was == GL_FEEDBACK && cur->feedback && cur->feedback_to) {
        int used = r < 0 ? cur->feedback_n : r;
        gwrite(cur->feedback_to, cur->feedback, used < cur->feedback_n ? used : cur->feedback_n, 4);
    }
    c->gpr[3] = (uint32_t)r;
    DONE();
}

/* ---- Vertex arrays ---- */

static size_t type_size(GLenum t)
{
    switch (t) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: return 4;
    case GL_DOUBLE: return 8;
    default: return 0;
    }
}

static garray *array_of(GLenum which)
{
    switch (which) {
    case GL_VERTEX_ARRAY: return &cur->a.vertex;
    case GL_NORMAL_ARRAY: return &cur->a.normal;
    case GL_COLOR_ARRAY: return &cur->a.color;
    case GL_INDEX_ARRAY: return &cur->a.index;
    case GL_EDGE_FLAG_ARRAY: return &cur->a.edge;
    case GL_TEXTURE_COORD_ARRAY: return &cur->a.tex[cur->client_unit];
    default: return NULL;
    }
}

static void set_array(GLenum which, GLint size, GLenum type, GLsizei stride, gaddr ptr)
{
    garray *a = array_of(which);
    a->size = size;
    a->type = type;
    a->stride = stride;
    a->ptr = ptr;
}

static void w_glVertexPointer(ppc_cpu *c)
{
    ENTER();
    set_array(GL_VERTEX_ARRAY, (GLint)c->gpr[3], (GLenum)c->gpr[4], (GLsizei)c->gpr[5], c->gpr[6]);
    DONE();
}
static void w_glColorPointer(ppc_cpu *c)
{
    ENTER();
    set_array(GL_COLOR_ARRAY, (GLint)c->gpr[3], (GLenum)c->gpr[4], (GLsizei)c->gpr[5], c->gpr[6]);
    DONE();
}
static void w_glTexCoordPointer(ppc_cpu *c)
{
    ENTER();
    set_array(GL_TEXTURE_COORD_ARRAY, (GLint)c->gpr[3], (GLenum)c->gpr[4], (GLsizei)c->gpr[5], c->gpr[6]);
    DONE();
}
static void w_glNormalPointer(ppc_cpu *c)
{
    ENTER();
    set_array(GL_NORMAL_ARRAY, 3, (GLenum)c->gpr[3], (GLsizei)c->gpr[4], c->gpr[5]);
    DONE();
}
static void w_glIndexPointer(ppc_cpu *c)
{
    ENTER();
    set_array(GL_INDEX_ARRAY, 1, (GLenum)c->gpr[3], (GLsizei)c->gpr[4], c->gpr[5]);
    DONE();
}
static void w_glEdgeFlagPointer(ppc_cpu *c)
{
    ENTER();
    set_array(GL_EDGE_FLAG_ARRAY, 1, GL_UNSIGNED_BYTE, (GLsizei)c->gpr[3], c->gpr[4]);
    DONE();
}

static void client_state(GLenum which, bool on)
{
    garray *a = array_of(which);
    if (a)
        a->on = on;
    if (on)
        glEnableClientState(which);
    else
        glDisableClientState(which);
}
static void w_glEnableClientState(ppc_cpu *c)
{
    ENTER();
    client_state((GLenum)c->gpr[3], true);
    DONE();
}
static void w_glDisableClientState(ppc_cpu *c)
{
    ENTER();
    client_state((GLenum)c->gpr[3], false);
    DONE();
}
static void w_glClientActiveTextureARB(ppc_cpu *c)
{
    ENTER();
    GLenum unit = (GLenum)c->gpr[3];
    if (unit >= GL_TEXTURE0 && unit < GL_TEXTURE0 + MAX_UNITS) {
        cur->client_unit = unit - GL_TEXTURE0;
        glClientActiveTexture(unit);
    }
    DONE();
}

static void w_glPushClientAttrib(ppc_cpu *c)
{
    ENTER();
    if (cur->nsaved < CLIENT_STACK) {
        arrays *s = &cur->saved[cur->nsaved++];
        *s = cur->a;
        s->vertex.host = s->normal.host = s->color.host = s->index.host = s->edge.host = NULL;
        for (int i = 0; i < MAX_UNITS; i++)
            s->tex[i].host = NULL;
    }
    glPushClientAttrib((GLbitfield)c->gpr[3]);
    DONE();
}
static void w_glPopClientAttrib(ppc_cpu *c)
{
    ENTER();
    glPopClientAttrib();
    if (cur->nsaved > 0) {
        arrays *s = &cur->saved[--cur->nsaved];
#define RESTORE(f) do { void *h = cur->a.f.host; size_t n = cur->a.f.host_cap; cur->a.f = s->f; cur->a.f.host = h; cur->a.f.host_cap = n; } while (0)
        RESTORE(vertex);
        RESTORE(normal);
        RESTORE(color);
        RESTORE(index);
        RESTORE(edge);
        for (int i = 0; i < MAX_UNITS; i++)
            RESTORE(tex[i]);
#undef RESTORE
    }
    DONE();
}

static void w_glGetPointerv(ppc_cpu *c)
{
    ENTER();
    gaddr p = 0;
    switch (c->gpr[3]) {
    case GL_VERTEX_ARRAY_POINTER: p = cur->a.vertex.ptr; break;
    case GL_NORMAL_ARRAY_POINTER: p = cur->a.normal.ptr; break;
    case GL_COLOR_ARRAY_POINTER: p = cur->a.color.ptr; break;
    case GL_INDEX_ARRAY_POINTER: p = cur->a.index.ptr; break;
    case GL_EDGE_FLAG_ARRAY_POINTER: p = cur->a.edge.ptr; break;
    case GL_TEXTURE_COORD_ARRAY_POINTER: p = cur->a.tex[cur->client_unit].ptr; break;
    case GL_SELECTION_BUFFER_POINTER: p = cur->select_to; break;
    case GL_FEEDBACK_BUFFER_POINTER: p = cur->feedback_to; break;
    }
    if (c->gpr[4])
        wr32(c->gpr[4], p);
    DONE();
}

/* glInterleavedArrays: the formats' layouts, as the specification gives
 * them (texture coordinates, colour, normal and vertex: their sizes and
 * offsets, the colour's type, and the whole). */
static void w_glInterleavedArrays(ppc_cpu *c)
{
    ENTER();
    static const struct { GLenum f; int tc, cc, n, vc, pc, pn, pv, s; GLenum ct; } fmts[] = {
        { GL_V2F, 0, 0, 0, 2, 0, 0, 0, 8, 0 },
        { GL_V3F, 0, 0, 0, 3, 0, 0, 0, 12, 0 },
        { GL_C4UB_V2F, 0, 4, 0, 2, 0, 0, 4, 12, GL_UNSIGNED_BYTE },
        { GL_C4UB_V3F, 0, 4, 0, 3, 0, 0, 4, 16, GL_UNSIGNED_BYTE },
        { GL_C3F_V3F, 0, 3, 0, 3, 0, 0, 12, 24, GL_FLOAT },
        { GL_N3F_V3F, 0, 0, 1, 3, 0, 0, 12, 24, 0 },
        { GL_C4F_N3F_V3F, 0, 4, 1, 3, 0, 16, 28, 40, GL_FLOAT },
        { GL_T2F_V3F, 2, 0, 0, 3, 0, 0, 8, 20, 0 },
        { GL_T4F_V4F, 4, 0, 0, 4, 0, 0, 16, 32, 0 },
        { GL_T2F_C4UB_V3F, 2, 4, 0, 3, 8, 0, 12, 24, GL_UNSIGNED_BYTE },
        { GL_T2F_C3F_V3F, 2, 3, 0, 3, 8, 0, 20, 32, GL_FLOAT },
        { GL_T2F_N3F_V3F, 2, 0, 1, 3, 0, 8, 20, 32, 0 },
        { GL_T2F_C4F_N3F_V3F, 2, 4, 1, 3, 8, 24, 36, 48, GL_FLOAT },
        { GL_T4F_C4F_N3F_V4F, 4, 4, 1, 4, 16, 32, 44, 60, GL_FLOAT },
    };
    GLenum format = (GLenum)c->gpr[3];
    GLsizei stride = (GLsizei)c->gpr[4];
    gaddr p = c->gpr[5];
    for (unsigned i = 0; i < sizeof fmts / sizeof fmts[0]; i++) {
        if (fmts[i].f != format)
            continue;
        GLsizei s = stride ? stride : fmts[i].s;
        client_state(GL_EDGE_FLAG_ARRAY, false);
        client_state(GL_INDEX_ARRAY, false);
        client_state(GL_TEXTURE_COORD_ARRAY, fmts[i].tc != 0);
        if (fmts[i].tc)
            set_array(GL_TEXTURE_COORD_ARRAY, fmts[i].tc, GL_FLOAT, s, p);
        client_state(GL_COLOR_ARRAY, fmts[i].cc != 0);
        if (fmts[i].cc)
            set_array(GL_COLOR_ARRAY, fmts[i].cc, fmts[i].ct, s, p + (uint32_t)fmts[i].pc);
        client_state(GL_NORMAL_ARRAY, fmts[i].n != 0);
        if (fmts[i].n)
            set_array(GL_NORMAL_ARRAY, 3, GL_FLOAT, s, p + (uint32_t)fmts[i].pn);
        client_state(GL_VERTEX_ARRAY, true);
        set_array(GL_VERTEX_ARRAY, fmts[i].vc, GL_FLOAT, s, p + (uint32_t)fmts[i].pv);
        break;
    }
    DONE();
}

/* Elements first to first + count - 1 of an array, in host order, packed. */
static const void *convert(garray *a, int first, int count)
{
    size_t ts = type_size(a->type), es = ts * (size_t)a->size;
    size_t stride = a->stride ? (size_t)a->stride : es;
    size_t need = es * (size_t)count;
    if (!ts || !a->ptr || count <= 0)
        return NULL;
    if (need > a->host_cap) {
        void *n = bb_realloc(a->host, need, BB_BYTES);
        if (!n)
            return NULL;
        a->host = n;
        a->host_cap = need;
    }
    uint8_t *d = a->host;
    gaddr s = a->ptr + (uint32_t)(stride * (size_t)first);
    if (ts == 1 && stride == es)
        memcpy(d, gptr(s), need);
    else
        for (int i = 0; i < count; i++, s += (uint32_t)stride, d += es)
            gread(d, s, a->size, ts);
    return a->host;
}

/* The program's enabled arrays as host arrays of elements first on. */
static bool arrays_to_host(int first, int count)
{
    const void *p;
    arrays *a = &cur->a;
    if (!a->vertex.on || !(p = convert(&a->vertex, first, count)))
        return false;
    glVertexPointer(a->vertex.size, a->vertex.type, 0, p);
    if (a->normal.on && (p = convert(&a->normal, first, count)))
        glNormalPointer(a->normal.type, 0, p);
    if (a->color.on && (p = convert(&a->color, first, count)))
        glColorPointer(a->color.size, a->color.type, 0, p);
    if (a->index.on && (p = convert(&a->index, first, count)))
        glIndexPointer(a->index.type, 0, p);
    if (a->edge.on && (p = convert(&a->edge, first, count)))
        glEdgeFlagPointer(0, p);
    for (int u = 0; u < MAX_UNITS; u++)
        if (a->tex[u].on && (p = convert(&a->tex[u], first, count))) {
            glClientActiveTexture(GL_TEXTURE0 + (GLenum)u);
            glTexCoordPointer(a->tex[u].size, a->tex[u].type, 0, p);
        }
    glClientActiveTexture(GL_TEXTURE0 + cur->client_unit);
    return true;
}

/* A draw from the arrays into a list: the elements arrays_to_host
 * converted (count of each enabled array), and the indices (none for
 * glDrawArrays). */
static void record_arrays(GLenum mode, int count, const GLuint *idx, int nidx)
{
    gllist *l = &cur->compiling;
    arrays *a = &cur->a;
    struct which { garray *a; GLenum which; int unit; } list[5 + MAX_UNITS] = {
        { &a->vertex, GL_VERTEX_ARRAY, 0 }, { &a->normal, GL_NORMAL_ARRAY, 0 }, { &a->color, GL_COLOR_ARRAY, 0 },
        { &a->index, GL_INDEX_ARRAY, 0 }, { &a->edge, GL_EDGE_FLAG_ARRAY, 0 },
    };
    for (int u = 0; u < MAX_UNITS; u++)
        list[5 + u] = (struct which){ &a->tex[u], GL_TEXTURE_COORD_ARRAY, u };
    uint32_t at = rec_open(l, OP_arrays);
    rec_u32(l, mode);
    rec_u32(l, (uint32_t)count);
    rec_u32(l, (uint32_t)nidx);
    rec_put(l, idx, 4 * (uint32_t)nidx);
    for (unsigned k = 0; k < sizeof list / sizeof list[0]; k++) {
        garray *g = list[k].a;
        size_t es = type_size(g->type) * (size_t)g->size;
        if (!g->on || !g->ptr || !g->host || !es)
            continue;
        uint32_t n = (uint32_t)(es * (size_t)count), pad = (8 - (n & 7)) & 7;
        uint32_t head[6] = { list[k].which, (uint32_t)list[k].unit, (uint32_t)g->size, g->type, n, pad };
        rec_put(l, head, sizeof head);
        rec_put(l, g->host, n);
        static const uint8_t zeros[8];
        rec_put(l, zeros, pad);
    }
    rec_close(l, at);
}

static void w_glDrawArrays(ppc_cpu *c)
{
    ENTER();
    GLint first = (GLint)c->gpr[4];
    GLsizei count = (GLsizei)c->gpr[5];
    if (first >= 0 && count > 0 && arrays_to_host(first, count)) {
        if (cur->recording)
            record_arrays((GLenum)c->gpr[3], count, NULL, 0);
        glDrawArrays((GLenum)c->gpr[3], 0, count);
    }
    DONE();
}

/* Indices in host order, less the least of them (the arrays converted
 * start there). */
static void draw_elements(GLenum mode, GLsizei count, GLenum type, gaddr indices)
{
    size_t ts = type_size(type);
    if (count <= 0 || !indices || (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_INT))
        return;
    GLuint *idx = scratch((size_t)count * 4);
    if (!idx)
        return;
    GLuint lo = 0xFFFFFFFFu, hi = 0;
    for (GLsizei i = 0; i < count; i++) {
        gaddr a = indices + (uint32_t)((size_t)i * ts);
        GLuint v = ts == 1 ? rd8(a) : ts == 2 ? rd16(a) : rd32(a);
        idx[i] = v;
        if (v < lo)
            lo = v;
        if (v > hi)
            hi = v;
    }
    if (hi - lo > 0x1000000u)
        return;
    for (GLsizei i = 0; i < count; i++)
        idx[i] -= lo;
    if (arrays_to_host((int)lo, (int)(hi - lo + 1))) {
        if (cur->recording)
            record_arrays(mode, (int)(hi - lo + 1), idx, count);
        glDrawElements(mode, count, GL_UNSIGNED_INT, idx);
    }
}

static void w_glDrawElements(ppc_cpu *c)
{
    ENTER();
    draw_elements((GLenum)c->gpr[3], (GLsizei)c->gpr[4], (GLenum)c->gpr[5], c->gpr[6]);
    DONE();
}
static void w_glDrawRangeElements(ppc_cpu *c)
{
    ENTER();
    draw_elements((GLenum)c->gpr[3], (GLsizei)c->gpr[6], (GLenum)c->gpr[7], c->gpr[8]);
    DONE();
}

/* glArrayElement: the element's values as immediate-mode calls, the
 * vertex last. */
static bool element(garray *a, int i, void *out)
{
    size_t ts = type_size(a->type);
    if (!a->on || !a->ptr || !ts)
        return false;
    size_t stride = a->stride ? (size_t)a->stride : ts * (size_t)a->size;
    gread(out, a->ptr + (uint32_t)(stride * (size_t)i), a->size, ts);
    return true;
}

/* An element's values, as glArrayElement gives them to the host. */
typedef union {
    GLbyte b[4];
    GLubyte ub[4];
    GLshort s[4];
    GLushort us[4];
    GLint i[4];
    GLuint ui[4];
    GLfloat f[4];
    GLdouble d[4];
} evalue;
typedef struct {
    uint32_t has; /* EV_ bits, and 1 << (EV_TEX + unit) */
    GLenum color_type, index_type, normal_type;
    int color_n;
    GLubyte edge;
    evalue color, index, normal;
    GLdouble tex[MAX_UNITS][4], vertex[4];
} element_values;
enum { EV_EDGE = 1, EV_COLOR = 2, EV_INDEX = 4, EV_NORMAL = 8, EV_VERTEX = 16, EV_TEX = 5 };

static void element_get(int i, element_values *e)
{
    arrays *a = &cur->a;
    evalue v;
    memset(e, 0, sizeof *e);
    if (element(&a->edge, i, &v)) {
        e->has |= EV_EDGE;
        e->edge = v.ub[0];
    }
    for (int u = 0; u < MAX_UNITS; u++)
        if (element(&a->tex[u], i, &v)) {
            e->has |= 1u << (EV_TEX + u);
            GLdouble *t = e->tex[u];
            t[0] = t[1] = t[2] = 0;
            t[3] = 1;
            for (int k = 0; k < a->tex[u].size && k < 4; k++)
                t[k] = a->tex[u].type == GL_SHORT ? v.s[k] : a->tex[u].type == GL_INT ? v.i[k]
                       : a->tex[u].type == GL_FLOAT ? v.f[k] : v.d[k];
        }
    if (element(&a->color, i, &e->color)) {
        e->has |= EV_COLOR;
        e->color_type = a->color.type;
        e->color_n = a->color.size;
    }
    if (element(&a->index, i, &e->index)) {
        e->has |= EV_INDEX;
        e->index_type = a->index.type;
    }
    if (element(&a->normal, i, &e->normal)) {
        e->has |= EV_NORMAL;
        e->normal_type = a->normal.type;
    }
    if (element(&a->vertex, i, &v)) {
        e->has |= EV_VERTEX;
        GLdouble *p = e->vertex;
        p[0] = p[1] = p[2] = 0;
        p[3] = 1;
        for (int k = 0; k < a->vertex.size && k < 4; k++)
            p[k] = a->vertex.type == GL_SHORT ? v.s[k] : a->vertex.type == GL_INT ? v.i[k]
                   : a->vertex.type == GL_FLOAT ? v.f[k] : v.d[k];
    }
}

/* The values as immediate-mode calls, the vertex last. */
static void element_put(const element_values *e)
{
    if (e->has & EV_EDGE)
        glEdgeFlag(e->edge);
    for (int u = 0; u < MAX_UNITS; u++)
        if (e->has & (1u << (EV_TEX + u)))
            glMultiTexCoord4dv(GL_TEXTURE0 + (GLenum)u, e->tex[u]);
    if (e->has & EV_COLOR) {
        bool four = e->color_n == 4;
        const evalue *v = &e->color;
        switch (e->color_type) {
        case GL_BYTE: four ? glColor4bv(v->b) : glColor3bv(v->b); break;
        case GL_UNSIGNED_BYTE: four ? glColor4ubv(v->ub) : glColor3ubv(v->ub); break;
        case GL_SHORT: four ? glColor4sv(v->s) : glColor3sv(v->s); break;
        case GL_UNSIGNED_SHORT: four ? glColor4usv(v->us) : glColor3usv(v->us); break;
        case GL_INT: four ? glColor4iv(v->i) : glColor3iv(v->i); break;
        case GL_UNSIGNED_INT: four ? glColor4uiv(v->ui) : glColor3uiv(v->ui); break;
        case GL_FLOAT: four ? glColor4fv(v->f) : glColor3fv(v->f); break;
        case GL_DOUBLE: four ? glColor4dv(v->d) : glColor3dv(v->d); break;
        }
    }
    if (e->has & EV_INDEX) {
        const evalue *v = &e->index;
        switch (e->index_type) {
        case GL_UNSIGNED_BYTE: glIndexub(v->ub[0]); break;
        case GL_SHORT: glIndexs(v->s[0]); break;
        case GL_INT: glIndexi(v->i[0]); break;
        case GL_FLOAT: glIndexf(v->f[0]); break;
        case GL_DOUBLE: glIndexd(v->d[0]); break;
        }
    }
    if (e->has & EV_NORMAL) {
        const evalue *v = &e->normal;
        switch (e->normal_type) {
        case GL_BYTE: glNormal3bv(v->b); break;
        case GL_SHORT: glNormal3sv(v->s); break;
        case GL_INT: glNormal3iv(v->i); break;
        case GL_FLOAT: glNormal3fv(v->f); break;
        case GL_DOUBLE: glNormal3dv(v->d); break;
        }
    }
    if (e->has & EV_VERTEX)
        glVertex4dv(e->vertex);
}

static void w_glArrayElement(ppc_cpu *c)
{
    ENTER();
    element_values e;
    element_get((int)c->gpr[3], &e);
    if (cur->recording)
        record(OP_element, NULL, 0, &e, sizeof e);
    element_put(&e);
    DONE();
}

/* ---- GLU ---- */

static gaddr glu_strings[16];

static void w_gluErrorString(ppc_cpu *c)
{
    ENTER();
    GLenum e = (GLenum)c->gpr[3];
    const GLubyte *s = gluErrorString(e);
    unsigned k = (e == GL_NO_ERROR ? 0 : e >= GL_INVALID_ENUM && e <= GL_OUT_OF_MEMORY ? 1 + e - GL_INVALID_ENUM
                                      : e >= GLU_INVALID_ENUM && e <= GLU_OUT_OF_MEMORY ? 8 + e - GLU_INVALID_ENUM
                                                                                         : 15);
    c->gpr[3] = s ? guest_string(&glu_strings[k], (const char *)s) : 0;
    DONE();
}
static void w_gluGetString(ppc_cpu *c)
{
    static gaddr version, extensions;
    ENTER();
    c->gpr[3] = c->gpr[3] == GLU_VERSION ? guest_string(&version, "1.2")
                : c->gpr[3] == GLU_EXTENSIONS ? guest_string(&extensions, "")
                                              : 0;
    DONE();
}

/* gluPickMatrix(x, y, width, height, viewport) */
static void w_gluPickMatrix(ppc_cpu *c)
{
    uint32_t iv[16];
    double fv[16];
    ENTER();
    args_get(c, "ddddi", iv, fv);
    GLint vp[4];
    gread(vp, iv[4], 4, 4);
    gluPickMatrix(D(0), D(1), D(2), D(3), vp);
    DONE();
}

/* gluProject and gluUnProject(x, y, z, model, proj, viewport, &x, &y, &z) */
static void project(ppc_cpu *c, bool un)
{
    uint32_t iv[16];
    double fv[16];
    args_get(c, "dddiiiiii", iv, fv);
    GLdouble model[16], proj[16], out[3] = { 0 };
    GLint vp[4];
    gread(model, iv[3], 16, 8);
    gread(proj, iv[4], 16, 8);
    gread(vp, iv[5], 4, 4);
    GLint r = (un ? gluUnProject : gluProject)(D(0), D(1), D(2), model, proj, vp, &out[0], &out[1], &out[2]);
    for (int k = 0; k < 3; k++)
        if (iv[6 + k])
            put_double(iv[6 + k], out[k]);
    c->gpr[3] = (uint32_t)r;
}
static void w_gluProject(ppc_cpu *c)
{
    ENTER();
    project(c, false);
    DONE();
}
static void w_gluUnProject(ppc_cpu *c)
{
    ENTER();
    project(c, true);
    DONE();
}

/* Quadrics: a system-heap word naming the host's, which a resumed
 * program has made again with its settings (quadric_settings) when it
 * next uses it. */
static GLUquadric *quadrics[MAX_QUADRICS];
static gaddr quadric_names[MAX_QUADRICS];

static int quadric_index(gaddr q)
{
    for (int i = 0; q && i < MAX_QUADRICS; i++)
        if (quadric_names[i] == q)
            return i;
    return -1;
}

static void quadric_set(GLUquadric *q, const uint32_t *s)
{
    gluQuadricDrawStyle(q, s[0]);
    gluQuadricNormals(q, s[1]);
    gluQuadricOrientation(q, s[2]);
    gluQuadricTexture(q, (GLboolean)s[3]);
}

static GLUquadric *quadric(gaddr q)
{
    int i = quadric_index(q);
    if (i < 0)
        return NULL;
    if (!quadrics[i] && (quadrics[i] = gluNewQuadric()) != NULL)
        quadric_set(quadrics[i], quadric_settings[i]);
    return quadrics[i];
}

static void w_gluNewQuadric(ppc_cpu *c)
{
    ENTER();
    c->gpr[3] = 0;
    for (int i = 0; i < MAX_QUADRICS; i++)
        if (!quadric_names[i]) {
            gaddr q = tb_NewPtr(4, true, true);
            if (q && (quadrics[i] = gluNewQuadric()) != NULL) {
                quadric_names[i] = q;
                quadric_settings[i][0] = GLU_FILL;
                quadric_settings[i][1] = GLU_SMOOTH;
                quadric_settings[i][2] = GLU_OUTSIDE;
                quadric_settings[i][3] = GL_FALSE;
                c->gpr[3] = q;
            }
            break;
        }
    DONE();
}
static void w_gluDeleteQuadric(ppc_cpu *c)
{
    ENTER();
    for (int i = 0; i < MAX_QUADRICS; i++)
        if (quadric_names[i] && quadric_names[i] == c->gpr[3]) {
            if (quadrics[i])
                gluDeleteQuadric(quadrics[i]);
            tb_DisposePtr(quadric_names[i]);
            quadrics[i] = NULL;
            quadric_names[i] = 0;
        }
    DONE();
}

GLU_QUADRICS(DEFINE)

/* ---------------------------------------------------------------------- */
/* Display lists, compiled again from their records (a resumed context) */

BB_HOST static GLUquadric *replay_q; /* a quadric set as a recorded draw had it */

static uint32_t unpack_args(const char *sig, const uint8_t *p, uint32_t len, uint32_t *iv, double *fv)
{
    uint32_t o = 0;
    for (int k = 0; sig[k] && o + 4 <= len; k++) {
        if (sig[k] == 'f') {
            float f;
            memcpy(&f, p + o, 4);
            fv[k] = f;
            o += 4;
        } else if (sig[k] == 'd') {
            memcpy(&fv[k], p + o, 8);
            o += 8;
        } else {
            memcpy(&iv[k], p + o, 4);
            o += 4;
        }
    }
    return o;
}

/* After an op's arguments, what else it read: the pixels (the unpacking
 * state set as it was), or the settings of the quadric it drew. */
static const void *replay_extra(const uint8_t *p, uint32_t len, uint32_t at)
{
    uint32_t kind = 0;
    if (at + 4 > len)
        return NULL;
    memcpy(&kind, p + at, 4);
    if (kind == EXTRA_PIXELS && at + 40 <= len) {
        GLint u[8];
        memcpy(u, p + at + 4, 32);
        for (int k = 0; k < 8; k++)
            glPixelStorei(unpack_names[k], u[k]);
        return p + at + 40;
    }
    if (kind == EXTRA_QUADRIC && at + 20 <= len) {
        uint32_t q[4];
        memcpy(q, p + at + 4, 16);
        if (!replay_q)
            replay_q = gluNewQuadric();
        if (replay_q)
            quadric_set(replay_q, q);
    }
    return NULL;
}

typedef void (*replay_fn)(const uint8_t *p, uint32_t len);

#undef P
#define P(k) ((void *)blob)
#undef Q
#define Q() GLUquadric *q = replay_q; if (q)
#define REPLAY(name, sig, call)                                                                                       \
    static void r_##name(const uint8_t *p, uint32_t len)                                                              \
    {                                                                                                                 \
        uint32_t iv[16] = { 0 };                                                                                      \
        double fv[16] = { 0 };                                                                                        \
        const void *blob = replay_extra(p, len, unpack_args(sig, p, len, iv, fv));                                    \
        (void)blob;                                                                                                   \
        call;                                                                                                         \
    }
#define REPLAY_RESULT(name, sig, expr) REPLAY(name, sig, (void)(expr))
#define REPLAY_V(name, n, T)                                                                                          \
    static void r_##name(const uint8_t *p, uint32_t len)                                                              \
    {                                                                                                                 \
        T t[4] = { 0 };                                                                                               \
        memcpy(t, p, len < sizeof t ? len : sizeof t);                                                                \
        name(t);                                                                                                      \
    }
#define REPLAY_T(name, host, n, T)                                                                                    \
    static void r_##name(const uint8_t *p, uint32_t len)                                                              \
    {                                                                                                                 \
        uint32_t e = 0;                                                                                               \
        T t[4] = { 0 };                                                                                               \
        memcpy(&e, p, 4);                                                                                             \
        memcpy(t, p + 4, len - 4 < sizeof t ? len - 4 : sizeof t);                                                    \
        host((GLenum)e, t);                                                                                           \
    }
#define REPLAY_R(name, T)                                                                                             \
    static void r_##name(const uint8_t *p, uint32_t len)                                                              \
    {                                                                                                                 \
        T ab[4] = { 0 };                                                                                              \
        memcpy(ab, p, len < sizeof ab ? len : sizeof ab);                                                             \
        name(ab, ab + 2);                                                                                             \
    }
#define REPLAY_PV(name, T)                                                                                            \
    static void r_##name(const uint8_t *p, uint32_t len)                                                              \
    {                                                                                                                 \
        uint32_t w[2] = { 0 };                                                                                        \
        T t[4] = { 0 };                                                                                               \
        memcpy(w, p, 8);                                                                                              \
        memcpy(t, p + 8, len - 8 < sizeof t ? len - 8 : sizeof t);                                                    \
        name((GLenum)w[0], (GLenum)w[1], t);                                                                          \
    }
#define REPLAY_PV1(name, T)                                                                                           \
    static void r_##name(const uint8_t *p, uint32_t len)                                                              \
    {                                                                                                                 \
        uint32_t w = 0;                                                                                               \
        T t[4] = { 0 };                                                                                               \
        memcpy(&w, p, 4);                                                                                             \
        memcpy(t, p + 4, len - 4 < sizeof t ? len - 4 : sizeof t);                                                    \
        name((GLenum)w, t);                                                                                           \
    }
#define REPLAY_NO(...)
#define REPLAY_MATRIX(name, host, T)                                                                                  \
    static void r_##name(const uint8_t *p, uint32_t len)                                                              \
    {                                                                                                                 \
        T m[16] = { 0 };                                                                                              \
        memcpy(m, p, len < sizeof m ? len : sizeof m);                                                                \
        host(m);                                                                                                      \
    }
GL_IMMEDIATE(REPLAY)
GL_IMMEDIATE_V(REPLAY_V, REPLAY_T, REPLAY_R)
GL_STATE(REPLAY)
GL_RESULTS(REPLAY_RESULT)
GL_PARAMS(REPLAY_PV, REPLAY_PV1, REPLAY_NO)
GL_MATRICES(REPLAY_MATRIX)
GLU_QUADRICS(REPLAY)
#undef P
#define P(k) host_ptr(iv[k])

static void r_glClipPlane(const uint8_t *p, uint32_t len)
{
    uint32_t plane = 0;
    GLdouble e[4] = { 0 };
    memcpy(&plane, p, 4);
    memcpy(e, p + 4, len >= 36 ? 32 : 0);
    glClipPlane(plane, e);
}

static void r_glCallLists(const uint8_t *p, uint32_t len)
{
    uint32_t w[2];
    if (len < 8)
        return;
    memcpy(w, p, 8);
    glCallLists((GLsizei)w[0], w[1], p + 8);
}

static void r_glDrawBuffer(const uint8_t *p, uint32_t len)
{
    uint32_t b = 0;
    memcpy(&b, p, len >= 4 ? 4 : 0);
    glDrawBuffer(b);
}

#define REPLAY_PIXELMAP(name, T)                                                                                      \
    static void r_##name(const uint8_t *p, uint32_t len)                                                              \
    {                                                                                                                 \
        uint32_t w[2];                                                                                                \
        if (len < 8)                                                                                                  \
            return;                                                                                                   \
        memcpy(w, p, 8);                                                                                              \
        name(w[0], (GLsizei)w[1], (const T *)(p + 8));                                                                \
    }
REPLAY_PIXELMAP(glPixelMapfv, GLfloat)
REPLAY_PIXELMAP(glPixelMapuiv, GLuint)
REPLAY_PIXELMAP(glPixelMapusv, GLushort)

/* glMap: the arguments, the values' size, the values. */
#define REPLAY_MAP(name, T, sig1, sig2, host1, host2)                                                                 \
    static void r_##name##1##host1(const uint8_t *p, uint32_t len)                                                    \
    {                                                                                                                 \
        uint32_t iv[16] = { 0 };                                                                                      \
        double fv[16] = { 0 };                                                                                        \
        uint32_t at = unpack_args(sig1, p, len, iv, fv) + 4;                                                          \
        if (at <= len)                                                                                                \
            glMap1##host1(U(0), (T)D(1), (T)D(2), I(3), I(4), (const T *)(p + at));                                   \
    }                                                                                                                 \
    static void r_##name##2##host2(const uint8_t *p, uint32_t len)                                                    \
    {                                                                                                                 \
        uint32_t iv[16] = { 0 };                                                                                      \
        double fv[16] = { 0 };                                                                                        \
        uint32_t at = unpack_args(sig2, p, len, iv, fv) + 4;                                                          \
        if (at <= len)                                                                                                \
            glMap2##host2(U(0), (T)D(1), (T)D(2), I(3), I(4), (T)D(5), (T)D(6), I(7), I(8), (const T *)(p + at));     \
    }
REPLAY_MAP(glMap, GLdouble, "iddiii", "iddiiddiii", d, d)
REPLAY_MAP(glMap, GLfloat, "iffiii", "iffiiffiii", f, f)

static void r_arrays(const uint8_t *p, uint32_t len)
{
    uint32_t h[3];
    if (len < 12)
        return;
    memcpy(h, p, 12);
    uint32_t at = 12 + 4 * h[2];
    if (at > len)
        return;
    const GLuint *idx = (const GLuint *)(p + 12);
    glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
    static const GLenum all[] = { GL_VERTEX_ARRAY, GL_NORMAL_ARRAY, GL_COLOR_ARRAY, GL_INDEX_ARRAY,
                                  GL_EDGE_FLAG_ARRAY };
    for (unsigned k = 0; k < sizeof all / sizeof all[0]; k++)
        glDisableClientState(all[k]);
    for (int u = 0; u < MAX_UNITS; u++) {
        glClientActiveTexture(GL_TEXTURE0 + (GLenum)u);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    while (at + 24 <= len) {
        uint32_t a[6];
        memcpy(a, p + at, 24);
        const void *data = p + at + 24;
        at += 24 + a[4] + a[5];
        if (at > len)
            break;
        switch (a[0]) {
        case GL_VERTEX_ARRAY: glVertexPointer((GLint)a[2], a[3], 0, data); break;
        case GL_NORMAL_ARRAY: glNormalPointer(a[3], 0, data); break;
        case GL_COLOR_ARRAY: glColorPointer((GLint)a[2], a[3], 0, data); break;
        case GL_INDEX_ARRAY: glIndexPointer(a[3], 0, data); break;
        case GL_EDGE_FLAG_ARRAY: glEdgeFlagPointer(0, data); break;
        case GL_TEXTURE_COORD_ARRAY:
            glClientActiveTexture(GL_TEXTURE0 + a[1]);
            glTexCoordPointer((GLint)a[2], a[3], 0, data);
            break;
        }
        glEnableClientState(a[0]);
    }
    if (h[2])
        glDrawElements(h[0], (GLsizei)h[2], GL_UNSIGNED_INT, idx);
    else
        glDrawArrays(h[0], 0, (GLsizei)h[1]);
    glPopClientAttrib();
}

static void r_element(const uint8_t *p, uint32_t len)
{
    element_values e;
    if (len < sizeof e)
        return;
    memcpy(&e, p, sizeof e);
    element_put(&e);
}

#define RP(name, ...) [OP_##name] = r_##name,
#define RP_NO(...)
static const replay_fn replays[OP_COUNT] = {
    GL_IMMEDIATE(RP) GL_IMMEDIATE_V(RP, RP, RP) GL_STATE(RP) GL_RESULTS(RP) GL_PARAMS(RP, RP, RP_NO)
    GL_MATRICES(RP) GLU_QUADRICS(RP)
    [OP_glClipPlane] = r_glClipPlane, [OP_glCallLists] = r_glCallLists, [OP_glDrawBuffer] = r_glDrawBuffer,
    [OP_glPixelMapfv] = r_glPixelMapfv, [OP_glPixelMapuiv] = r_glPixelMapuiv, [OP_glPixelMapusv] = r_glPixelMapusv,
    [OP_glMap1d] = r_glMap1d, [OP_glMap1f] = r_glMap1f, [OP_glMap2d] = r_glMap2d, [OP_glMap2f] = r_glMap2f,
    [OP_arrays] = r_arrays, [OP_element] = r_element,
};
#undef RP
#undef RP_NO

/* Each of the group's lists compiled again, in its new context. */
static void replay_lists(const glgroup *g)
{
    for (int i = 0; i < g->nlists; i++) {
        const gllist *l = &g->lists[i];
        glNewList(l->name, GL_COMPILE);
        for (uint32_t at = 0; l->ops && at + 8 <= l->len;) {
            uint16_t op;
            uint32_t len;
            memcpy(&op, l->ops + at, 2);
            memcpy(&len, l->ops + at + 4, 4);
            if (at + 8 + len > l->len)
                break;
            if (op < OP_COUNT && replays[op])
                replays[op](l->ops + at + 8, len);
            at += 8 + ((len + 7) & ~7u);
        }
        glEndList();
    }
    if (replay_q) {
        gluDeleteQuadric(replay_q);
        replay_q = NULL;
    }
}

/* ---------------------------------------------------------------------- */
/* Hibernating (opengl_hib.c has the state) */

static hostgl_ctx *live_mate(const hostgl_ctx *h)
{
    for (int i = 0; i < MAX_CONTEXTS; i++)
        if (contexts[i] && contexts[i] != h && contexts[i]->group == h->group && contexts[i]->cgl)
            return contexts[i];
    return NULL;
}

/* A resumed program's context, made again at its first use: a host
 * context sharing with any of its group made again already (else the
 * group's textures and lists given to this one), its framebuffer, then
 * its state. */
static bool rebuild(hostgl_ctx *h)
{
    if (!hostgl_available())
        return false;
    hostgl_ctx *mate = live_mate(h);
    if (CGLCreateContext(pixel_format, mate ? mate->cgl : NULL, &h->cgl) != kCGLNoError) {
        h->cgl = NULL;
        return false;
    }
    CGLSetCurrentContext(h->cgl);
    make_framebuffer(h);
    h->sized = false;
    hostgl_resize(h, h->width, h->height);
    CGLSetCurrentContext(h->cgl);
    size_t tex = glh_saved_texture_bytes(h->group), state = glh_saved_bytes(h);
    if (!mate) {
        glh_restore_textures(h->group);
        replay_lists(h->group);
    }
    glh_restore(h);
    if (h->select)
        glSelectBuffer(h->select_n, h->select);
    if (h->feedback)
        glFeedbackBuffer(h->feedback_n, h->feedback_type, h->feedback);
    LOGF(LOG_IDLE, "resume: OpenGL context made again (%dx%d; %zu bytes of state and framebuffer%s)\n", h->width,
         h->height, state, mate ? ", sharing" : "");
    if (!mate && (tex || h->group->nlists))
        LOGF(LOG_IDLE, "resume: OpenGL: %zu bytes of textures, %d display lists\n", tex, h->group->nlists);
    return true;
}

/* What keeps a context from being saved as it is, or NULL: for the drain
 * to wait on at a slice point, and refuse at an idle call. */
const char *hostgl_blocker(void)
{
    const char *why = NULL;
    CGLContextObj was = CGLGetCurrentContext();
    for (int i = 0; i < MAX_CONTEXTS && !why; i++) {
        hostgl_ctx *h = contexts[i];
        if (!h || !h->cgl)
            continue; /* resumed and not used yet: as the snapshot had it */
        if (h->in_begin)
            why = "is between glBegin and glEnd (OpenGL)";
        else if (h->recording)
            why = "is compiling an OpenGL display list";
        else if (h->render_mode != GL_RENDER)
            why = "has OpenGL in selection or feedback mode";
        else if (h->nsaved > 0)
            why = "has OpenGL client attributes pushed (glPushClientAttrib)";
        if (why)
            break;
        GLint depth = 0;
        CGLSetCurrentContext(h->cgl);
        glGetIntegerv(GL_ATTRIB_STACK_DEPTH, &depth);
        if (depth > 0)
            why = "has OpenGL attributes pushed (glPushAttrib)";
    }
    CGLSetCurrentContext(was);
    return why;
}

/* Before the snapshot: each context's state, and each group's textures,
 * into the machine.  From any thread, the machine standing still. */
void hostgl_before_hibernate(void)
{
    CGLContextObj was = CGLGetCurrentContext();
    for (int i = 0; i < MAX_CONTEXTS; i++) {
        hostgl_ctx *h = contexts[i];
        if (!h || !h->cgl || h->hib)
            continue;
        CGLLockContext(h->cgl);
        CGLSetCurrentContext(h->cgl);
        glh_capture(h);
        glh_capture_textures(h->group);
        CGLUnlockContext(h->cgl);
    }
    CGLSetCurrentContext(was);
}

/* Saved, and going on: what was read back let go of where the host still has it. */
void hostgl_after_hibernate(void)
{
    for (int i = 0; i < MAX_CONTEXTS; i++) {
        hostgl_ctx *h = contexts[i];
        if (h && h->cgl) {
            glh_free(h);
            glh_free_textures(h->group);
        }
    }
}

/* Resumed: no context is the host's any more (a test resumes in the
 * process that saved: its old ones are left alone), each made again at
 * its first use; GLU's quadrics as well. */
void hostgl_after_resume(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CONTEXTS; i++)
        if (contexts[i]) {
            contexts[i]->cgl = NULL;
            n++;
        }
    for (int i = 0; i < MAX_QUADRICS; i++)
        quadrics[i] = NULL;
    if (available > 0)
        CGLSetCurrentContext(NULL);
    if (n)
        LOGF(LOG_IDLE, "resume: OpenGL: %d context%s to make again\n", n, n == 1 ? "" : "s");
}

/* What the snapshot holds of OpenGL's state, for the tests. */
size_t hostgl_saved_bytes(void)
{
    size_t n = 0;
    for (int i = 0; i < MAX_CONTEXTS; i++)
        if (contexts[i]) {
            n += glh_saved_bytes(contexts[i]);
            bool first = true;
            for (int k = 0; k < i; k++)
                first = first && !(contexts[k] && contexts[k]->group == contexts[i]->group);
            if (first)
                n += glh_saved_texture_bytes(contexts[i]->group);
        }
    return n;
}

int hostgl_display_lists(void) { return cur ? cur->group->nlists : 0; }

/* ---------------------------------------------------------------------- */

const ppc_export gl_exports[] = {
    GL_IMMEDIATE(EXPORT)
    GL_IMMEDIATE_V(EXPORT_V, EXPORT_V, EXPORT_V)
    GL_STATE(EXPORT)
    GL_RESULTS(EXPORT)
    GL_PARAMS(EXPORT_V, EXPORT_V, EXPORT_V)
    GL_MATRICES(EXPORT_V)
    GLU_QUADRICS(EXPORT)
    { "glFlush", w_glFlush },
    { "glFinish", w_glFinish },
    { "glDrawBuffer", w_glDrawBuffer },
    { "glReadBuffer", w_glReadBuffer },
    { "glClipPlane", w_glClipPlane },
    { "glGetClipPlane", w_glGetClipPlane },
    { "glGetTexLevelParameterfv", w_glGetTexLevelParameterfv },
    { "glGetTexLevelParameteriv", w_glGetTexLevelParameteriv },
    { "glGenTextures", w_glGenTextures },
    { "glGenTexturesEXT", w_glGenTextures },
    { "glDeleteTextures", w_glDeleteTextures },
    { "glDeleteTexturesEXT", w_glDeleteTextures },
    { "glAreTexturesResident", w_glAreTexturesResident },
    { "glAreTexturesResidentEXT", w_glAreTexturesResident },
    { "glPrioritizeTextures", w_glPrioritizeTextures },
    { "glPrioritizeTexturesEXT", w_glPrioritizeTextures },
    { "glCallLists", w_glCallLists },
    { "glPixelMapfv", w_glPixelMapfv },
    { "glPixelMapuiv", w_glPixelMapuiv },
    { "glPixelMapusv", w_glPixelMapusv },
    { "glGetPixelMapfv", w_glGetPixelMapfv },
    { "glGetPixelMapuiv", w_glGetPixelMapuiv },
    { "glGetPixelMapusv", w_glGetPixelMapusv },
    { "glMap1d", w_glMap1d },
    { "glMap1f", w_glMap1f },
    { "glMap2d", w_glMap2d },
    { "glMap2f", w_glMap2f },
    { "glGetMapdv", w_glGetMapdv },
    { "glGetMapfv", w_glGetMapfv },
    { "glGetMapiv", w_glGetMapiv },
    { "glGetBooleanv", w_glGetBooleanv },
    { "glGetIntegerv", w_glGetIntegerv },
    { "glGetFloatv", w_glGetFloatv },
    { "glGetDoublev", w_glGetDoublev },
    { "glGetString", w_glGetString },
    { "glSelectBuffer", w_glSelectBuffer },
    { "glFeedbackBuffer", w_glFeedbackBuffer },
    { "glRenderMode", w_glRenderMode },
    { "glVertexPointer", w_glVertexPointer },
    { "glVertexPointerEXT", w_glVertexPointer },
    { "glColorPointer", w_glColorPointer },
    { "glTexCoordPointer", w_glTexCoordPointer },
    { "glNormalPointer", w_glNormalPointer },
    { "glIndexPointer", w_glIndexPointer },
    { "glEdgeFlagPointer", w_glEdgeFlagPointer },
    { "glEnableClientState", w_glEnableClientState },
    { "glDisableClientState", w_glDisableClientState },
    { "glClientActiveTextureARB", w_glClientActiveTextureARB },
    { "glPushClientAttrib", w_glPushClientAttrib },
    { "glPopClientAttrib", w_glPopClientAttrib },
    { "glGetPointerv", w_glGetPointerv },
    { "glInterleavedArrays", w_glInterleavedArrays },
    { "glDrawArrays", w_glDrawArrays },
    { "glDrawArraysEXT", w_glDrawArrays },
    { "glDrawElements", w_glDrawElements },
    { "glDrawRangeElements", w_glDrawRangeElements },
    { "glArrayElement", w_glArrayElement },
    { "glArrayElementEXT", w_glArrayElement },
    { "gluErrorString", w_gluErrorString },
    { "gluGetString", w_gluGetString },
    { "gluPickMatrix", w_gluPickMatrix },
    { "gluProject", w_gluProject },
    { "gluUnProject", w_gluUnProject },
    { "gluNewQuadric", w_gluNewQuadric },
    { "gluDeleteQuadric", w_gluDeleteQuadric },
};
const unsigned gl_export_count = sizeof gl_exports / sizeof gl_exports[0];

/* Guest memory is gone (a new program): its strings and quadric names
 * with it.  AGL has destroyed the contexts. */
void hostgl_reset(void)
{
    memset(strings, 0, sizeof strings);
    memset(glu_strings, 0, sizeof glu_strings);
    for (int i = 0; i < MAX_QUADRICS; i++) {
        quadric_names[i] = 0;
        quadrics[i] = NULL; /* their contexts are gone */
    }
    cur = NULL;
    CGLSetCurrentContext(NULL);
}

#else /* no host OpenGL */

bool hostgl_available(void) { return false; }
hostgl_ctx *hostgl_create(hostgl_ctx *share, int depth_bits, int stencil_bits)
{
    (void)share, (void)depth_bits, (void)stencil_bits;
    return NULL;
}
void hostgl_destroy(hostgl_ctx *h) { (void)h; }
void hostgl_set_current(hostgl_ctx *h) { (void)h; }
void hostgl_resize(hostgl_ctx *h, int width, int height) { (void)h, (void)width, (void)height; }
void hostgl_read_frame(hostgl_ctx *h, int x, int y, int width, int height, gaddr dst, uint32_t row_bytes)
{
    (void)h, (void)x, (void)y, (void)width, (void)height, (void)dst, (void)row_bytes;
}
void hostgl_copy_state(hostgl_ctx *src, hostgl_ctx *dst, uint32_t mask) { (void)src, (void)dst, (void)mask; }
void hostgl_glyph_list(uint32_t list, int width, int height, int row_bytes, float xorig, float yorig, float xmove,
                       const uint8_t *bits)
{
    (void)list, (void)width, (void)height, (void)row_bytes, (void)xorig, (void)yorig, (void)xmove, (void)bits;
}
void hostgl_reset(void) {}
const char *hostgl_blocker(void) { return NULL; }
void hostgl_before_hibernate(void) {}
void hostgl_after_hibernate(void) {}
void hostgl_after_resume(void) {}
size_t hostgl_saved_bytes(void) { return 0; }
int hostgl_display_lists(void) { return 0; }
const ppc_export gl_exports[] = { { "", NULL } };
const unsigned gl_export_count = 0;

#endif
