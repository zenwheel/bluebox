/*
 * Hibernating an OpenGL context (opengl.c): OpenGL's state is the host's,
 * which a snapshot cannot carry, so before one is taken what the host has
 * of each context is read back into the machine, and a resumed process
 * makes the context again from it the first time the program uses it:
 *
 *   read back (glsaved)   the fixed-function state, by glGet: enables,
 *                         lighting, materials, fog, texture environments
 *                         and coordinate generation, clip planes, the
 *                         matrix stacks whole, pixel transfer and maps,
 *                         evaluators, the stipple, the current values and
 *                         raster position, the pixel store; the context's
 *                         default textures (name 0); the framebuffer's
 *                         colours, depths and stencil values
 *   read back (gltexture) a sharing group's texture objects: each level's
 *                         image and the object's parameters
 *   kept as they go       its display lists (recorded as they are
 *                         compiled, opengl.c), vertex arrays, selection
 *                         and feedback buffers, which are the machine's
 *
 * The state is read back exactly (doubles hold every value), and set again
 * in an order that makes it what it was: what is kept in eye coordinates
 * (light positions, clip planes, eye planes) with the identity model-view
 * matrix; materials before colour tracking; the matrices and the enables
 * last.  Textures are read at the precision their internal format keeps
 * and given back with the same internal format.  A context mid-way
 * through something (glBegin, a list being compiled, selection or
 * feedback, pushed attributes) is not saved (hostgl_blocker).
 */
#ifdef __APPLE__
#include <string.h>

#include "hostgl_internal.h"
#include "tb_internal.h"

enum { MAX_LEVELS = 16, NTEXPARAMS = 12, NLIGHTS = 8, NPLANES = 6, MAX_STACK = 32, NMAPS = 18, NPIXMAPS = 10 };

typedef struct {
    int level, w, h, d, border;
    GLint ifmt;
    GLenum fmt, type;
    uint32_t bytes;
    uint8_t *px;
} gllevel;

typedef struct gltexture {
    GLuint name;
    GLenum target;
    int nlevels;
    gllevel lv[MAX_LEVELS];
    GLfloat param[NTEXPARAMS][4];
} gltexture;

static const GLenum tex_params[NTEXPARAMS] = {
    GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER, GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_TEXTURE_WRAP_R,
    GL_TEXTURE_BORDER_COLOR, GL_TEXTURE_PRIORITY, GL_TEXTURE_MIN_LOD, GL_TEXTURE_MAX_LOD, GL_TEXTURE_BASE_LEVEL,
    GL_TEXTURE_MAX_LEVEL, GL_GENERATE_MIPMAP,
};

/* ---- The simple state: each read with glGetDoublev, n values, and set
 * by code over v (its values; v[-1] the entry before's last).  An entry
 * setting nothing (0) is read for the one after it. ---- */

#define E(k) ((GLenum)v[k])
#define I(k) ((GLint)v[k])
#define U(k) ((GLuint)(int64_t)v[k])
#define F(k) ((GLfloat)v[k])
#define D(k) (v[k])
#define B(k) ((GLboolean)(v[k] != 0))
#define F4 f4(v, tmp)
#define TRANSFER(X, name) X(name, 1, glPixelTransferf(name, F(0)))
#define HINT(X, name) X(name, 1, glHint(name, E(0)))
#define STATE(X) \
    X(GL_ALPHA_TEST_FUNC, 1, 0) X(GL_ALPHA_TEST_REF, 1, glAlphaFunc(E(-1), F(0))) \
    X(GL_BLEND_SRC_RGB, 1, 0) X(GL_BLEND_DST_RGB, 1, 0) X(GL_BLEND_SRC_ALPHA, 1, 0) \
    X(GL_BLEND_DST_ALPHA, 1, glBlendFuncSeparate(E(-3), E(-2), E(-1), E(0))) \
    X(GL_BLEND_COLOR, 4, glBlendColor(F(0), F(1), F(2), F(3))) \
    X(GL_BLEND_EQUATION_RGB, 1, 0) X(GL_BLEND_EQUATION_ALPHA, 1, glBlendEquationSeparate(E(-1), E(0))) \
    X(GL_COLOR_CLEAR_VALUE, 4, glClearColor(F(0), F(1), F(2), F(3))) \
    X(GL_DEPTH_CLEAR_VALUE, 1, glClearDepth(D(0))) \
    X(GL_STENCIL_CLEAR_VALUE, 1, glClearStencil(I(0))) \
    X(GL_INDEX_CLEAR_VALUE, 1, glClearIndex(F(0))) \
    X(GL_ACCUM_CLEAR_VALUE, 4, glClearAccum(F(0), F(1), F(2), F(3))) \
    X(GL_COLOR_WRITEMASK, 4, glColorMask(B(0), B(1), B(2), B(3))) \
    X(GL_DEPTH_WRITEMASK, 1, glDepthMask(B(0))) \
    X(GL_STENCIL_WRITEMASK, 1, glStencilMask(U(0))) \
    X(GL_INDEX_WRITEMASK, 1, glIndexMask(U(0))) \
    X(GL_DEPTH_FUNC, 1, glDepthFunc(E(0))) \
    X(GL_DEPTH_RANGE, 2, glDepthRange(D(0), D(1))) \
    X(GL_LOGIC_OP_MODE, 1, glLogicOp(E(0))) \
    X(GL_STENCIL_FUNC, 1, 0) X(GL_STENCIL_REF, 1, 0) X(GL_STENCIL_VALUE_MASK, 1, glStencilFunc(E(-2), I(-1), U(0))) \
    X(GL_STENCIL_FAIL, 1, 0) X(GL_STENCIL_PASS_DEPTH_FAIL, 1, 0) \
    X(GL_STENCIL_PASS_DEPTH_PASS, 1, glStencilOp(E(-2), E(-1), E(0))) \
    X(GL_CULL_FACE_MODE, 1, glCullFace(E(0))) \
    X(GL_FRONT_FACE, 1, glFrontFace(E(0))) \
    X(GL_POLYGON_MODE, 2, (glPolygonMode(GL_FRONT, E(0)), glPolygonMode(GL_BACK, E(1)))) \
    X(GL_POLYGON_OFFSET_FACTOR, 1, 0) X(GL_POLYGON_OFFSET_UNITS, 1, glPolygonOffset(F(-1), F(0))) \
    X(GL_LINE_WIDTH, 1, glLineWidth(F(0))) \
    X(GL_LINE_STIPPLE_PATTERN, 1, 0) X(GL_LINE_STIPPLE_REPEAT, 1, glLineStipple(I(0), (GLushort)U(-1))) \
    X(GL_POINT_SIZE, 1, glPointSize(F(0))) \
    X(GL_SHADE_MODEL, 1, glShadeModel(E(0))) \
    X(GL_SCISSOR_BOX, 4, glScissor(I(0), I(1), I(2), I(3))) \
    X(GL_VIEWPORT, 4, glViewport(I(0), I(1), I(2), I(3))) \
    X(GL_ZOOM_X, 1, 0) X(GL_ZOOM_Y, 1, glPixelZoom(F(-1), F(0))) \
    X(GL_MAP_COLOR, 1, glPixelTransferi(GL_MAP_COLOR, I(0))) \
    X(GL_MAP_STENCIL, 1, glPixelTransferi(GL_MAP_STENCIL, I(0))) \
    X(GL_INDEX_SHIFT, 1, glPixelTransferi(GL_INDEX_SHIFT, I(0))) \
    X(GL_INDEX_OFFSET, 1, glPixelTransferi(GL_INDEX_OFFSET, I(0))) \
    TRANSFER(X, GL_RED_SCALE) TRANSFER(X, GL_RED_BIAS) TRANSFER(X, GL_GREEN_SCALE) TRANSFER(X, GL_GREEN_BIAS) \
    TRANSFER(X, GL_BLUE_SCALE) TRANSFER(X, GL_BLUE_BIAS) TRANSFER(X, GL_ALPHA_SCALE) TRANSFER(X, GL_ALPHA_BIAS) \
    TRANSFER(X, GL_DEPTH_SCALE) TRANSFER(X, GL_DEPTH_BIAS) \
    HINT(X, GL_PERSPECTIVE_CORRECTION_HINT) HINT(X, GL_POINT_SMOOTH_HINT) HINT(X, GL_LINE_SMOOTH_HINT) \
    HINT(X, GL_POLYGON_SMOOTH_HINT) HINT(X, GL_FOG_HINT) HINT(X, GL_GENERATE_MIPMAP_HINT) \
    X(GL_FOG_MODE, 1, glFogi(GL_FOG_MODE, I(0))) \
    X(GL_FOG_DENSITY, 1, glFogf(GL_FOG_DENSITY, F(0))) \
    X(GL_FOG_START, 1, glFogf(GL_FOG_START, F(0))) \
    X(GL_FOG_END, 1, glFogf(GL_FOG_END, F(0))) \
    X(GL_FOG_INDEX, 1, glFogf(GL_FOG_INDEX, F(0))) \
    X(GL_FOG_COLOR, 4, glFogfv(GL_FOG_COLOR, F4)) \
    X(GL_LIGHT_MODEL_AMBIENT, 4, glLightModelfv(GL_LIGHT_MODEL_AMBIENT, F4)) \
    X(GL_LIGHT_MODEL_LOCAL_VIEWER, 1, glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, I(0))) \
    X(GL_LIGHT_MODEL_TWO_SIDE, 1, glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, I(0))) \
    X(GL_LIGHT_MODEL_COLOR_CONTROL, 1, glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, I(0))) \
    X(GL_COLOR_MATERIAL_FACE, 1, 0) X(GL_COLOR_MATERIAL_PARAMETER, 1, glColorMaterial(E(-1), E(0))) \
    X(GL_LIST_BASE, 1, glListBase(U(0))) \
    X(GL_MAP1_GRID_DOMAIN, 2, 0) X(GL_MAP1_GRID_SEGMENTS, 1, glMapGrid1d(I(0), D(-2), D(-1))) \
    X(GL_MAP2_GRID_DOMAIN, 4, 0) \
    X(GL_MAP2_GRID_SEGMENTS, 2, glMapGrid2d(I(0), D(-4), D(-3), I(1), D(-2), D(-1))) \
    X(GL_EDGE_FLAG, 1, glEdgeFlag(B(0))) \
    X(GL_CURRENT_INDEX, 1, glIndexd(D(0))) \
    X(GL_CURRENT_NORMAL, 3, glNormal3d(D(0), D(1), D(2))) \
    X(GL_CURRENT_COLOR, 4, glColor4d(D(0), D(1), D(2), D(3)))

#define COUNT(name, n, set) +(n)
enum { NVALUES = 0 STATE(COUNT) };
#undef COUNT

static const GLenum enables[] = {
    GL_ALPHA_TEST, GL_AUTO_NORMAL, GL_BLEND, GL_CLIP_PLANE0, GL_CLIP_PLANE1, GL_CLIP_PLANE2, GL_CLIP_PLANE3,
    GL_CLIP_PLANE4, GL_CLIP_PLANE5, GL_COLOR_LOGIC_OP, GL_CULL_FACE, GL_DEPTH_TEST, GL_DITHER, GL_FOG,
    GL_INDEX_LOGIC_OP, GL_LIGHT0, GL_LIGHT1, GL_LIGHT2, GL_LIGHT3, GL_LIGHT4, GL_LIGHT5, GL_LIGHT6, GL_LIGHT7,
    GL_LIGHTING, GL_LINE_SMOOTH, GL_LINE_STIPPLE, GL_MAP1_COLOR_4, GL_MAP1_INDEX, GL_MAP1_NORMAL,
    GL_MAP1_TEXTURE_COORD_1, GL_MAP1_TEXTURE_COORD_2, GL_MAP1_TEXTURE_COORD_3, GL_MAP1_TEXTURE_COORD_4,
    GL_MAP1_VERTEX_3, GL_MAP1_VERTEX_4, GL_MAP2_COLOR_4, GL_MAP2_INDEX, GL_MAP2_NORMAL, GL_MAP2_TEXTURE_COORD_1,
    GL_MAP2_TEXTURE_COORD_2, GL_MAP2_TEXTURE_COORD_3, GL_MAP2_TEXTURE_COORD_4, GL_MAP2_VERTEX_3, GL_MAP2_VERTEX_4,
    GL_NORMALIZE, GL_POINT_SMOOTH, GL_POLYGON_OFFSET_FILL, GL_POLYGON_OFFSET_LINE, GL_POLYGON_OFFSET_POINT,
    GL_POLYGON_SMOOTH, GL_POLYGON_STIPPLE, GL_RESCALE_NORMAL, GL_SCISSOR_TEST, GL_STENCIL_TEST,
    GL_COLOR_MATERIAL, /* last: it takes the current colour into the material */
};
enum { NENABLES = sizeof enables / sizeof enables[0] };
static const GLenum unit_enables[] = {
    GL_TEXTURE_1D, GL_TEXTURE_2D, GL_TEXTURE_3D, GL_TEXTURE_GEN_S, GL_TEXTURE_GEN_T, GL_TEXTURE_GEN_R,
    GL_TEXTURE_GEN_Q,
};
enum { NUNIT_ENABLES = sizeof unit_enables / sizeof unit_enables[0] };

static const GLenum light_params[] = {
    GL_AMBIENT, GL_DIFFUSE, GL_SPECULAR, GL_POSITION, GL_SPOT_DIRECTION, GL_SPOT_EXPONENT, GL_SPOT_CUTOFF,
    GL_CONSTANT_ATTENUATION, GL_LINEAR_ATTENUATION, GL_QUADRATIC_ATTENUATION,
};
enum { NLIGHT_PARAMS = sizeof light_params / sizeof light_params[0] };
static const GLenum material_params[] = { GL_AMBIENT, GL_DIFFUSE, GL_SPECULAR, GL_EMISSION, GL_SHININESS,
                                          GL_COLOR_INDEXES };
enum { NMATERIAL_PARAMS = sizeof material_params / sizeof material_params[0] };
static const GLenum env_params[] = {
    GL_TEXTURE_ENV_MODE, GL_TEXTURE_ENV_COLOR, GL_COMBINE_RGB, GL_COMBINE_ALPHA, GL_SRC0_RGB, GL_SRC1_RGB,
    GL_SRC2_RGB, GL_SRC0_ALPHA, GL_SRC1_ALPHA, GL_SRC2_ALPHA, GL_OPERAND0_RGB, GL_OPERAND1_RGB, GL_OPERAND2_RGB,
    GL_OPERAND0_ALPHA, GL_OPERAND1_ALPHA, GL_OPERAND2_ALPHA, GL_RGB_SCALE, GL_ALPHA_SCALE,
};
enum { NENV_PARAMS = sizeof env_params / sizeof env_params[0] };
static const GLenum maps[NMAPS] = {
    GL_MAP1_COLOR_4, GL_MAP1_INDEX, GL_MAP1_NORMAL, GL_MAP1_TEXTURE_COORD_1, GL_MAP1_TEXTURE_COORD_2,
    GL_MAP1_TEXTURE_COORD_3, GL_MAP1_TEXTURE_COORD_4, GL_MAP1_VERTEX_3, GL_MAP1_VERTEX_4, GL_MAP2_COLOR_4,
    GL_MAP2_INDEX, GL_MAP2_NORMAL, GL_MAP2_TEXTURE_COORD_1, GL_MAP2_TEXTURE_COORD_2, GL_MAP2_TEXTURE_COORD_3,
    GL_MAP2_TEXTURE_COORD_4, GL_MAP2_VERTEX_3, GL_MAP2_VERTEX_4,
};
static const GLenum pixmaps[NPIXMAPS] = {
    GL_PIXEL_MAP_I_TO_I, GL_PIXEL_MAP_S_TO_S, GL_PIXEL_MAP_I_TO_R, GL_PIXEL_MAP_I_TO_G, GL_PIXEL_MAP_I_TO_B,
    GL_PIXEL_MAP_I_TO_A, GL_PIXEL_MAP_R_TO_R, GL_PIXEL_MAP_G_TO_G, GL_PIXEL_MAP_B_TO_B, GL_PIXEL_MAP_A_TO_A,
};
static const GLenum store_names[16] = {
    GL_PACK_SWAP_BYTES, GL_PACK_LSB_FIRST, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS,
    GL_PACK_ALIGNMENT, GL_PACK_IMAGE_HEIGHT, GL_PACK_SKIP_IMAGES,
    GL_UNPACK_SWAP_BYTES, GL_UNPACK_LSB_FIRST, GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_PIXELS,
    GL_UNPACK_ALIGNMENT, GL_UNPACK_IMAGE_HEIGHT, GL_UNPACK_SKIP_IMAGES,
};
static const GLenum default_targets[3] = { GL_TEXTURE_1D, GL_TEXTURE_2D, GL_TEXTURE_3D };
static const GLenum bindings[3] = { GL_TEXTURE_BINDING_1D, GL_TEXTURE_BINDING_2D, GL_TEXTURE_BINDING_3D };
static const GLenum coords[4] = { GL_S, GL_T, GL_R, GL_Q };

typedef struct {
    GLfloat env[NENV_PARAMS][4];
    GLint gen_mode[4];
    GLdouble gen_object[4][4], gen_eye[4][4];
    GLint binding[3];
    GLdouble texcoord[4];
    GLboolean enabled[NUNIT_ENABLES];
    int depth; /* the texture matrix stack's */
    GLdouble matrix[MAX_STACK][16];
} glunit;

typedef struct glsaved {
    double v[NVALUES];
    GLboolean enabled[NENABLES];
    GLfloat light[NLIGHTS][NLIGHT_PARAMS][4];
    GLfloat material[2][NMATERIAL_PARAMS][4];
    GLdouble plane[NPLANES][4];
    GLubyte stipple[128];
    GLint store[16];
    GLdouble raster[4], raster_color[4], raster_tex[4], raster_index;
    GLint raster_valid;
    GLint matrix_mode, active;
    int depth[2]; /* model-view and projection */
    GLdouble matrix[2][MAX_STACK][16];
    int nunits;
    glunit unit[MAX_UNITS];
    GLint map_order[NMAPS][2];
    GLdouble map_domain[NMAPS][4];
    GLdouble *map_coeff; /* each map's in turn */
    GLint pixmap_size[NPIXMAPS];
    GLfloat *pixmap; /* each map's in turn */
    gltexture defaults[3];
    int fw, fh;
    uint32_t *color, *ds; /* the framebuffer */
} glsaved;

static const GLfloat *f4(const double *v, GLfloat *out)
{
    for (int k = 0; k < 4; k++)
        out[k] = (GLfloat)v[k];
    return out;
}

static int map_dims(GLenum m)
{
    switch (m) {
    case GL_MAP1_INDEX: case GL_MAP2_INDEX: case GL_MAP1_TEXTURE_COORD_1: case GL_MAP2_TEXTURE_COORD_1: return 1;
    case GL_MAP1_TEXTURE_COORD_2: case GL_MAP2_TEXTURE_COORD_2: return 2;
    case GL_MAP1_VERTEX_3: case GL_MAP2_VERTEX_3: case GL_MAP1_NORMAL: case GL_MAP2_NORMAL:
    case GL_MAP1_TEXTURE_COORD_3: case GL_MAP2_TEXTURE_COORD_3:
        return 3;
    default: return 4;
    }
}

static int units(void)
{
    GLint n = 1;
    glGetIntegerv(GL_MAX_TEXTURE_UNITS, &n);
    return n < 1 ? 1 : n > MAX_UNITS ? MAX_UNITS : n;
}

void glh_pixel_store_defaults(bool pack, bool unpack)
{
    for (int k = pack ? 0 : 8; k < (unpack ? 16 : 8); k++)
        glPixelStorei(store_names[k], (k & 7) == 5 ? 1 : 0);
}

/* ---- Textures ---- */

/* How a level is read back: its base format, at the precision kept. */
static void level_format(GLenum target, int level, GLenum *fmt, GLenum *type, int *n, int *size)
{
    GLint r = 0, a = 0, l = 0, i = 0, d = 0, g = 0, b = 0;
    glGetTexLevelParameteriv(target, level, GL_TEXTURE_RED_SIZE, &r);
    glGetTexLevelParameteriv(target, level, GL_TEXTURE_GREEN_SIZE, &g);
    glGetTexLevelParameteriv(target, level, GL_TEXTURE_BLUE_SIZE, &b);
    glGetTexLevelParameteriv(target, level, GL_TEXTURE_ALPHA_SIZE, &a);
    glGetTexLevelParameteriv(target, level, GL_TEXTURE_LUMINANCE_SIZE, &l);
    glGetTexLevelParameteriv(target, level, GL_TEXTURE_INTENSITY_SIZE, &i);
    glGetTexLevelParameteriv(target, level, GL_TEXTURE_DEPTH_SIZE, &d);
    int most = r > g ? r : g;
    most = most > b ? most : b;
    most = most > a ? most : a;
    most = most > l ? most : l;
    most = most > i ? most : i;
    if (d) {
        *fmt = GL_DEPTH_COMPONENT, *n = 1, *type = GL_UNSIGNED_INT, *size = 4;
        return;
    }
    if (l)
        *fmt = a ? GL_LUMINANCE_ALPHA : GL_LUMINANCE, *n = a ? 2 : 1;
    else if (i) /* read as luminance: given back to an intensity format, it is the intensity again */
        *fmt = GL_LUMINANCE, *n = 1;
    else if (r || g || b)
        *fmt = a ? GL_RGBA : GL_RGB, *n = a ? 4 : 3;
    else
        *fmt = GL_ALPHA, *n = 1;
    if (most > 8)
        *type = GL_UNSIGNED_SHORT, *size = 2;
    else
        *type = GL_UNSIGNED_BYTE, *size = 1;
}

/* The texture bound to target (on the active unit), read back. */
static void capture_texture(GLenum target, GLuint name, gltexture *t)
{
    memset(t, 0, sizeof *t);
    t->name = name;
    t->target = target;
    for (int lv = 0; lv < MAX_LEVELS; lv++) {
        GLint w = 0, h = 1, d = 1, border = 0, ifmt = 0;
        glGetTexLevelParameteriv(target, lv, GL_TEXTURE_WIDTH, &w);
        if (w <= 0)
            continue;
        if (target != GL_TEXTURE_1D)
            glGetTexLevelParameteriv(target, lv, GL_TEXTURE_HEIGHT, &h);
        if (target == GL_TEXTURE_3D)
            glGetTexLevelParameteriv(target, lv, GL_TEXTURE_DEPTH, &d);
        glGetTexLevelParameteriv(target, lv, GL_TEXTURE_BORDER, &border);
        glGetTexLevelParameteriv(target, lv, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
        gllevel *l = &t->lv[t->nlevels];
        int n, size;
        level_format(target, lv, &l->fmt, &l->type, &n, &size);
        size_t bytes = (size_t)w * (size_t)(h > 0 ? h : 1) * (size_t)(d > 0 ? d : 1) * (size_t)n * (size_t)size;
        l->px = bb_malloc(bytes ? bytes : 1, BB_BYTES);
        if (!l->px)
            continue;
        glGetTexImage(target, lv, l->fmt, l->type, l->px);
        l->level = lv, l->w = w, l->h = h, l->d = d, l->border = border, l->ifmt = ifmt;
        l->bytes = (uint32_t)bytes;
        t->nlevels++;
    }
    for (int k = 0; k < NTEXPARAMS; k++)
        glGetTexParameterfv(target, tex_params[k], t->param[k]);
}

static void restore_texture(const gltexture *t)
{
    glBindTexture(t->target, t->name);
    glTexParameteri(t->target, GL_GENERATE_MIPMAP, GL_FALSE); /* each level as it was */
    for (int k = 0; k < t->nlevels; k++) {
        const gllevel *l = &t->lv[k];
        if (t->target == GL_TEXTURE_1D)
            glTexImage1D(t->target, l->level, l->ifmt, l->w, l->border, l->fmt, l->type, l->px);
        else if (t->target == GL_TEXTURE_3D)
            glTexImage3D(t->target, l->level, l->ifmt, l->w, l->h, l->d, l->border, l->fmt, l->type, l->px);
        else
            glTexImage2D(t->target, l->level, l->ifmt, l->w, l->h, l->border, l->fmt, l->type, l->px);
    }
    for (int k = 0; k < NTEXPARAMS; k++)
        glTexParameterfv(t->target, tex_params[k], t->param[k]);
}

static void free_texture(gltexture *t)
{
    for (int k = 0; k < t->nlevels; k++)
        bb_free(t->lv[k].px);
    t->nlevels = 0;
}

static size_t texture_bytes(const gltexture *t)
{
    size_t n = 0;
    for (int k = 0; k < t->nlevels; k++)
        n += t->lv[k].bytes;
    return n;
}

void glh_capture_textures(glgroup *g)
{
    if (g->saved || !g->ntex)
        return;
    g->saved = bb_calloc((size_t)g->ntex, sizeof *g->saved, BB_TAG(gltexture));
    if (!g->saved)
        return;
    glPushAttrib(GL_TEXTURE_BIT);
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glh_pixel_store_defaults(true, false);
    glActiveTexture(GL_TEXTURE0);
    static const GLenum targets[] = { GL_TEXTURE_2D, GL_TEXTURE_1D, GL_TEXTURE_3D };
    for (int i = 0; i < g->ntex; i++) {
        GLuint name = g->tex[i];
        if (!glIsTexture(name))
            continue; /* a name made and not bound yet */
        while (glGetError() != GL_NO_ERROR) {
        }
        for (unsigned k = 0; k < sizeof targets / sizeof targets[0]; k++) {
            glBindTexture(targets[k], name);
            if (glGetError() == GL_NO_ERROR) { /* its target: another is an error */
                capture_texture(targets[k], name, &g->saved[g->nsaved++]);
                break;
            }
        }
    }
    glPopClientAttrib();
    glPopAttrib();
    while (glGetError() != GL_NO_ERROR) { /* levels past the greatest the host has */
    }
}

void glh_restore_textures(glgroup *g)
{
    glh_pixel_store_defaults(false, true);
    for (int i = 0; i < g->nsaved; i++)
        restore_texture(&g->saved[i]);
    /* Names made and not bound are taken still: the host gives the least
     * free names, so asking it for as many as are free up to the greatest
     * and giving back those not the group's leaves just those taken. */
    uint32_t top = g->ntex ? g->tex[g->ntex - 1] : 0;
    GLsizei want = (GLsizei)top - g->nsaved;
    if (want > 0 && want <= 1 << 20) {
        GLuint *names = bb_malloc((size_t)want * 4, BB_BYTES);
        if (names) {
            glGenTextures(want, names);
            for (GLsizei i = 0; i < want; i++) {
                int lo = 0, hi = g->ntex;
                while (lo < hi) {
                    int mid = (lo + hi) / 2;
                    if (g->tex[mid] < names[i])
                        lo = mid + 1;
                    else
                        hi = mid;
                }
                if (lo >= g->ntex || g->tex[lo] != names[i])
                    glDeleteTextures(1, &names[i]);
            }
            bb_free(names);
        }
    }
    glBindTexture(GL_TEXTURE_1D, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindTexture(GL_TEXTURE_3D, 0);
    glh_free_textures(g);
}

void glh_free_textures(glgroup *g)
{
    for (int i = 0; i < g->nsaved; i++)
        free_texture(&g->saved[i]);
    bb_free(g->saved);
    g->saved = NULL;
    g->nsaved = 0;
}

size_t glh_saved_texture_bytes(const glgroup *g)
{
    size_t n = 0;
    for (int i = 0; i < g->nsaved; i++)
        n += texture_bytes(&g->saved[i]);
    return n;
}

/* ---- The matrices: each stack whole, read by popping and pushed back ---- */

static void capture_stack(GLenum mode, GLenum depth_name, GLenum matrix_name, int *depth, GLdouble m[][16])
{
    glMatrixMode(mode);
    GLint d = 1;
    glGetIntegerv(depth_name, &d);
    if (d < 1)
        d = 1;
    if (d > MAX_STACK)
        d = MAX_STACK;
    for (int i = d - 1; i >= 0; i--) {
        glGetDoublev(matrix_name, m[i]);
        if (i > 0)
            glPopMatrix();
    }
    for (int i = 1; i < d; i++) {
        glPushMatrix();
        glLoadMatrixd(m[i]);
    }
    *depth = d;
}

static void restore_stack(GLenum mode, int depth, GLdouble m[][16])
{
    glMatrixMode(mode);
    glLoadMatrixd(m[0]);
    for (int i = 1; i < depth; i++) {
        glPushMatrix();
        glLoadMatrixd(m[i]);
    }
}

/* ---- The context ---- */

void glh_capture(hostgl_ctx *h)
{
    GLenum e;
    while ((e = glGetError()) != GL_NO_ERROR) /* kept for the program's glGetError */
        if (!h->pending_error)
            h->pending_error = e;
    glsaved *s = bb_calloc(1, sizeof *s, BB_TAG(glsaved));
    if (!s)
        return;
    h->hib = s;
    for (int k = 0; k < 16; k++)
        glGetIntegerv(store_names[k], &s->store[k]);
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glh_pixel_store_defaults(true, false);

    int k = 0;
#define GET(name, n, set) glGetDoublev(name, &s->v[k]), k += (n);
    STATE(GET)
#undef GET
    for (int i = 0; i < NENABLES; i++)
        s->enabled[i] = glIsEnabled(enables[i]);
    for (int l = 0; l < NLIGHTS; l++)
        for (int p = 0; p < NLIGHT_PARAMS; p++)
            glGetLightfv(GL_LIGHT0 + (GLenum)l, light_params[p], s->light[l][p]);
    for (int f = 0; f < 2; f++)
        for (int p = 0; p < NMATERIAL_PARAMS; p++)
            glGetMaterialfv(f ? GL_BACK : GL_FRONT, material_params[p], s->material[f][p]);
    for (int p = 0; p < NPLANES; p++)
        glGetClipPlane(GL_CLIP_PLANE0 + (GLenum)p, s->plane[p]);
    glGetPolygonStipple(s->stipple);
    glGetDoublev(GL_CURRENT_RASTER_POSITION, s->raster);
    glGetDoublev(GL_CURRENT_RASTER_COLOR, s->raster_color);
    glGetDoublev(GL_CURRENT_RASTER_TEXTURE_COORDS, s->raster_tex);
    glGetDoublev(GL_CURRENT_RASTER_INDEX, &s->raster_index);
    glGetIntegerv(GL_CURRENT_RASTER_POSITION_VALID, &s->raster_valid);

    /* Evaluators and pixel maps, of the sizes they have. */
    size_t ncoeff = 0, npix = 0;
    for (int m = 0; m < NMAPS; m++) {
        s->map_order[m][1] = 1;
        glGetMapiv(maps[m], GL_ORDER, s->map_order[m]);
        glGetMapdv(maps[m], GL_DOMAIN, s->map_domain[m]);
        ncoeff += (size_t)s->map_order[m][0] * (size_t)(m >= 9 ? s->map_order[m][1] : 1) * (size_t)map_dims(maps[m]);
    }
    s->map_coeff = bb_malloc((ncoeff ? ncoeff : 1) * sizeof(GLdouble), BB_BYTES);
    for (int m = 0, at = 0; s->map_coeff && m < NMAPS; m++) {
        glGetMapdv(maps[m], GL_COEFF, s->map_coeff + at);
        at += s->map_order[m][0] * (m >= 9 ? s->map_order[m][1] : 1) * map_dims(maps[m]);
    }
    for (int m = 0; m < NPIXMAPS; m++) {
        glGetIntegerv(pixmaps[m] + (GL_PIXEL_MAP_I_TO_I_SIZE - GL_PIXEL_MAP_I_TO_I), &s->pixmap_size[m]);
        npix += (size_t)s->pixmap_size[m];
    }
    s->pixmap = bb_malloc((npix ? npix : 1) * sizeof(GLfloat), BB_BYTES);
    for (int m = 0, at = 0; s->pixmap && m < NPIXMAPS; m++) {
        glGetPixelMapfv(pixmaps[m], s->pixmap + at);
        at += s->pixmap_size[m];
    }

    /* Each texture unit's, then the matrices. */
    glGetIntegerv(GL_MATRIX_MODE, &s->matrix_mode);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s->active);
    s->nunits = units();
    for (int u = 0; u < s->nunits; u++) {
        glunit *t = &s->unit[u];
        glActiveTexture(GL_TEXTURE0 + (GLenum)u);
        for (int p = 0; p < NENV_PARAMS; p++)
            glGetTexEnvfv(GL_TEXTURE_ENV, env_params[p], t->env[p]);
        for (int q = 0; q < 4; q++) {
            glGetTexGeniv(coords[q], GL_TEXTURE_GEN_MODE, &t->gen_mode[q]);
            glGetTexGendv(coords[q], GL_OBJECT_PLANE, t->gen_object[q]);
            glGetTexGendv(coords[q], GL_EYE_PLANE, t->gen_eye[q]);
        }
        for (int b = 0; b < 3; b++)
            glGetIntegerv(bindings[b], &t->binding[b]);
        glGetDoublev(GL_CURRENT_TEXTURE_COORDS, t->texcoord);
        for (int i = 0; i < NUNIT_ENABLES; i++)
            t->enabled[i] = glIsEnabled(unit_enables[i]);
        capture_stack(GL_TEXTURE, GL_TEXTURE_STACK_DEPTH, GL_TEXTURE_MATRIX, &t->depth, t->matrix);
    }
    capture_stack(GL_MODELVIEW, GL_MODELVIEW_STACK_DEPTH, GL_MODELVIEW_MATRIX, &s->depth[0], s->matrix[0]);
    capture_stack(GL_PROJECTION, GL_PROJECTION_STACK_DEPTH, GL_PROJECTION_MATRIX, &s->depth[1], s->matrix[1]);
    glMatrixMode((GLenum)s->matrix_mode);

    /* The default textures, on unit 0. */
    glActiveTexture(GL_TEXTURE0);
    for (int b = 0; b < 3; b++) {
        glBindTexture(default_targets[b], 0);
        capture_texture(default_targets[b], 0, &s->defaults[b]);
        glBindTexture(default_targets[b], (GLuint)s->unit[0].binding[b]);
    }
    glActiveTexture((GLenum)s->active);

    /* The framebuffer, as it is: no pixel transfer on the way. */
    s->fw = h->width;
    s->fh = h->height;
    size_t n = (size_t)h->width * (size_t)h->height;
    glPushAttrib(GL_PIXEL_MODE_BIT);
    glPixelTransferi(GL_MAP_COLOR, GL_FALSE);
    glPixelTransferi(GL_MAP_STENCIL, GL_FALSE);
    glPixelTransferi(GL_INDEX_SHIFT, 0);
    glPixelTransferi(GL_INDEX_OFFSET, 0);
    static const GLenum scales[] = { GL_RED_SCALE, GL_GREEN_SCALE, GL_BLUE_SCALE, GL_ALPHA_SCALE, GL_DEPTH_SCALE };
    static const GLenum biases[] = { GL_RED_BIAS, GL_GREEN_BIAS, GL_BLUE_BIAS, GL_ALPHA_BIAS, GL_DEPTH_BIAS };
    for (int i = 0; i < 5; i++) {
        glPixelTransferf(scales[i], 1);
        glPixelTransferf(biases[i], 0);
    }
    s->color = n ? bb_malloc(n * 4, BB_BYTES) : NULL;
    if (s->color)
        glReadPixels(0, 0, h->width, h->height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, s->color);
    s->ds = n && h->depth ? bb_malloc(n * 4, BB_BYTES) : NULL;
    if (s->ds) {
        if (h->stencil_bits)
            glReadPixels(0, 0, h->width, h->height, GL_DEPTH_STENCIL_EXT, GL_UNSIGNED_INT_24_8_EXT, s->ds);
        else
            glReadPixels(0, 0, h->width, h->height, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, s->ds);
    }
    glPopAttrib();
    glPopClientAttrib();
    while (glGetError() != GL_NO_ERROR) { /* the program's were taken before */
    }
}

/* Into h's new context, its group's textures and lists made already. */
void glh_restore(hostgl_ctx *h)
{
    glsaved *s = h->hib;
    if (!s)
        return;
    GLfloat tmp[4];
    glh_pixel_store_defaults(true, true);

    /* The framebuffer first, while the context is as made. */
    if (s->color && s->fw == h->width && s->fh == h->height) {
        glWindowPos2i(0, 0);
        glDrawPixels(s->fw, s->fh, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, s->color);
        if (s->ds) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_ALWAYS);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            if (h->stencil_bits)
                glDrawPixels(s->fw, s->fh, GL_DEPTH_STENCIL_EXT, GL_UNSIGNED_INT_24_8_EXT, s->ds);
            else
                glDrawPixels(s->fw, s->fh, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, s->ds);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthFunc(GL_LESS);
            glDisable(GL_DEPTH_TEST);
        }
    }
    for (int b = 0; b < 3; b++)
        restore_texture(&s->defaults[b]);

    /* The raster position, with identity matrices and the depth range as
     * made; then what is kept in eye coordinates. */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    if (s->raster_valid) {
        glColor4dv(s->raster_color);
        glIndexd(s->raster_index);
        glMultiTexCoord4dv(GL_TEXTURE0, s->raster_tex);
        glWindowPos3d(s->raster[0], s->raster[1], s->raster[2]);
    } else {
        glRasterPos4d(0, 0, 2, 1); /* outside: not valid */
    }
    for (int l = 0; l < NLIGHTS; l++)
        for (int p = 0; p < NLIGHT_PARAMS; p++)
            glLightfv(GL_LIGHT0 + (GLenum)l, light_params[p], s->light[l][p]);
    for (int p = 0; p < NPLANES; p++)
        glClipPlane(GL_CLIP_PLANE0 + (GLenum)p, s->plane[p]);
    for (int u = 0; u < s->nunits; u++) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)u);
        for (int q = 0; q < 4; q++) {
            glTexGeni(coords[q], GL_TEXTURE_GEN_MODE, s->unit[u].gen_mode[q]);
            glTexGendv(coords[q], GL_OBJECT_PLANE, s->unit[u].gen_object[q]);
            glTexGendv(coords[q], GL_EYE_PLANE, s->unit[u].gen_eye[q]);
        }
    }
    glDisable(GL_COLOR_MATERIAL);
    for (int f = 0; f < 2; f++)
        for (int p = 0; p < NMATERIAL_PARAMS; p++)
            glMaterialfv(f ? GL_BACK : GL_FRONT, material_params[p], s->material[f][p]);

    int k = 0;
    const double *v;
#define SET(name, n, set) v = &s->v[k], k += (n), (void)v, (void)tmp, (void)(set);
    STATE(SET)
#undef SET
    glPolygonStipple(s->stipple);
    for (int m = 0, at = 0; s->pixmap && m < NPIXMAPS; m++) {
        if (s->pixmap_size[m] > 0)
            glPixelMapfv(pixmaps[m], s->pixmap_size[m], s->pixmap + at);
        at += s->pixmap_size[m];
    }
    for (int m = 0, at = 0; s->map_coeff && m < NMAPS; m++) {
        int d = map_dims(maps[m]), uo = s->map_order[m][0], vo = s->map_order[m][1];
        const GLdouble *dom = s->map_domain[m];
        if (m < 9) {
            glMap1d(maps[m], dom[0], dom[1], d, uo, s->map_coeff + at);
            at += uo * d;
        } else {
            glMap2d(maps[m], dom[0], dom[1], d * vo, uo, dom[2], dom[3], d, vo, s->map_coeff + at);
            at += uo * vo * d;
        }
    }

    for (int u = 0; u < s->nunits; u++) {
        glunit *t = &s->unit[u];
        glActiveTexture(GL_TEXTURE0 + (GLenum)u);
        for (int p = 0; p < NENV_PARAMS; p++)
            glTexEnvfv(GL_TEXTURE_ENV, env_params[p], t->env[p]);
        for (int b = 0; b < 3; b++)
            glBindTexture(default_targets[b], (GLuint)t->binding[b]);
        glMultiTexCoord4dv(GL_TEXTURE0 + (GLenum)u, t->texcoord);
        restore_stack(GL_TEXTURE, t->depth, t->matrix);
        for (int i = 0; i < NUNIT_ENABLES; i++)
            (t->enabled[i] ? glEnable : glDisable)(unit_enables[i]);
    }
    restore_stack(GL_PROJECTION, s->depth[1], s->matrix[1]);
    restore_stack(GL_MODELVIEW, s->depth[0], s->matrix[0]);
    glMatrixMode((GLenum)s->matrix_mode);
    for (int i = 0; i < NENABLES; i++)
        (s->enabled[i] ? glEnable : glDisable)(enables[i]);
    glActiveTexture((GLenum)s->active);
    for (int i = 0; i < 16; i++)
        glPixelStorei(store_names[i], s->store[i]);

    /* The client's side: which of the program's arrays are on. */
    arrays *a = &h->a;
    (a->vertex.on ? glEnableClientState : glDisableClientState)(GL_VERTEX_ARRAY);
    (a->normal.on ? glEnableClientState : glDisableClientState)(GL_NORMAL_ARRAY);
    (a->color.on ? glEnableClientState : glDisableClientState)(GL_COLOR_ARRAY);
    (a->index.on ? glEnableClientState : glDisableClientState)(GL_INDEX_ARRAY);
    (a->edge.on ? glEnableClientState : glDisableClientState)(GL_EDGE_FLAG_ARRAY);
    for (int u = 0; u < s->nunits; u++) {
        glClientActiveTexture(GL_TEXTURE0 + (GLenum)u);
        (a->tex[u].on ? glEnableClientState : glDisableClientState)(GL_TEXTURE_COORD_ARRAY);
    }
    glClientActiveTexture(GL_TEXTURE0 + h->client_unit);
    while (glGetError() != GL_NO_ERROR) {
    }
    glh_free(h);
}

void glh_free(hostgl_ctx *h)
{
    glsaved *s = h->hib;
    if (!s)
        return;
    for (int b = 0; b < 3; b++)
        free_texture(&s->defaults[b]);
    bb_free(s->map_coeff);
    bb_free(s->pixmap);
    bb_free(s->color);
    bb_free(s->ds);
    bb_free(s);
    h->hib = NULL;
}

size_t glh_saved_bytes(const hostgl_ctx *h)
{
    const glsaved *s = h->hib;
    if (!s)
        return 0;
    size_t n = sizeof *s;
    for (int b = 0; b < 3; b++)
        n += texture_bytes(&s->defaults[b]);
    if (s->color)
        n += (size_t)s->fw * (size_t)s->fh * 4;
    if (s->ds)
        n += (size_t)s->fw * (size_t)s->fh * 4;
    return n;
}
#endif
