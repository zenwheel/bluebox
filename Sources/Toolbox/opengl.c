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
 */
#include <stdlib.h>
#include <string.h>

#include "hostgl.h"
#include "tb_protos_gen.h"

#ifdef __APPLE__
/* Deprecated since macOS 10.14, and still there. */
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <OpenGL/glu.h>

/* ---------------------------------------------------------------------- */
/* Contexts */

/* A vertex array as the program described it. */
typedef struct {
    bool on;
    GLint size;
    GLenum type;
    GLsizei stride;
    gaddr ptr;
    void *host; /* converted elements */
    size_t host_cap;
} garray;

enum { MAX_UNITS = 8, CLIENT_STACK = 16 };

typedef struct {
    garray vertex, normal, color, index, edge, tex[MAX_UNITS];
} arrays;

struct hostgl_ctx {
    CGLContextObj cgl;
    GLuint fbo, color, depth;
    int depth_bits, stencil_bits;
    int width, height;
    bool sized;
    GLenum draw_buffer, read_buffer; /* as the program set them */
    arrays a;
    arrays saved[CLIENT_STACK]; /* glPushClientAttrib's vertex array state */
    int nsaved;
    GLenum client_unit; /* glClientActiveTexture, from 0 */
    /* glSelectBuffer and glFeedbackBuffer: the host's, and the program's */
    GLuint *select;
    GLfloat *feedback;
    gaddr select_to, feedback_to;
    GLsizei select_n, feedback_n;
    GLenum render_mode;
};

static hostgl_ctx *cur;
static CGLPixelFormatObj pixel_format;
static int available = -1;

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

/* Make h current for a while (AGL works on contexts that aren't), then
 * put the program's back. */
static void use(hostgl_ctx *h)
{
    if (CGLGetCurrentContext() != h->cgl)
        CGLSetCurrentContext(h->cgl);
}
static void unuse(void) { CGLSetCurrentContext(cur ? cur->cgl : NULL); }

hostgl_ctx *hostgl_create(hostgl_ctx *share, int depth_bits, int stencil_bits)
{
    if (!hostgl_available())
        return NULL;
    hostgl_ctx *h = calloc(1, sizeof *h);
    if (!h)
        return NULL;
    if (CGLCreateContext(pixel_format, share ? share->cgl : NULL, &h->cgl) != kCGLNoError) {
        free(h);
        return NULL;
    }
    h->depth_bits = depth_bits;
    h->stencil_bits = stencil_bits;
    h->draw_buffer = h->read_buffer = GL_BACK;
    h->render_mode = GL_RENDER;
    use(h);
    glGenFramebuffersEXT(1, &h->fbo);
    glGenRenderbuffersEXT(1, &h->color);
    if (depth_bits || stencil_bits)
        glGenRenderbuffersEXT(1, &h->depth);
    glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_TRUE);
    glPixelStorei(GL_PACK_SWAP_BYTES, GL_TRUE);
    unuse();
    hostgl_resize(h, 1, 1);
    h->sized = false;
    return h;
}

static void free_arrays(arrays *a)
{
    free(a->vertex.host);
    free(a->normal.host);
    free(a->color.host);
    free(a->index.host);
    free(a->edge.host);
    for (int i = 0; i < MAX_UNITS; i++)
        free(a->tex[i].host);
}

void hostgl_destroy(hostgl_ctx *h)
{
    if (!h)
        return;
    if (cur == h)
        cur = NULL;
    use(h);
    glDeleteFramebuffersEXT(1, &h->fbo);
    glDeleteRenderbuffersEXT(1, &h->color);
    if (h->depth)
        glDeleteRenderbuffersEXT(1, &h->depth);
    unuse();
    CGLDestroyContext(h->cgl);
    free_arrays(&h->a);
    free(h->select);
    free(h->feedback);
    free(h);
}

void hostgl_set_current(hostgl_ctx *h)
{
    cur = h;
    CGLSetCurrentContext(h ? h->cgl : NULL);
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
    use(h);
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
        uint8_t *n = realloc(frame, need);
        if (!n)
            return;
        frame = n;
        frame_cap = need;
    }
    use(h);
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
    if (src && dst)
        CGLCopyContext(src->cgl, dst->cgl, mask);
}

void hostgl_glyph_list(uint32_t list, int width, int height, int row_bytes, float xorig, float yorig, float xmove,
                       const uint8_t *bits)
{
    if (!cur)
        return;
    uint8_t *flipped = malloc((size_t)(row_bytes * height) + 1);
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
    free(flipped);
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
        void *p = realloc(buf, n);
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
        if (CGLGetCurrentContext() != cur->cgl)                                                                       \
            CGLSetCurrentContext(cur->cgl);                                                                           \
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
        name(t);                                                                                                      \
        DONE();                                                                                                       \
    }
#define DEFINE_T(name, host, n, T)                                                                                    \
    static void w_##name(ppc_cpu *c)                                                                                  \
    {                                                                                                                 \
        ENTER();                                                                                                      \
        T t[4];                                                                                                       \
        gread(t, c->gpr[4], n, sizeof(T));                                                                            \
        host((GLenum)c->gpr[3], t);                                                                                   \
        DONE();                                                                                                       \
    }
#define DEFINE_R(name, T)                                                                                             \
    static void w_##name(ppc_cpu *c)                                                                                  \
    {                                                                                                                 \
        ENTER();                                                                                                      \
        T a[2], b[2];                                                                                                 \
        gread(a, c->gpr[3], 2, sizeof(T));                                                                            \
        gread(b, c->gpr[4], 2, sizeof(T));                                                                            \
        name(a, b);                                                                                                   \
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
    X(glGetError, "", glGetError()) \
    X(gluBuild1DMipmaps, "iiiiii", gluBuild1DMipmaps(U(0), I(1), I(2), U(3), U(4), P(5))) \
    X(gluBuild2DMipmaps, "iiiiiii", gluBuild2DMipmaps(U(0), I(1), I(2), I(3), U(4), U(5), P(6))) \
    X(gluScaleImage, "iiiiiiiii", gluScaleImage(U(0), I(1), I(2), U(3), P(4), I(5), I(6), U(7), P(8)))

#define DEFINE_RESULT(name, sig, expr)                                                                                \
    static void w_##name(ppc_cpu *c)                                                                                  \
    {                                                                                                                 \
        uint32_t iv[16];                                                                                              \
        double fv[16];                                                                                                \
        ENTER();                                                                                                      \
        args_get(c, sig, iv, fv);                                                                                     \
        c->gpr[3] = (uint32_t)(expr);                                                                                 \
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
    glDrawBuffer(cur->draw_buffer == GL_NONE ? GL_NONE : GL_COLOR_ATTACHMENT0_EXT);
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
        gread(t, c->gpr[5], param_count((GLenum)c->gpr[4]), sizeof(T));                                               \
        name((GLenum)c->gpr[3], (GLenum)c->gpr[4], t);                                                                \
        DONE();                                                                                                       \
    }
#define DEFINE_PV1(name, T) /* no target: fog and the light model */                                                   \
    static void w_##name(ppc_cpu *c)                                                                                  \
    {                                                                                                                 \
        ENTER();                                                                                                      \
        T t[4];                                                                                                       \
        gread(t, c->gpr[4], param_count((GLenum)c->gpr[3]), sizeof(T));                                               \
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
#define GL_PARAMS(X, X1, XG) \
    X(glLightfv, GLfloat) X(glLightiv, GLint) X(glMaterialfv, GLfloat) X(glMaterialiv, GLint) \
    X(glTexEnvfv, GLfloat) X(glTexEnviv, GLint) X(glTexParameterfv, GLfloat) X(glTexParameteriv, GLint) \
    X(glTexGendv, GLdouble) X(glTexGenfv, GLfloat) X(glTexGeniv, GLint) \
    X1(glFogfv, GLfloat) X1(glFogiv, GLint) X1(glLightModelfv, GLfloat) X1(glLightModeliv, GLint) \
    XG(glGetLightfv, GLfloat) XG(glGetLightiv, GLint) XG(glGetMaterialfv, GLfloat) XG(glGetMaterialiv, GLint) \
    XG(glGetTexEnvfv, GLfloat) XG(glGetTexEnviv, GLint) XG(glGetTexParameterfv, GLfloat) \
    XG(glGetTexParameteriv, GLint) XG(glGetTexGendv, GLdouble) XG(glGetTexGenfv, GLfloat) XG(glGetTexGeniv, GLint)
GL_PARAMS(DEFINE_PV, DEFINE_PV1, DEFINE_GETPV)

#define DEFINE_MATRIX(name, host, T)                                                                                  \
    static void w_##name(ppc_cpu *c)                                                                                  \
    {                                                                                                                 \
        ENTER();                                                                                                      \
        T m[16];                                                                                                      \
        gread(m, c->gpr[3], 16, sizeof(T));                                                                           \
        host(m);                                                                                                      \
        DONE();                                                                                                       \
    }
#define GL_MATRICES(X) \
    X(glLoadMatrixf, glLoadMatrixf, GLfloat) X(glLoadMatrixd, glLoadMatrixd, GLdouble) \
    X(glMultMatrixf, glMultMatrixf, GLfloat) X(glMultMatrixd, glMultMatrixd, GLdouble) \
    X(glLoadTransposeMatrixfARB, glLoadTransposeMatrixf, GLfloat) \
    X(glLoadTransposeMatrixdARB, glLoadTransposeMatrixd, GLdouble) \
    X(glMultTransposeMatrixfARB, glMultTransposeMatrixf, GLfloat) \
    X(glMultTransposeMatrixdARB, glMultTransposeMatrixd, GLdouble)
GL_MATRICES(DEFINE_MATRIX)

static void w_glClipPlane(ppc_cpu *c)
{
    ENTER();
    GLdouble e[4];
    gread(e, c->gpr[4], 4, 8);
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

static void w_glGenTextures(ppc_cpu *c)
{
    ENTER();
    GLsizei n = (GLsizei)c->gpr[3];
    GLuint *t = n > 0 ? scratch((size_t)n * 4) : NULL;
    if (t) {
        glGenTextures(n, t);
        gwrite(c->gpr[4], t, n, 4);
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
            glCallLists(n, type, gptr(lists));
        } else {
            void *t = scratch((size_t)n * size);
            if (t) {
                gread(t, lists, n, size);
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
    free(cur->select);
    cur->select = n > 0 ? calloc((size_t)n, 4) : NULL;
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
    free(cur->feedback);
    cur->feedback = n > 0 ? calloc((size_t)n, 4) : NULL;
    cur->feedback_n = cur->feedback ? n : 0;
    cur->feedback_to = c->gpr[5];
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
        void *n = realloc(a->host, need);
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

static void w_glDrawArrays(ppc_cpu *c)
{
    ENTER();
    GLint first = (GLint)c->gpr[4];
    GLsizei count = (GLsizei)c->gpr[5];
    if (first >= 0 && count > 0 && arrays_to_host(first, count))
        glDrawArrays((GLenum)c->gpr[3], 0, count);
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
    if (arrays_to_host((int)lo, (int)(hi - lo + 1)))
        glDrawElements(mode, count, GL_UNSIGNED_INT, idx);
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

static void w_glArrayElement(ppc_cpu *c)
{
    ENTER();
    int i = (int)c->gpr[3];
    arrays *a = &cur->a;
    union {
        GLbyte b[4];
        GLubyte ub[4];
        GLshort s[4];
        GLushort us[4];
        GLint i[4];
        GLuint ui[4];
        GLfloat f[4];
        GLdouble d[4];
    } v;
    if (element(&a->edge, i, &v))
        glEdgeFlag(v.ub[0]);
    for (int u = 0; u < MAX_UNITS; u++)
        if (element(&a->tex[u], i, &v)) {
            GLdouble t[4] = { 0, 0, 0, 1 };
            for (int k = 0; k < a->tex[u].size && k < 4; k++)
                t[k] = a->tex[u].type == GL_SHORT ? v.s[k] : a->tex[u].type == GL_INT ? v.i[k]
                       : a->tex[u].type == GL_FLOAT ? v.f[k] : v.d[k];
            glMultiTexCoord4dv(GL_TEXTURE0 + (GLenum)u, t);
        }
    if (element(&a->color, i, &v)) {
        bool four = a->color.size == 4;
        switch (a->color.type) {
        case GL_BYTE: four ? glColor4bv(v.b) : glColor3bv(v.b); break;
        case GL_UNSIGNED_BYTE: four ? glColor4ubv(v.ub) : glColor3ubv(v.ub); break;
        case GL_SHORT: four ? glColor4sv(v.s) : glColor3sv(v.s); break;
        case GL_UNSIGNED_SHORT: four ? glColor4usv(v.us) : glColor3usv(v.us); break;
        case GL_INT: four ? glColor4iv(v.i) : glColor3iv(v.i); break;
        case GL_UNSIGNED_INT: four ? glColor4uiv(v.ui) : glColor3uiv(v.ui); break;
        case GL_FLOAT: four ? glColor4fv(v.f) : glColor3fv(v.f); break;
        case GL_DOUBLE: four ? glColor4dv(v.d) : glColor3dv(v.d); break;
        }
    }
    if (element(&a->index, i, &v)) {
        switch (a->index.type) {
        case GL_UNSIGNED_BYTE: glIndexub(v.ub[0]); break;
        case GL_SHORT: glIndexs(v.s[0]); break;
        case GL_INT: glIndexi(v.i[0]); break;
        case GL_FLOAT: glIndexf(v.f[0]); break;
        case GL_DOUBLE: glIndexd(v.d[0]); break;
        }
    }
    if (element(&a->normal, i, &v)) {
        switch (a->normal.type) {
        case GL_BYTE: glNormal3bv(v.b); break;
        case GL_SHORT: glNormal3sv(v.s); break;
        case GL_INT: glNormal3iv(v.i); break;
        case GL_FLOAT: glNormal3fv(v.f); break;
        case GL_DOUBLE: glNormal3dv(v.d); break;
        }
    }
    if (element(&a->vertex, i, &v)) {
        GLdouble p[4] = { 0, 0, 0, 1 };
        for (int k = 0; k < a->vertex.size && k < 4; k++)
            p[k] = a->vertex.type == GL_SHORT ? v.s[k] : a->vertex.type == GL_INT ? v.i[k]
                   : a->vertex.type == GL_FLOAT ? v.f[k] : v.d[k];
        glVertex4dv(p);
    }
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

/* Quadrics: a system-heap word naming the host's. */
enum { MAX_QUADRICS = 64 };
static GLUquadric *quadrics[MAX_QUADRICS];
static gaddr quadric_names[MAX_QUADRICS];

static GLUquadric *quadric(gaddr q)
{
    for (int i = 0; q && i < MAX_QUADRICS; i++)
        if (quadric_names[i] == q)
            return quadrics[i];
    return NULL;
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
            gluDeleteQuadric(quadrics[i]);
            tb_DisposePtr(quadric_names[i]);
            quadrics[i] = NULL;
            quadric_names[i] = 0;
        }
    DONE();
}

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
GLU_QUADRICS(DEFINE)

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
const ppc_export gl_exports[] = { { "", NULL } };
const unsigned gl_export_count = 0;

#endif
