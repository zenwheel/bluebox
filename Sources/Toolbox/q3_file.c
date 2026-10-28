/*
 * QuickDraw 3D's files (QD3DIO.h): 3DMF metafiles read and written, in
 * binary (either byte order) and text, on a storage object (q3_storage.c).
 *
 * Reading takes the storage's bytes whole.  An object is read as an
 * item: a finished object, an attribute (an element for a set), or what
 * a container's root is being built from (a data object's words, a
 * mesh's faces, an attribute set list, a trimesh's attribute array...).
 * A container's children are attached to its root item, then the root
 * becomes its object.  Begin and end groups gather what is between them
 * into the group; references go through the table of contents (binary)
 * or the labels (text).  What isn't known is kept as an unknown object.
 *
 * The primitive calls (Q3Uns32_Read and so on) read and write at the
 * file's place, in its format, so programs' own classes read and write
 * their data through them as Apple's did.
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "q3_view.h"
#include "tb_internal.h"

#define T4(s) Q3T(s[0], s[1], s[2], s[3])

enum {
    kQ3FileModeNormal = 0, kQ3FileModeStream = 1, kQ3FileModeDatabase = 2, kQ3FileModeText = 4,
    kQ3FileReadWholeGroup = 0, kQ3FileReadObjectsInGroup = 1, kQ3FileCurrentlyInsideGroup = 2,
    kQ3ErrorFileAlreadyOpen = -28443, kQ3ErrorReadMoreThanSize = -28428, kQ3ErrorInvalidMetafilePrimitive = -28454,
    kQ3WarningUnmatchedEndGroup = -28285, kQ3ErrorFileCancelled = -28456,
};

uint32_t q3_storage_size(gaddr storage);
uint8_t *q3_storage_bytes(gaddr storage, uint32_t *size);
uint32_t q3_storage_write(gaddr storage, uint32_t off, uint32_t count, const uint8_t *in);
void q3_storage_truncate(gaddr storage, uint32_t size);
bool q3_storage_open(gaddr storage);
void q3_storage_close(gaddr storage);
void q3_storage_classes(void);
gaddr q3_renderer_new(uint32_t type);
gaddr q3v_file(q3view *v);
const q3schema *q3_geom_schema(uint32_t type);

/* ---------------------------------------------------------------------- */
/* Names: 3DMF's text names and the types they stand for */

static const struct {
    uint32_t type;
    const char *name;
} names[] = {
    { T4("3DMF"), "3DMetafile" },     { T4("cntr"), "Container" },       { T4("bgng"), "BeginGroup" },
    { T4("endg"), "EndGroup" },       { T4("rfrn"), "Reference" },       { T4("toc "), "TableOfContents" },
    { T4("type"), "Type" },           { T4("kdif"), "DiffuseColor" },    { T4("kspc"), "SpecularColor" },
    { T4("cspc"), "SpecularControl" }, { T4("kxpr"), "TransparencyColor" }, { T4("nrml"), "Normal" },
    { T4("shuv"), "ShadingUV" },      { T4("sruv"), "SurfaceUV" },       { T4("camb"), "AmbientCoefficient" },
    { T4("srtn"), "SurfaceTangent" }, { T4("hlst"), "HighlightState" },  { T4("vasl"), "VertexAttributeSetList" },
    { T4("fasl"), "FaceAttributeSetList" }, { T4("gasl"), "GeometryAttributeSetList" },
    { T4("atar"), "AttributeArray" }, { T4("gplh"), "GeneralPolygonHint" }, { T4("dgst"), "DisplayGroupState" },
    { T4("caps"), "Caps" },           { T4("tcap"), "TopCapAttributeSet" }, { T4("bcap"), "BottomCapAttributeSet" },
    { T4("fcap"), "FaceCapAttributeSet" }, { T4("icap"), "InteriorCapAttributeSet" }, { T4("lght"), "LightData" },
    { T4("cmpl"), "CameraPlacement" }, { T4("cmrg"), "CameraRange" },    { T4("cmvp"), "CameraViewPort" },
    { T4("imdm"), "ImageDimensions" }, { T4("immk"), "ImageMask" },      { T4("imcc"), "ImageClearColor" },
    { T4("sduv"), "ShaderUVTransform" }, { T4("shdr"), "ShaderData" },   { T4("sdxf"), "ShaderTransform" },
    { T4("ssat"), "SurfaceShader" },  { T4("ctwn"), "InteractiveRenderer" }, { T4("strc"), "CString" },
};

static uint32_t type_of_name(const char *n)
{
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcasecmp(names[i].name, n))
            return names[i].type;
    q3class *k = q3_class_named(n);
    return k ? k->type : 0;
}

static const char *name_of_type(uint32_t type)
{
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (names[i].type == type)
            return names[i].name;
    q3class *k = q3_class(type);
    return k ? k->name : NULL;
}

/* Flags in text: their names. */
static const struct {
    const char *name;
    uint32_t value;
} flags[] = {
    { "False", 0 },        { "True", 1 },          { "Off", 0 },          { "On", 1 },
    { "BigEndian", 0 },    { "LittleEndian", 1 },  { "Normal", 0 },       { "Stream", 1 },
    { "Database", 2 },     { "Include", 0 },       { "Exclude", 1 },      { "RGB32", 0 },
    { "ARGB32", 1 },       { "RGB16", 2 },         { "ARGB16", 3 },       { "RGB16_565", 4 },
    { "RGB24", 5 },        { "Complex", 0 },       { "Concave", 1 },      { "Convex", 2 },
    { "Both", 0 },         { "Culled", 1 },        { "Remove", 1 },       { "Flipped", 2 },
    { "Flip", 2 },         { "None", 0 },          { "Top", 1 },          { "Bottom", 2 },
    { "Interior", 4 },     { "Vertex", 1 },        { "Pixel", 2 },        { "Filled", 0 },
    { "Edges", 1 },        { "Points", 2 },        { "CounterClockwise", 0 }, { "Clockwise", 1 },
    { "Constant", 0 },     { "WorldSpace", 1 },    { "ScreenSpace", 2 },  { "X", 0 },
    { "Y", 1 },            { "Z", 2 },             { "Inline", 2 },       { "DontDraw", 1 },
    { "NoBoundingBox", 4 }, { "NoBoundingSphere", 8 }, { "DontPick", 16 }, { "Linear", 0 },
    { "Exponential", 1 },  { "ExponentialSquared", 2 }, { "Alpha", 3 },  { "InverseDistance", 1 },
    { "InverseDistanceSquared", 2 }, { "Cosine", 3 }, { "Face", 1 },       { "Edge", 2 },
    { "Object", 0 },
};

/* ---------------------------------------------------------------------- */
/* The file object */

typedef struct {
    uint32_t refid, pos;
    char label[64];
    gaddr obj;
} tocent;

typedef struct {
    int32_t id;
    char name[128];
} typeent;

enum { FR_OBJECT, FR_CONTAINER };
typedef struct {
    int kind;
    uint32_t end; /* binary: where it ends */
    int depth;    /* text: the depth inside its parentheses */
} frame;

typedef struct {
    gaddr storage;
    bool open, writing, text, swap;
    uint32_t mode, version;
    /* Reading. */
    uint8_t *b;
    uint32_t n, pos, eof;
    int depth;
    frame fr[64];
    int nfr;
    tocent *toc;
    int ntoc;
    typeent *types;
    int ntypes;
    uint32_t read_group;
    int in_group;
    gaddr ext_refs;
    gaddr idle, idle_data;
    /* Writing. */
    uint8_t *o;
    uint32_t olen, ocap;
    uint32_t wsizes[64];
    int nw, tdepth;
    bool line_start;
    int32_t next_custom;
} q3file;

#define FILE_TYPE Q3_FILE

static q3file *file_of(gaddr g)
{
    q3obj *o = q3_get(g);
    return o && o->type == FILE_TYPE ? o->data : NULL;
}

static void close_file(q3file *f, bool write_out);

static void file_dispose(q3obj *o)
{
    q3file *f = o->data;
    if (!f)
        return;
    close_file(f, true);
    q3_assign(&f->storage, 0);
    q3_assign(&f->ext_refs, 0);
    free(f);
}

/* ---------------------------------------------------------------------- */
/* Reading: bytes and tokens */

static uint32_t limit_of(const q3file *f)
{
    for (int i = f->nfr - 1; i >= 0; i--)
        if (f->fr[i].kind == FR_OBJECT || f->fr[i].kind == FR_CONTAINER)
            return f->fr[i].end < f->eof ? f->fr[i].end : f->eof;
    return f->eof;
}

static bool raw(q3file *f, uint8_t *out, uint32_t n)
{
    if (f->pos + n > limit_of(f) || f->pos + n < f->pos) {
        q3_error(kQ3ErrorReadMoreThanSize);
        return false;
    }
    memcpy(out, f->b + f->pos, n);
    f->pos += n;
    return true;
}

static bool bin_u(q3file *f, int bytes, uint64_t *v)
{
    uint8_t b[8];
    if (!raw(f, b, (uint32_t)bytes))
        return false;
    *v = 0;
    for (int i = 0; i < bytes; i++)
        *v = *v << 8 | b[f->swap ? bytes - 1 - i : i];
    return true;
}

/* Text tokens. */
enum { TK_END, TK_LP, TK_RP, TK_WORD, TK_NUM, TK_STR, TK_HEX };

static void skip_space(q3file *f)
{
    while (f->pos < f->eof) {
        uint8_t ch = f->b[f->pos];
        if (ch == '#') {
            while (f->pos < f->eof && f->b[f->pos] != '\n' && f->b[f->pos] != '\r')
                f->pos++;
        } else if (isspace(ch) || ch == ',' || ch == 0) {
            f->pos++;
        } else {
            break;
        }
    }
}

/* The next token (into buf), consumed or not. */
static int token(q3file *f, char *buf, size_t size, bool consume)
{
    uint32_t save = f->pos;
    skip_space(f);
    int kind = TK_END;
    size_t n = 0;
    buf[0] = 0;
    if (f->pos >= f->eof) {
        kind = TK_END;
    } else if (f->b[f->pos] == '(') {
        kind = TK_LP;
        f->pos++;
    } else if (f->b[f->pos] == ')') {
        kind = TK_RP;
        f->pos++;
    } else if (f->b[f->pos] == '"') {
        kind = TK_STR;
        f->pos++;
        while (f->pos < f->eof && f->b[f->pos] != '"') {
            uint8_t ch = f->b[f->pos++];
            if (ch == '\\' && f->pos < f->eof) {
                ch = f->b[f->pos++];
                ch = ch == 'n' ? '\n' : ch == 't' ? '\t' : ch == 'r' ? '\r' : ch;
            }
            if (n + 1 < size)
                buf[n++] = (char)ch;
        }
        f->pos++;
    } else {
        uint8_t ch = f->b[f->pos];
        kind = isdigit(ch) || ch == '-' || ch == '+' || ch == '.' ? TK_NUM : TK_WORD;
        if (ch == '0' && f->pos + 1 < f->eof && (f->b[f->pos + 1] == 'x' || f->b[f->pos + 1] == 'X'))
            kind = TK_HEX;
        while (f->pos < f->eof) {
            ch = f->b[f->pos];
            if (isspace(ch) || ch == '(' || ch == ')' || ch == ',' || ch == '"' || ch == '#' || ch == 0)
                break;
            if (n + 1 < size)
                buf[n++] = (char)ch;
            f->pos++;
            if (ch == ':' || ch == '>')
                break; /* a label ends there */
        }
    }
    buf[n] = 0;
    if (kind == TK_NUM) { /* "3DMetafile" begins with a digit and isn't one */
        char *end;
        strtod(buf, &end);
        if (*end)
            kind = TK_WORD;
    }
    if (!consume)
        f->pos = save;
    else if (kind == TK_LP)
        f->depth++;
    else if (kind == TK_RP)
        f->depth--;
    return kind;
}

/* At the end of the innermost object's data. */
static bool at_data_end(q3file *f)
{
    if (!f->text)
        return f->pos >= limit_of(f);
    char t[8];
    int k = token(f, t, sizeof t, false);
    return k == TK_RP || k == TK_END;
}

/* ---------------------------------------------------------------------- */
/* Reading: the primitives */

static bool text_number(q3file *f, double *v, bool *isint)
{
    char t[256];
    if (at_data_end(f)) {
        q3_error(kQ3ErrorReadMoreThanSize);
        return false;
    }
    int k = token(f, t, sizeof t, true);
    if (k == TK_HEX) {
        *v = (double)strtoull(t, NULL, 16);
        *isint = true;
        return true;
    }
    if (k == TK_NUM) {
        char *end;
        *isint = !strpbrk(t, ".eE");
        *v = *isint ? (double)strtoll(t, &end, 10) : strtod(t, &end);
        return true;
    }
    if (k == TK_WORD) { /* a flag's name, or "|" joined ones */
        uint32_t val = 0;
        bool found = false;
        char *save = NULL;
        for (char *p = strtok_r(t, "|", &save); p; p = strtok_r(NULL, "|", &save))
            for (unsigned i = 0; i < sizeof flags / sizeof flags[0]; i++)
                if (!strcasecmp(flags[i].name, p)) {
                    val |= flags[i].value;
                    found = true;
                    break;
                }
        /* Flags joined by spaces round "|": "Top | Bottom". */
        char nt[64];
        while (token(f, nt, sizeof nt, false) == TK_WORD && !strcmp(nt, "|")) {
            token(f, nt, sizeof nt, true);
            if (token(f, nt, sizeof nt, true) == TK_WORD)
                for (unsigned i = 0; i < sizeof flags / sizeof flags[0]; i++)
                    if (!strcasecmp(flags[i].name, nt))
                        val |= flags[i].value;
        }
        *v = val;
        *isint = true;
        if (!found)
            q3_error(kQ3ErrorInvalidMetafilePrimitive);
        return found;
    }
    q3_error(kQ3ErrorInvalidMetafilePrimitive);
    return false;
}

static bool r_u(q3file *f, int bytes, uint64_t *v)
{
    if (!f->text)
        return bin_u(f, bytes, v);
    double d;
    bool isint;
    if (!text_number(f, &d, &isint))
        return false;
    *v = d < 0 ? (uint64_t)(int64_t)d : (uint64_t)d;
    return true;
}

static bool r_u32(q3file *f, uint32_t *v)
{
    uint64_t x = 0;
    bool ok = r_u(f, 4, &x);
    *v = (uint32_t)x;
    return ok;
}

static bool r_f32(q3file *f, float *v)
{
    if (!f->text) {
        uint64_t x;
        if (!bin_u(f, 4, &x))
            return false;
        uint32_t u = (uint32_t)x;
        memcpy(v, &u, 4);
        return true;
    }
    double d;
    bool isint;
    if (!text_number(f, &d, &isint))
        return false;
    *v = (float)d;
    return true;
}

static bool r_f64(q3file *f, double *v)
{
    if (!f->text) {
        uint64_t x;
        if (!bin_u(f, 8, &x))
            return false;
        memcpy(v, &x, 8);
        return true;
    }
    bool isint;
    return text_number(f, v, &isint);
}

static bool r_fs(q3file *f, float *v, int n)
{
    for (int i = 0; i < n; i++)
        if (!r_f32(f, &v[i]))
            return false;
    return true;
}

/* A string (C): binary, NUL-terminated and padded to 4; text, quoted. */
static bool r_str(q3file *f, char *out, uint32_t size, uint32_t *len)
{
    if (f->text) {
        char t[1024];
        if (at_data_end(f) || token(f, t, sizeof t, true) != TK_STR) {
            q3_error(kQ3ErrorInvalidMetafilePrimitive);
            return false;
        }
        uint32_t n = (uint32_t)strlen(t);
        if (out && size) {
            uint32_t c = n < size - 1 ? n : size - 1;
            memcpy(out, t, c);
            out[c] = 0;
        }
        if (len)
            *len = n;
        return true;
    }
    uint32_t start = f->pos, lim = limit_of(f), n = 0;
    while (start + n < lim && f->b[start + n])
        n++;
    if (start + n >= lim) {
        q3_error(kQ3ErrorReadMoreThanSize);
        return false;
    }
    if (out && size) {
        uint32_t c = n < size - 1 ? n : size - 1;
        memcpy(out, f->b + start, c);
        out[c] = 0;
    }
    if (len)
        *len = n;
    f->pos = start + ((n + 1 + 3) & ~3u);
    if (f->pos > lim)
        f->pos = lim;
    return true;
}

/* Raw bytes: binary as they are (and padded to 4 after); text as hex. */
static bool r_raw(q3file *f, uint8_t *out, uint32_t n)
{
    if (!f->text) {
        if (!raw(f, out, n))
            return false;
        uint32_t pad = (4 - (f->pos & 3)) & 3;
        f->pos = f->pos + pad <= limit_of(f) ? f->pos + pad : limit_of(f);
        return true;
    }
    uint32_t got = 0;
    char t[4096];
    while (got < n && !at_data_end(f)) {
        int k = token(f, t, sizeof t, true);
        if (k != TK_HEX && k != TK_NUM)
            return false;
        const char *p = k == TK_HEX ? t + 2 : t;
        if (k == TK_NUM) { /* a byte as a number */
            out[got++] = (uint8_t)strtoul(t, NULL, 0);
            continue;
        }
        for (; p[0] && p[1] && got < n; p += 2) {
            char hx[3] = { p[0], p[1], 0 };
            out[got++] = (uint8_t)strtoul(hx, NULL, 16);
        }
    }
    return got == n;
}

/* ---------------------------------------------------------------------- */
/* Reading: objects as items */

enum { IT_NONE, IT_OBJ, IT_ELEM, IT_ENDG, IT_BUILD, IT_ASL, IT_ARRAY, IT_WORDS, IT_MESH };

typedef struct {
    float (*pts)[3];
    gaddr *vsets, *fsets;
    uint32_t nverts, nfaces, nidx;
    uint32_t *fcount, *fidx;
    gaddr set;
} meshb;

typedef struct {
    int kind;
    uint32_t type;
    gaddr g;
    q3node *node;
    /* An element. */
    int32_t etype;
    uint8_t eb[24];
    uint32_t esize;
    /* Small payloads. */
    uint32_t w[18];
    int nw;
    /* An attribute set list. */
    uint32_t asl_n;
    gaddr *asl;
    /* An attribute array. */
    uint32_t a_pos, a_index;
    uint8_t *a_data, *a_use;
    uint32_t a_bytes, a_nuse;
    meshb *mesh;
} item;

static item read_item(q3file *f);
static gaddr finish(item *it);
static void read_array_at(q3file *f, item *root, item *a);

static void free_item(item *it)
{
    if (it->kind == IT_OBJ && it->g)
        q3_release(it->g);
    if (it->kind == IT_BUILD && it->node)
        q3d_free(it->node, q3_schema_of(it->type));
    if (it->kind == IT_ELEM && it->etype == kQ3AttributeTypeSurfaceShader) {
        gaddr sh = (uint32_t)it->eb[0] << 24 | (uint32_t)it->eb[1] << 16 | (uint32_t)it->eb[2] << 8 | it->eb[3];
        q3_release(sh);
    }
    if (it->kind == IT_ASL) {
        for (uint32_t i = 0; i < it->asl_n; i++)
            if (it->asl[i] > 1)
                q3_release(it->asl[i]);
        free(it->asl);
    }
    if (it->kind == IT_WORDS && (it->type == T4("tcap") || it->type == T4("bcap") || it->type == T4("fcap") ||
                                  it->type == T4("icap")) && it->w[0])
        q3_release(it->w[0]);
    if (it->kind == IT_MESH && it->mesh) {
        meshb *m = it->mesh;
        for (uint32_t i = 0; m->vsets && i < m->nverts; i++)
            if (m->vsets[i])
                q3_release(m->vsets[i]);
        for (uint32_t i = 0; m->fsets && i < m->nfaces; i++)
            if (m->fsets[i])
                q3_release(m->fsets[i]);
        if (m->set)
            q3_release(m->set);
        free(m->pts);
        free(m->vsets);
        free(m->fsets);
        free(m->fcount);
        free(m->fidx);
        free(m);
    }
    free(it->a_data);
    free(it->a_use);
    memset(it, 0, sizeof *it);
}

static void push(q3file *f, int kind, uint32_t end)
{
    if (f->nfr < 64)
        f->fr[f->nfr] = (frame){ kind, end, f->depth };
    f->nfr++;
}

static void pop(q3file *f)
{
    if (f->nfr > 0)
        f->nfr--;
}

/* Text: past this object's closing parenthesis. */
static void close_paren(q3file *f, int depth)
{
    char t[256];
    while (f->depth >= depth) {
        int k = token(f, t, sizeof t, true);
        if (k == TK_END)
            break;
    }
}

static q3node *new_node(uint32_t type)
{
    const q3schema *s = q3_schema_of(type);
    return s ? q3d_new(s, 1) : NULL;
}

static q3node *bytes_node(const uint8_t *b, uint32_t n)
{
    q3node *k = calloc(1, sizeof *k);
    if (!k)
        return NULL;
    k->bytes = malloc(n ? n : 1);
    k->nbytes = n;
    if (k->bytes && n)
        memcpy(k->bytes, b, n);
    return k;
}

/* An array for pointer field `field` of a node's top element. */
static q3node *kid(q3node *nd, uint32_t type, int field, uint32_t n)
{
    const q3schema *s = q3_schema_of(type);
    if (!s || field >= s->nf)
        return NULL;
    q3node *k = q3d_new(s->f[field].elem, n);
    q3d_set_kid(nd, s, 0, field, k);
    return k;
}

static void setf(uint32_t *w, int i, float v) { q3d_setf(w, i, v); }

static bool read_points(q3file *f, q3node *arr, uint32_t n, uint32_t stride)
{
    for (uint32_t i = 0; i < n; i++) {
        float p[3];
        if (!r_fs(f, p, 3))
            return false;
        for (int k = 0; k < 3; k++)
            setf(arr->w, (int)(i * stride) + k, p[k]);
    }
    return true;
}

/* An index, sized by how many things it counts from. */
static bool r_index(q3file *f, uint32_t count, uint32_t *v)
{
    uint64_t x = 0;
    int bytes = count >= 0x10000 ? 4 : count >= 0x100 ? 2 : 1;
    if (!r_u(f, bytes, &x))
        return false;
    if (bytes == 1 && x == 0xFF)
        x = 0xFFFFFFFF;
    else if (bytes == 2 && x == 0xFFFF)
        x = 0xFFFFFFFF;
    *v = (uint32_t)x;
    return true;
}

static void camera_defaults(uint32_t *w)
{
    setf(w, 2, 10);              /* at (0, 0, 10) */
    setf(w, 7, 1);               /* up y */
    setf(w, 9, 1);               /* hither */
    setf(w, 10, 1000);           /* yon */
    setf(w, 11, -1);
    setf(w, 12, 1);
    setf(w, 13, 2);
    setf(w, 14, 2);
}

gaddr q3_memory_storage_new(const uint8_t *b, uint32_t n);
static gaddr memory_storage(const uint8_t *b, uint32_t n) { return q3_memory_storage_new(b, n); }

/* A pixmap's fields and image: width, height, rowBytes, pixelSize,
 * pixelType, bitOrder, byteOrder, then the image. */
static bool read_pixmap(q3file *f, uint32_t *w)
{
    uint32_t v[7];
    for (int i = 0; i < 7; i++)
        if (!r_u32(f, &v[i]))
            return false;
    uint32_t n = v[1] * v[2];
    if (n > (64u << 20))
        return false;
    uint8_t *img = malloc(n ? n : 1);
    if (!img || !r_raw(f, img, n)) {
        free(img);
        return false;
    }
    w[0] = memory_storage(img, n);
    free(img);
    for (int i = 0; i < 7; i++)
        w[1 + i] = v[i];
    return w[0] != 0;
}

/* The data of a known type, at the object's place. */
static item parse(q3file *f, uint32_t type)
{
    item it;
    memset(&it, 0, sizeof it);
    it.type = type;
    float v[16];
    uint32_t u;

    /* Attributes. */
    static const struct {
        uint32_t code;
        int32_t etype;
        int floats;
    } attrs[] = {
        { T4("kdif"), kQ3AttributeTypeDiffuseColor, 3 },  { T4("kspc"), kQ3AttributeTypeSpecularColor, 3 },
        { T4("cspc"), kQ3AttributeTypeSpecularControl, 1 }, { T4("kxpr"), kQ3AttributeTypeTransparencyColor, 3 },
        { T4("nrml"), kQ3AttributeTypeNormal, 3 },        { T4("shuv"), kQ3AttributeTypeShadingUV, 2 },
        { T4("sruv"), kQ3AttributeTypeSurfaceUV, 2 },     { T4("camb"), kQ3AttributeTypeAmbientCoefficient, 1 },
        { T4("srtn"), kQ3AttributeTypeSurfaceTangent, 6 }, { T4("hlst"), kQ3AttributeTypeHighlightState, -1 },
    };
    for (unsigned i = 0; i < sizeof attrs / sizeof attrs[0]; i++)
        if (attrs[i].code == type) {
            it.kind = IT_ELEM;
            it.etype = attrs[i].etype;
            int n = attrs[i].floats < 0 ? 1 : attrs[i].floats;
            for (int k = 0; k < n; k++) {
                uint32_t x = 0;
                if (attrs[i].floats < 0)
                    r_u32(f, &x);
                else {
                    float fl = 0;
                    r_f32(f, &fl);
                    memcpy(&x, &fl, 4);
                }
                it.eb[4 * k] = (uint8_t)(x >> 24), it.eb[4 * k + 1] = (uint8_t)(x >> 16);
                it.eb[4 * k + 2] = (uint8_t)(x >> 8), it.eb[4 * k + 3] = (uint8_t)x;
            }
            it.esize = (uint32_t)n * 4;
            return it;
        }

    switch (type) {
    /* Words for a root to take. */
    case T4("gplh"): case T4("caps"): case T4("dgst"):
        it.kind = IT_WORDS;
        it.nw = 1;
        r_u32(f, &it.w[0]);
        return it;
    case T4("lght"):
        it.kind = IT_WORDS;
        it.nw = 5;
        r_u32(f, &it.w[0]);
        r_fs(f, v, 4);
        for (int k = 0; k < 4; k++)
            setf(it.w, 1 + k, v[k]);
        return it;
    case T4("cmpl"): case T4("cmrg"): case T4("cmvp"): case T4("sduv"): case T4("imcc"): case T4("sdxf"): {
        int n = type == T4("cmpl") || type == T4("sduv") ? 9 : type == T4("cmrg") ? 2 : type == T4("sdxf") ? 16 : 4;
        it.kind = IT_WORDS;
        it.nw = n;
        r_fs(f, v, n);
        for (int k = 0; k < n; k++)
            setf(it.w, k, v[k]);
        return it;
    }
    case T4("shdr"): case T4("imdm"):
        it.kind = IT_WORDS;
        it.nw = 2;
        r_u32(f, &it.w[0]);
        r_u32(f, &it.w[1]);
        return it;
    case T4("tcap"): case T4("bcap"): case T4("fcap"): case T4("icap"):
        it.kind = IT_WORDS;
        it.nw = 1;
        return it;
    case T4("vasl"): case T4("fasl"): case T4("gasl"): {
        it.kind = IT_ASL;
        uint32_t total = 0, packing = 0, nidx = 0;
        r_u32(f, &total);
        r_u32(f, &packing);
        r_u32(f, &nidx);
        if (total > (1u << 24))
            total = 0;
        it.asl_n = total;
        it.asl = calloc(total + 1, sizeof *it.asl);
        /* Which of them have sets: w[] would be too small; mark in asl
         * with 1 those that do, filled in order by the children. */
        bool *has = calloc(total + 1, 1);
        for (uint32_t i = 0; has && i < total; i++)
            has[i] = packing == 1; /* exclude: all but those listed */
        for (uint32_t i = 0; i < nidx; i++) {
            uint32_t x = 0;
            r_index(f, total, &x);
            if (has && x < total)
                has[x] = packing == 0;
        }
        if (has && packing == 0 && nidx == total)
            for (uint32_t i = 0; i < total; i++)
                has[i] = true;
        for (uint32_t i = 0; it.asl && has && i < total; i++)
            it.asl[i] = has[i] ? 1 : 0;
        free(has);
        return it;
    }
    case T4("atar"): {
        it.kind = IT_ARRAY;
        uint32_t reserved = 0, use = 0;
        r_u32(f, &it.w[0]); /* the attribute type */
        r_u32(f, &reserved);
        r_u32(f, &it.a_pos);
        r_u32(f, &it.a_index);
        r_u32(f, &use);
        it.w[1] = use;
        return it; /* the data follows, read when attached: its count is the trimesh's */
    }
    case T4("strc"): {
        char s[1024];
        uint32_t len = 0;
        r_str(f, s, sizeof s, &len);
        q3obj *o = q3_new(Q3_CSTRING, strdup(s));
        it.kind = IT_OBJ;
        it.g = o ? o->g : 0;
        return it;
    }
    case T4("attr"): case T4("set "): {
        q3set *s = calloc(1, sizeof *s);
        q3obj *o = s ? q3_new(type == T4("attr") ? Q3_ATTRIBUTE_SET : Q3_SET, s) : NULL;
        if (!o)
            free(s);
        it.kind = IT_OBJ;
        it.g = o ? o->g : 0;
        return it;
    }
    case T4("grup"): case T4("dspg"): case T4("ordg"): case T4("iopx"): case T4("lghg"): case T4("info"): {
        q3group *g = calloc(1, sizeof *g);
        q3obj *o = g ? q3_new(type, g) : NULL;
        if (!o)
            free(g);
        else
            g->state = 1 | 4 | 8 | 16 | 32;
        it.kind = IT_OBJ;
        it.g = o ? o->g : 0;
        return it;
    }
    case T4("ctwn"): case T4("wrfr"): case T4("gnrr"):
        it.kind = IT_OBJ;
        it.g = q3_renderer_new(type);
        return it;
    case T4("mesh"): {
        meshb *m = calloc(1, sizeof *m);
        it.kind = IT_MESH;
        it.mesh = m;
        if (!m)
            return it;
        r_u32(f, &m->nverts);
        if (m->nverts > (1u << 24))
            m->nverts = 0;
        m->pts = calloc(m->nverts + 1, sizeof *m->pts);
        m->vsets = calloc(m->nverts + 1, sizeof *m->vsets);
        for (uint32_t i = 0; m->pts && i < m->nverts; i++)
            r_fs(f, m->pts[i], 3);
        uint32_t nfaces = 0, ncont = 0;
        r_u32(f, &nfaces);
        r_u32(f, &ncont);
        uint32_t total = nfaces + ncont;
        if (total > (1u << 24))
            total = 0;
        m->fcount = calloc(total + 1, sizeof *m->fcount);
        m->fsets = calloc(total + 1, sizeof *m->fsets);
        uint32_t cap = 64;
        m->fidx = malloc(cap * sizeof *m->fidx);
        for (uint32_t i = 0; m->fcount && m->fidx && i < total; i++) {
            uint32_t cnt = 0;
            r_u32(f, &cnt);
            int32_t sc = (int32_t)cnt;
            uint32_t n = (uint32_t)(sc < 0 ? -sc : sc);
            if (n > (1u << 20))
                break;
            m->fcount[m->nfaces++] = sc < 0 ? (n | 0x80000000u) : n;
            for (uint32_t j = 0; j < n; j++) {
                uint32_t x = 0;
                r_u32(f, &x);
                if (m->nidx == cap) {
                    cap *= 2;
                    uint32_t *nn = realloc(m->fidx, cap * sizeof *nn);
                    if (!nn)
                        break;
                    m->fidx = nn;
                }
                m->fidx[m->nidx++] = x < m->nverts ? x : 0;
            }
        }
        return it;
    }
    default: break;
    }

    /* Data objects. */
    q3node *nd = new_node(type);
    if (!nd)
        return it; /* none: not a type we know */
    it.kind = IT_BUILD;
    it.node = nd;
    uint32_t *w = nd->w;
    switch (type) {
    case T4("box "):
        setf(w, 4, 1), setf(w, 8, 1), setf(w, 9, 1);
        if (!at_data_end(f)) {
            r_fs(f, v, 12);
            for (int k = 0; k < 3; k++) {
                setf(w, 3 + k, v[k]);
                setf(w, 6 + k, v[3 + k]);
                setf(w, 9 + k, v[6 + k]);
                setf(w, k, v[9 + k]);
            }
        }
        break;
    case T4("cone"): case T4("cyln"): case T4("elpd"): case T4("tors"): {
        bool torus = type == T4("tors");
        setf(w, 4, 1), setf(w, 8, 1), setf(w, 9, 1);
        int uv = torus ? 13 : 12;
        setf(w, uv + 1, 1), setf(w, uv + 3, 1);
        if (torus)
            setf(w, 12, 1);
        if (type == T4("elpd")) /* the default ellipsoid: the unit sphere */
            setf(w, 4, 1);
        if (!at_data_end(f)) {
            r_fs(f, v, 12);
            for (int k = 0; k < 3; k++) {
                setf(w, 3 + k, v[k]);
                setf(w, 6 + k, v[3 + k]);
                setf(w, 9 + k, v[6 + k]);
                setf(w, k, v[9 + k]);
            }
            if (torus && !at_data_end(f) && r_f32(f, &v[0]))
                setf(w, 12, v[0]);
            for (int k = 0; k < 4 && !at_data_end(f); k++)
                if (r_f32(f, &v[0]))
                    setf(w, uv + k, v[0]);
        }
        break;
    }
    case T4("disk"): case T4("elps"):
        setf(w, 3, 1), setf(w, 7, 1), setf(w, 10, 1);
        if (type == T4("disk"))
            setf(w, 12, 1);
        if (!at_data_end(f)) {
            r_fs(f, v, 9);
            for (int k = 0; k < 3; k++) {
                setf(w, 3 + k, v[k]);
                setf(w, 6 + k, v[3 + k]);
                setf(w, k, v[6 + k]);
            }
            for (int k = 0; k < (type == T4("disk") ? 4 : 2) && !at_data_end(f); k++)
                if (r_f32(f, &v[0]))
                    setf(w, 9 + k, v[0]);
        }
        break;
    case T4("gpgn"): {
        r_u32(f, &w[0]);
        if (w[0] > (1u << 20))
            w[0] = 0;
        q3node *cs = kid(nd, type, 0, w[0]);
        const q3schema *ss = q3_schema_of(type);
        for (uint32_t c = 0; cs && c < w[0]; c++) {
            uint32_t n = 0;
            r_u32(f, &n);
            if (n > (1u << 22))
                n = 0;
            cs->w[c * 2] = n;
            q3node *vs = q3d_new(ss->f[0].elem->f[0].elem, n);
            q3d_set_kid(cs, ss->f[0].elem, c, 0, vs);
            if (vs)
                read_points(f, vs, n, 4);
        }
        break;
    }
    case T4("line"):
        for (int i = 0; i < 2; i++) {
            r_fs(f, v, 3);
            for (int k = 0; k < 3; k++)
                setf(w, i * 4 + k, v[k]);
        }
        break;
    case T4("trng"):
        for (int i = 0; i < 3; i++) {
            r_fs(f, v, 3);
            for (int k = 0; k < 3; k++)
                setf(w, i * 4 + k, v[k]);
        }
        break;
    case T4("pnt "):
        r_fs(f, v, 3);
        for (int k = 0; k < 3; k++)
            setf(w, k, v[k]);
        break;
    case T4("plyg"): case T4("plyl"): {
        r_u32(f, &w[0]);
        if (w[0] > (1u << 22))
            w[0] = 0;
        q3node *vs = kid(nd, type, 0, w[0]);
        if (vs)
            read_points(f, vs, w[0], 4);
        break;
    }
    case T4("trig"): {
        r_u32(f, &w[0]);
        r_u32(f, &w[1]);
        uint64_t n = (uint64_t)w[0] * w[1];
        if (n > (1u << 22))
            w[0] = w[1] = 0, n = 0;
        q3node *vs = kid(nd, type, 0, (uint32_t)n);
        if (vs)
            read_points(f, vs, (uint32_t)n, 4);
        break;
    }
    case T4("plhd"): {
        r_u32(f, &w[0]);
        if (w[0] > (1u << 22))
            w[0] = 0;
        q3node *vs = kid(nd, type, 0, w[0]);
        if (vs)
            read_points(f, vs, w[0], 4);
        r_u32(f, &w[4]);
        if (w[4] > (1u << 22))
            w[4] = 0;
        q3node *ts = kid(nd, type, 2, w[4]);
        for (uint32_t i = 0; ts && i < w[4]; i++) {
            for (int k = 0; k < 3; k++)
                r_index(f, w[0], &ts->w[i * 5 + (uint32_t)k]);
            r_u32(f, &ts->w[i * 5 + 3]);
        }
        if (!at_data_end(f)) {
            r_u32(f, &w[2]);
            if (w[2] > (1u << 22))
                w[2] = 0;
            q3node *es = kid(nd, type, 1, w[2]);
            for (uint32_t i = 0; es && i < w[2]; i++) {
                r_index(f, w[0], &es->w[i * 5]);
                r_index(f, w[0], &es->w[i * 5 + 1]);
                r_index(f, w[4], &es->w[i * 5 + 2]);
                r_index(f, w[4], &es->w[i * 5 + 3]);
            }
        }
        break;
    }
    case T4("tmsh"): {
        uint32_t nt = 0, nta = 0, ne = 0, nea = 0, np = 0, nva = 0;
        r_u32(f, &nt), r_u32(f, &nta), r_u32(f, &ne), r_u32(f, &nea), r_u32(f, &np), r_u32(f, &nva);
        if (nt > (1u << 24) || ne > (1u << 24) || np > (1u << 24) || nta > 64 || nea > 64 || nva > 64)
            break;
        w[1] = nt, w[3] = nta, w[5] = ne, w[7] = nea, w[9] = np, w[11] = nva;
        q3node *tris = kid(nd, type, 1, nt);
        for (uint32_t i = 0; tris && i < nt * 3; i++)
            r_index(f, np, &tris->w[i]);
        q3node *edges = kid(nd, type, 3, ne);
        for (uint32_t i = 0; edges && i < ne; i++) {
            r_index(f, np, &edges->w[i * 4]);
            r_index(f, np, &edges->w[i * 4 + 1]);
            r_index(f, np, &edges->w[i * 4 + 2]);
            r_index(f, np, &edges->w[i * 4 + 3]);
        }
        q3node *pts = kid(nd, type, 5, np);
        if (pts)
            read_points(f, pts, np, 3);
        r_fs(f, v, 6);
        for (int k = 0; k < 6; k++)
            setf(w, 13 + k, v[k]);
        r_u32(f, &w[19]);
        kid(nd, type, 2, nta);
        kid(nd, type, 4, nea);
        kid(nd, type, 6, nva);
        break;
    }
    case T4("nrbc"): {
        r_u32(f, &w[0]);
        r_u32(f, &w[1]);
        if (w[0] > 64 || w[1] > (1u << 20))
            w[0] = w[1] = 0;
        q3node *cp = kid(nd, type, 0, w[1]);
        for (uint32_t i = 0; cp && i < w[1] * 4; i++)
            r_f32(f, (float *)&cp->w[i]);
        q3node *kn = kid(nd, type, 1, w[0] + w[1]);
        for (uint32_t i = 0; kn && i < w[0] + w[1]; i++)
            r_f32(f, (float *)&kn->w[i]);
        break;
    }
    case T4("nrbp"): {
        for (int k = 0; k < 4; k++)
            r_u32(f, &w[k]);
        if (w[0] > 64 || w[1] > 64 || (uint64_t)w[2] * w[3] > (1u << 20)) {
            memset(w, 0, 4 * 4);
            break;
        }
        q3node *cp = kid(nd, type, 0, w[2] * w[3]);
        for (uint32_t i = 0; cp && i < w[2] * w[3] * 4; i++)
            r_f32(f, (float *)&cp->w[i]);
        q3node *uk = kid(nd, type, 1, w[0] + w[3]);
        for (uint32_t i = 0; uk && i < w[0] + w[3]; i++)
            r_f32(f, (float *)&uk->w[i]);
        q3node *vk = kid(nd, type, 2, w[1] + w[2]);
        for (uint32_t i = 0; vk && i < w[1] + w[2]; i++)
            r_f32(f, (float *)&vk->w[i]);
        break;
    }
    case T4("mrkr"): {
        r_fs(f, v, 3);
        for (int k = 0; k < 3; k++)
            setf(w, k, v[k]);
        r_u32(f, &w[3]), r_u32(f, &w[4]);
        r_u32(f, &w[6]), r_u32(f, &w[7]), r_u32(f, &w[8]), r_u32(f, &w[9]);
        uint32_t n = w[7] * w[8];
        if (n > (16u << 20))
            break;
        uint8_t *img = malloc(n ? n : 1);
        if (img && r_raw(f, img, n))
            q3d_set_kid(nd, q3_schema_of(type), 0, 0, bytes_node(img, n));
        free(img);
        break;
    }
    case T4("mrkp"):
        r_fs(f, v, 3);
        for (int k = 0; k < 3; k++)
            setf(w, k, v[k]);
        r_u32(f, &w[3]), r_u32(f, &w[4]);
        read_pixmap(f, w + 5);
        break;
    /* Lights: their light data a child. */
    case T4("ambn"): case T4("drct"): case T4("pntl"): case T4("spot"):
        w[0] = 1;
        setf(w, 1, 1), setf(w, 2, 1), setf(w, 3, 1), setf(w, 4, 1);
        if (type == T4("drct")) {
            setf(w, 8, -1);
            if (!at_data_end(f)) {
                r_fs(f, v, 3);
                for (int k = 0; k < 3; k++)
                    setf(w, 6 + k, v[k]);
                r_u32(f, &w[5]);
            }
        } else if (type == T4("pntl")) {
            if (!at_data_end(f)) {
                r_fs(f, v, 3);
                for (int k = 0; k < 3; k++)
                    setf(w, 7 + k, v[k]);
                r_u32(f, &w[6]);
                r_u32(f, &w[5]);
            }
        } else if (type == T4("spot")) {
            setf(w, 12, -1);
            if (!at_data_end(f)) {
                r_fs(f, v, 6);
                for (int k = 0; k < 3; k++) {
                    setf(w, 7 + k, v[k]);
                    setf(w, 10 + k, v[3 + k]);
                }
                r_u32(f, &w[5]);
                r_u32(f, &w[6]);
                r_fs(f, v, 2);
                setf(w, 13, v[0]);
                setf(w, 14, v[1]);
                r_u32(f, &w[15]);
            }
        }
        break;
    /* Cameras: placement, range and view port children. */
    case T4("orth"):
        camera_defaults(w);
        setf(w, 15, -1), setf(w, 16, 1), setf(w, 17, 1), setf(w, 18, -1);
        if (r_fs(f, v, 4))
            for (int k = 0; k < 4; k++)
                setf(w, 15 + k, v[k]);
        break;
    case T4("vwpl"):
        camera_defaults(w);
        if (r_fs(f, v, 5))
            for (int k = 0; k < 5; k++)
                setf(w, 15 + k, v[k]);
        break;
    case T4("vana"):
        camera_defaults(w);
        setf(w, 15, 1), setf(w, 16, 1);
        if (r_fs(f, v, 2))
            setf(w, 15, v[0]), setf(w, 16, v[1]);
        break;
    /* Styles. */
    case T4("bckf"): case T4("intp"): case T4("fist"): case T4("pkid"): case T4("rcsh"): case T4("ofdr"):
    case T4("pkpt"):
        r_u32(f, &w[0]);
        break;
    case T4("sbdv"):
        r_u32(f, &w[0]);
        if (r_f32(f, &v[0]))
            setf(w, 1, v[0]);
        setf(w, 2, v[0]);
        if (!at_data_end(f) && r_f32(f, &v[1]))
            setf(w, 2, v[1]);
        break;
    case T4("anti"):
        r_u32(f, &w[0]);
        r_u32(f, &w[1]);
        if (r_f32(f, &v[0]))
            setf(w, 2, v[0]);
        break;
    case T4("fogg"):
        r_u32(f, &w[0]);
        r_u32(f, &w[1]);
        if (r_fs(f, v, 7))
            for (int k = 0; k < 7; k++)
                setf(w, 2 + k, v[k]);
        break;
    /* Transforms. */
    case T4("mtrx"):
        if (r_fs(f, v, 16))
            for (int k = 0; k < 16; k++)
                setf(w, k, v[k]);
        break;
    case T4("scal"): case T4("trns"):
        if (type == T4("scal"))
            setf(w, 0, 1), setf(w, 1, 1), setf(w, 2, 1);
        if (r_fs(f, v, 3))
            for (int k = 0; k < 3; k++)
                setf(w, k, v[k]);
        break;
    case T4("rott"): case T4("rtap"):
        r_u32(f, &w[0]);
        if (r_f32(f, &v[0]))
            setf(w, 1, v[0]);
        if (type == T4("rtap") && r_fs(f, v, 3))
            for (int k = 0; k < 3; k++)
                setf(w, 2 + k, v[k]);
        break;
    case T4("rtaa"):
        if (r_fs(f, v, 7))
            for (int k = 0; k < 7; k++)
                setf(w, k, v[k]);
        break;
    case T4("qtrn"):
        setf(w, 0, 1);
        if (r_fs(f, v, 4))
            for (int k = 0; k < 4; k++)
                setf(w, k, v[k]);
        break;
    /* Shaders: the identity uv transform; a texture shader's texture a
     * child. */
    case T4("phil"): case T4("lmil"): case T4("nuil"): case T4("txsu"):
        setf(w, 0, 1), setf(w, 4, 1), setf(w, 8, 1);
        break;
    case T4("txpm"):
        read_pixmap(f, w);
        break;
    case T4("txmm"): {
        uint32_t use = 0;
        r_u32(f, &use);
        w[1] = use;
        r_u32(f, &w[2]);
        r_u32(f, &w[3]);
        r_u32(f, &w[4]);
        r_u32(f, &w[6]), r_u32(f, &w[7]), r_u32(f, &w[8]), r_u32(f, &w[9]);
        uint32_t n = w[7] * w[8];
        if (n > (64u << 20))
            break;
        uint8_t *img = malloc(n ? n : 1);
        if (img && r_raw(f, img, n))
            w[0] = memory_storage(img, n);
        free(img);
        break;
    }
    case T4("vwhn"): case T4("push"): case T4("pop "): case T4("rset"):
        break;
    default:
        (void)u;
        break;
    }
    return it;
}

/* Where each geometry keeps its own attribute set (as q3_geom.c). */
static int set_word(uint32_t type)
{
    static const struct {
        uint32_t type;
        int off;
    } t[] = {
        { T4("box "), 13 }, { T4("cone"), 20 }, { T4("cyln"), 21 }, { T4("disk"), 13 }, { T4("elps"), 11 },
        { T4("elpd"), 18 }, { T4("gpgn"), 3 },  { T4("line"), 8 },  { T4("mrkr"), 10 }, { T4("nrbc"), 4 },
        { T4("nrbp"), 9 },  { T4("mrkp"), 13 }, { T4("pnt "), 3 },  { T4("plyg"), 2 },  { T4("plhd"), 6 },
        { T4("plyl"), 3 },  { T4("tors"), 19 }, { T4("trng"), 12 }, { T4("trig"), 4 },  { T4("tmsh"), 0 },
        { T4("high"), 0 },  { T4("vwhn"), 3 },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        if (t[i].type == type)
            return t[i].off;
    return -1;
}

static void hold(uint32_t *slot, gaddr g)
{
    gaddr old = *slot;
    *slot = g ? q3_ref(g) : 0;
    if (old)
        q3_release(old);
}

/* Vertex sets from a list onto the root's vertices, in order. */
static void vertex_sets(item *root, item *asl)
{
    uint32_t *slots[4096];
    uint32_t n = 0;
    q3node *nd = root->node;
    uint32_t type = root->type;
    const q3schema *s = q3_schema_of(type);
    if (root->kind == IT_MESH) {
        for (uint32_t i = 0; i < asl->asl_n && i < root->mesh->nverts; i++)
            if (asl->asl[i] > 1)
                hold(&root->mesh->vsets[i], asl->asl[i]);
        return;
    }
    if (!nd || !s)
        return;
    if (type == T4("trng") || type == T4("line")) {
        for (uint32_t i = 0; i < (type == T4("trng") ? 3u : 2u); i++)
            slots[n++] = &nd->w[i * 4 + 3];
    }
    q3node *vs = NULL;
    if (type == T4("plyg") || type == T4("plyl") || type == T4("trig") || type == T4("plhd"))
        vs = q3d_kid(nd, s, 0, 0);
    for (uint32_t i = 0; vs && i < vs->n && n < 4096; i++)
        slots[n++] = &vs->w[i * 4 + 3];
    if (type == T4("gpgn")) {
        q3node *cs = q3d_kid(nd, s, 0, 0);
        for (uint32_t c = 0; cs && c < cs->n; c++) {
            q3node *cv = q3d_kid(cs, s->f[0].elem, c, 0);
            for (uint32_t i = 0; cv && i < cv->n && n < 4096; i++)
                slots[n++] = &cv->w[i * 4 + 3];
        }
    }
    for (uint32_t i = 0; i < asl->asl_n && i < n; i++)
        if (asl->asl[i] > 1)
            hold(slots[i], asl->asl[i]);
    if (asl->asl_n > n && vs && vs->n > n) /* past the fixed slots */
        for (uint32_t i = n; i < asl->asl_n && i < vs->n; i++)
            if (asl->asl[i] > 1)
                hold(&vs->w[i * 4 + 3], asl->asl[i]);
}

static void face_sets(item *root, item *asl, bool geometry_list)
{
    uint32_t type = root->type;
    if (root->kind == IT_MESH) {
        uint32_t face = 0;
        for (uint32_t i = 0; i < root->mesh->nfaces && face < asl->asl_n; i++)
            if (!(root->mesh->fcount[i] & 0x80000000u)) {
                if (asl->asl[face] > 1)
                    hold(&root->mesh->fsets[i], asl->asl[face]);
                face++;
            }
        return;
    }
    q3node *nd = root->node;
    const q3schema *s = q3_schema_of(type);
    if (!nd || !s)
        return;
    q3node *arr = NULL;
    uint32_t stride = 1, off = 0, want = 0;
    if (type == T4("box ")) {
        arr = q3d_kid(nd, s, 0, 0);
        if (!arr)
            arr = kid(nd, type, 0, 6);
    } else if (type == T4("trig")) {
        want = nd->w[0] > 1 && nd->w[1] > 1 ? 2 * (nd->w[0] - 1) * (nd->w[1] - 1) : 0;
        arr = q3d_kid(nd, s, 0, 1);
        if (!arr && want)
            arr = kid(nd, type, 1, want);
    } else if (type == T4("plhd")) {
        arr = q3d_kid(nd, s, 0, 2);
        stride = 5, off = 4;
    } else if (type == T4("plyl") && geometry_list) {
        want = nd->w[0] ? nd->w[0] - 1 : 0;
        arr = q3d_kid(nd, s, 0, 1);
        if (!arr && want)
            arr = kid(nd, type, 1, want);
    }
    for (uint32_t i = 0; arr && i < asl->asl_n && i < arr->n; i++)
        if (asl->asl[i] > 1)
            hold(&arr->w[i * stride + off], asl->asl[i]);
}

/* A trimesh's attribute array, its data read now that the counts are known. */
static void read_array(q3file *f, item *root, item *a)
{
    q3node *nd = root->node;
    const q3schema *s = q3_schema_of(root->type);
    if (!nd || !s)
        return;
    int field = a->a_pos == 0 ? 2 : a->a_pos == 1 ? 4 : 6;
    uint32_t n = a->a_pos == 0 ? nd->w[1] : a->a_pos == 1 ? nd->w[5] : nd->w[9];
    q3node *arr = q3d_kid(nd, s, 0, field);
    if (!arr || a->a_index >= arr->n)
        return;
    int32_t type = (int32_t)a->w[0];
    int32_t size = q3_element_size(type);
    const q3schema *es = s->f[field].elem;
    uint8_t *use = NULL;
    if (a->w[1]) {
        use = malloc(n ? n : 1);
        for (uint32_t i = 0; use && i < n; i++) {
            uint64_t x = 0;
            r_u(f, 1, &x);
            use[i] = (uint8_t)x;
        }
    }
    if (size <= 0 || type == kQ3AttributeTypeSurfaceShader) {
        free(use);
        return; /* shaders and custom ones: objects follow, not kept */
    }
    uint8_t *data = malloc((size_t)n * (size_t)size + 1);
    for (uint32_t i = 0; data && i < n; i++)
        for (int32_t k = 0; k < size / 4; k++) {
            uint32_t x = 0;
            if (type == kQ3AttributeTypeHighlightState)
                r_u32(f, &x);
            else {
                float fl = 0;
                r_f32(f, &fl);
                memcpy(&x, &fl, 4);
            }
            uint8_t *b = data + (size_t)i * (size_t)size + 4 * (size_t)k;
            b[0] = (uint8_t)(x >> 24), b[1] = (uint8_t)(x >> 16), b[2] = (uint8_t)(x >> 8), b[3] = (uint8_t)x;
        }
    arr->w[a->a_index * 3] = (uint32_t)type;
    if (data)
        q3d_set_kid(arr, es, a->a_index, 0, bytes_node(data, n * (uint32_t)size));
    if (use)
        q3d_set_kid(arr, es, a->a_index, 1, bytes_node(use, n));
    free(data);
    free(use);
}

/* A container's child onto its root. */
static void attach(q3file *f, item *root, item *child)
{
    uint32_t rt = root->type;
    if (child->kind == IT_BUILD || child->kind == IT_MESH) { /* finished first */
        gaddr g = finish(child);
        child->kind = g ? IT_OBJ : IT_NONE;
        child->g = g;
    }
    if (root->kind == IT_ASL) { /* the sets of a list, in order */
        q3obj *c = child->kind == IT_OBJ ? q3_get(child->g) : NULL;
        for (uint32_t i = 0; c && q3_type_is(c->type, Q3_ATTRIBUTE_SET) && i < root->asl_n; i++)
            if (root->asl[i] == 1) {
                root->asl[i] = q3_ref(c->g);
                break;
            }
        return;
    }
    q3obj *ro = root->kind == IT_OBJ ? q3_get(root->g) : NULL;
    switch (child->kind) {
    case IT_ELEM:
        if (ro && q3_type_is(ro->type, Q3_SET))
            q3set_add_bytes(ro->data, child->etype, child->eb, child->esize);
        return;
    case IT_ASL:
        if (child->type == T4("vasl"))
            vertex_sets(root, child);
        else
            face_sets(root, child, child->type == T4("gasl"));
        return;
    case IT_ARRAY:
        if (rt == T4("tmsh"))
            read_array_at(f, root, child);
        return;
    case IT_WORDS: {
        uint32_t ct = child->type;
        q3node *nd = root->node;
        if (ct == T4("tcap") || ct == T4("bcap") || ct == T4("fcap") || ct == T4("icap")) {
            int off = -1;
            if (rt == T4("cone"))
                off = ct == T4("icap") ? 17 : ct == T4("fcap") ? 18 : ct == T4("bcap") ? 19 : -1;
            else if (rt == T4("cyln"))
                off = ct == T4("icap") ? 17 : ct == T4("tcap") ? 18 : ct == T4("fcap") ? 19 : 20;
            else if (rt == T4("elpd") && ct == T4("icap"))
                off = 17;
            else if (rt == T4("tors") && ct == T4("icap"))
                off = 18;
            if (nd && off >= 0)
                hold(&nd->w[off], child->w[0]);
            return;
        }
        if (ct == T4("gplh") && nd && rt == T4("gpgn"))
            nd->w[2] = child->w[0];
        else if (ct == T4("caps") && nd)
            nd->w[rt == T4("tors") ? 17 : 16] = child->w[0];
        else if (ct == T4("lght") && nd && q3_type_is(rt, Q3_LIGHT))
            memcpy(nd->w, child->w, 5 * 4);
        else if (nd && q3_type_is(rt, Q3_CAMERA) && (ct == T4("cmpl") || ct == T4("cmrg") || ct == T4("cmvp")))
            memcpy(nd->w + (ct == T4("cmpl") ? 0 : ct == T4("cmrg") ? 9 : 11), child->w, (size_t)child->nw * 4);
        else if (ct == T4("sduv") && nd && q3_type_is(rt, Q3_SHADER))
            memcpy(nd->w, child->w, 9 * 4);
        else if (ct == T4("shdr") && nd && q3_type_is(rt, Q3_SHADER))
            nd->w[9] = child->w[0], nd->w[10] = child->w[1];
        else if (ct == T4("dgst") && ro && q3_type_is(ro->type, Q3_DISPLAY_GROUP)) {
            /* The file's flags say what isn't so. */
            uint32_t x = child->w[0], st = 0;
            st |= x & 1 ? 0 : 1;   /* drawn */
            st |= x & 2 ? 2 : 0;   /* inline */
            st |= x & 4 ? 0 : 4;   /* bounding box */
            st |= x & 8 ? 0 : 8;   /* bounding sphere */
            st |= x & 16 ? 0 : 16; /* picked */
            st |= 32;
            ((q3group *)ro->data)->state = st;
        } else if (rt == T4("vwhn") && nd) {
            if (ct == T4("imdm"))
                nd->w[4] = 1, nd->w[5] = child->w[0], nd->w[6] = child->w[1];
            else if (ct == T4("imcc"))
                nd->w[13] = 1, memcpy(nd->w + 14, child->w, 16);
        }
        return;
    }
    case IT_OBJ: {
        q3obj *c = q3_get(child->g);
        if (!c)
            return;
        if (root->kind == IT_WORDS && (rt == T4("tcap") || rt == T4("bcap") || rt == T4("fcap") || rt == T4("icap"))) {
            if (q3_type_is(c->type, Q3_ATTRIBUTE_SET))
                hold(&root->w[0], c->g);
            return;
        }
        if (ro && q3_type_is(ro->type, Q3_SET) && q3_type_is(c->type, Q3_SURFACE_SHADER)) {
            uint8_t b[4] = { (uint8_t)(c->g >> 24), (uint8_t)(c->g >> 16), (uint8_t)(c->g >> 8), (uint8_t)c->g };
            q3set_add_bytes(ro->data, kQ3AttributeTypeSurfaceShader, b, 4);
            return;
        }
        if (ro && q3_type_is(ro->type, Q3_GROUP)) {
            q3_group_add(ro->g, c->g);
            return;
        }
        if (q3_type_is(c->type, Q3_ATTRIBUTE_SET)) {
            if (root->kind == IT_MESH)
                hold(&root->mesh->set, c->g);
            else if (root->node && set_word(rt) >= 0)
                hold(&root->node->w[set_word(rt)], c->g);
            else if (ro && q3_type_is(ro->type, Q3_SHAPE)) /* a shape's set */
                q3_assign(&ro->set, c->g);
            return;
        }
        if (root->node && rt == T4("txsu") && q3_type_is(c->type, Q3_TEXTURE)) {
            hold(&root->node->w[11], c->g);
            return;
        }
        if (root->node && rt == T4("vwhn")) {
            if (q3_type_is(c->type, Q3_RENDERER))
                hold(&root->node->w[0], c->g);
            else if (q3_type_is(c->type, Q3_CAMERA))
                hold(&root->node->w[1], c->g);
            else if (q3_type_is(c->type, Q3_LIGHT_GROUP))
                hold(&root->node->w[2], c->g);
        }
        return;
    }
    default: return;
    }
}

/* The finished object of an item (a new reference), or 0. */
static gaddr finish(item *it)
{
    gaddr g = 0;
    switch (it->kind) {
    case IT_OBJ:
        g = it->g;
        it->g = 0;
        break;
    case IT_BUILD: {
        q3obj *o = q3_new(it->type, it->node);
        if (o)
            it->node = NULL;
        g = o ? o->g : 0;
        break;
    }
    case IT_MESH: {
        meshb *m = it->mesh;
        if (m && m->nverts) {
            g = q3_mesh_build(m->nverts, (const float (*)[3])m->pts, m->vsets, m->nfaces, m->fcount, m->fidx,
                              m->fsets);
            if (g && m->set)
                q3_mesh_set_set(q3_get(g), m->set);
        }
        break;
    }
    case IT_ENDG: {
        q3obj *o = q3_new(Q3T('e', 'n', 'd', 'g'), NULL);
        g = o ? o->g : 0;
        break;
    }
    default: break;
    }
    free_item(it);
    return g;
}

/* ---------------------------------------------------------------------- */
/* Unknown objects */

#define UNKNOWN_TEXT Q3T('u', 'k', 't', 'x')
#define UNKNOWN_BINARY Q3T('u', 'k', 'b', 'n')

typedef struct {
    uint32_t type, byte_order;
    char *name;        /* text: the object's name; binary: a custom type's */
    uint8_t *contents;
    uint32_t size;
    bool dirty;
} unknown;

static void unknown_dispose(q3obj *o)
{
    unknown *u = o->data;
    if (u) {
        free(u->name);
        free(u->contents);
        free(u);
    }
}

static bool unknown_duplicate(q3obj *to, const q3obj *from)
{
    const unknown *s = from->data;
    unknown *u = calloc(1, sizeof *u);
    if (!u)
        return false;
    *u = *s;
    u->name = s->name ? strdup(s->name) : NULL;
    u->contents = malloc(s->size ? s->size : 1);
    if (u->contents && s->size)
        memcpy(u->contents, s->contents, s->size);
    to->data = u;
    return true;
}

static const char *custom_name(q3file *f, int32_t id)
{
    for (int i = 0; i < f->ntypes; i++)
        if (f->types[i].id == id)
            return f->types[i].name;
    return NULL;
}

static item unknown_item(q3file *f, uint32_t type, const char *name, uint32_t start, uint32_t end)
{
    item it;
    memset(&it, 0, sizeof it);
    unknown *u = calloc(1, sizeof *u);
    if (!u)
        return it;
    u->type = type;
    u->byte_order = f->swap ? 1 : 0;
    u->size = end > start ? end - start : 0;
    u->contents = malloc(u->size + 1);
    if (u->contents) {
        memcpy(u->contents, f->b + start, u->size);
        u->contents[u->size] = 0;
    }
    if (name)
        u->name = strdup(name);
    else if ((int32_t)type < 0 && custom_name(f, (int32_t)type))
        u->name = strdup(custom_name(f, (int32_t)type));
    q3obj *o = q3_new(f->text ? UNKNOWN_TEXT : UNKNOWN_BINARY, u);
    if (!o) {
        unknown_dispose(&(q3obj){ .data = u });
        return it;
    }
    it.kind = IT_OBJ;
    it.type = o->type;
    it.g = o->g;
    return it;
}

/* A class a program registered, read by its own read method. */
static gaddr custom_read(q3file *f, gaddr file, uint32_t type)
{
    q3class *k = q3_class(type);
    if (!k || !k->registered)
        return 0;
    for (q3class *c = k; c; c = c->parent ? q3_class(c->parent) : NULL) {
        if (!c->registered || !c->metahandler)
            continue;
        uint32_t m = Q3T('r', 'e', 'a', 'd');
        gaddr fn = ppc_call(c->metahandler, &m, 1);
        if (fn) {
            uint32_t arg = file;
            return ppc_call(fn, &arg, 1);
        }
    }
    (void)f;
    return 0;
}

static gaddr current_file; /* the file object being read, for custom read methods */

/* ---------------------------------------------------------------------- */
/* Reading: one object */

static gaddr toc_object(q3file *f, uint32_t refid);
static item group_body(q3file *f, item grp);

static item read_binary(q3file *f)
{
    item none;
    memset(&none, 0, sizeof none);
    uint32_t lim = limit_of(f);
    if (f->pos + 8 > lim)
        return none;
    uint64_t t64 = 0, s64 = 0;
    bin_u(f, 4, &t64);
    bin_u(f, 4, &s64);
    uint32_t type = (uint32_t)t64, start = f->pos;
    uint32_t end = start + (uint32_t)s64;
    if (end > lim || end < start)
        end = lim;
    item it;
    memset(&it, 0, sizeof it);
    switch (type) {
    case T4("cntr"): {
        push(f, FR_CONTAINER, end);
        it = read_binary(f);
        while (f->pos + 8 <= end) {
            uint32_t at = f->pos;
            item ch = read_binary(f);
            attach(f, &it, &ch);
            free_item(&ch);
            if (f->pos <= at)
                break;
        }
        pop(f);
        f->pos = end;
        return it;
    }
    case T4("bgng"): {
        push(f, FR_OBJECT, end);
        item grp = read_binary(f);
        pop(f);
        f->pos = end;
        return group_body(f, grp);
    }
    case T4("endg"):
        f->pos = end;
        it.kind = IT_ENDG;
        it.type = type;
        return it;
    case T4("rfrn"): {
        uint64_t id = 0;
        push(f, FR_OBJECT, end);
        bin_u(f, 4, &id);
        pop(f);
        f->pos = end;
        it.kind = IT_OBJ;
        it.g = toc_object(f, (uint32_t)id);
        return it;
    }
    case T4("type"): {
        push(f, FR_OBJECT, end);
        uint64_t id = 0;
        bin_u(f, 4, &id);
        char name[128];
        if (r_str(f, name, sizeof name, NULL)) {
            typeent *n = realloc(f->types, (size_t)(f->ntypes + 1) * sizeof *n);
            if (n) {
                f->types = n;
                f->types[f->ntypes].id = (int32_t)id;
                snprintf(f->types[f->ntypes].name, sizeof f->types[0].name, "%s", name);
                f->ntypes++;
            }
        }
        pop(f);
        f->pos = end;
        return read_binary(f);
    }
    case T4("toc "):
        f->pos = end;
        return read_binary(f);
    default: break;
    }
    /* A custom type by its name. */
    uint32_t known = type;
    if ((int32_t)type < 0 && custom_name(f, (int32_t)type)) {
        q3class *k = q3_class_named(custom_name(f, (int32_t)type));
        known = k ? k->type : 0;
    }
    push(f, FR_OBJECT, end);
    q3class *k = known ? q3_class(known) : NULL;
    if (k && k->registered) {
        it.kind = IT_OBJ;
        it.type = known;
        it.g = custom_read(f, current_file, known);
    } else {
        it = parse(f, known);
    }
    pop(f);
    if (it.kind == IT_ARRAY) { /* its data stays to be read on attaching */
        it.w[2] = f->pos;
        it.w[3] = end;
        f->pos = end;
        return it;
    }
    if (it.kind == IT_NONE)
        it = unknown_item(f, type, NULL, start, end);
    f->pos = end;
    return it;
}

static item read_text(q3file *f)
{
    item none;
    memset(&none, 0, sizeof none);
    char t[256];
    if (at_data_end(f))
        return none;
    int k = token(f, t, sizeof t, true);
    if (k == TK_WORD && t[0] && t[strlen(t) - 1] == ':') /* a label */
        k = token(f, t, sizeof t, true);
    if (k != TK_WORD)
        return none;
    char name[256];
    snprintf(name, sizeof name, "%s", t);
    uint32_t type = type_of_name(name);
    if (token(f, t, sizeof t, true) != TK_LP)
        return none;
    int depth = f->depth;
    uint32_t start = f->pos;
    item it;
    memset(&it, 0, sizeof it);
    switch (type) {
    case T4("cntr"):
        push(f, FR_CONTAINER, 0);
        it = read_text(f);
        while (!at_data_end(f)) {
            uint32_t at = f->pos;
            item ch = read_text(f);
            attach(f, &it, &ch);
            free_item(&ch);
            if (f->pos <= at)
                break;
        }
        pop(f);
        close_paren(f, depth);
        return it;
    case T4("bgng"): {
        push(f, FR_OBJECT, 0);
        item grp = read_text(f);
        pop(f);
        close_paren(f, depth);
        return group_body(f, grp);
    }
    case T4("endg"):
        close_paren(f, depth);
        it.kind = IT_ENDG;
        it.type = type;
        return it;
    case T4("rfrn"): {
        uint32_t id = 0;
        push(f, FR_OBJECT, 0);
        r_u32(f, &id);
        pop(f);
        close_paren(f, depth);
        it.kind = IT_OBJ;
        it.g = toc_object(f, id);
        return it;
    }
    case T4("toc "): case T4("type"):
        close_paren(f, depth);
        return read_text(f);
    default: break;
    }
    push(f, FR_OBJECT, 0);
    q3class *kc = type ? q3_class(type) : NULL;
    if (kc && kc->registered) {
        it.kind = IT_OBJ;
        it.type = type;
        it.g = custom_read(f, current_file, type);
    } else {
        it = parse(f, type);
    }
    pop(f);
    if (it.kind == IT_ARRAY) { /* the data follows within the parentheses */
        it.w[2] = f->pos;
        it.w[3] = (uint32_t)depth;
        close_paren(f, depth);
        return it;
    }
    if (it.kind == IT_NONE) {
        uint32_t s = start;
        close_paren(f, depth);
        uint32_t e = f->pos > s ? f->pos - 1 : s; /* before the closing parenthesis */
        it = unknown_item(f, 0, name, s, e);
        return it;
    }
    close_paren(f, depth);
    return it;
}

static item read_item(q3file *f) { return f->text ? read_text(f) : read_binary(f); }

/* An attribute array's data, read when its trimesh takes it: the place
 * kept is put back for the while. */
static void read_array_at(q3file *f, item *root, item *a)
{
    uint32_t pos = f->pos;
    int depth = f->depth;
    f->pos = a->w[2];
    if (f->text) {
        f->depth = (int)a->w[3];
        push(f, FR_OBJECT, 0);
    } else {
        push(f, FR_OBJECT, a->w[3]);
    }
    read_array(f, root, a);
    pop(f);
    f->pos = pos;
    f->depth = depth;
}

/* What follows a begin group, up to its end group, into the group. */
static item group_body(q3file *f, item grp)
{
    if (f->read_group & kQ3FileReadObjectsInGroup) {
        f->in_group++;
        return grp;
    }
    /* Its state may come next. */
    if (!f->text && f->pos + 8 <= limit_of(f) && f->b[f->pos] == 'd' && f->b[f->pos + 1] == 'g') {
        item st = read_item(f);
        attach(f, &grp, &st);
        free_item(&st);
    }
    for (;;) {
        if (f->text ? at_data_end(f) : f->pos + 8 > limit_of(f))
            break;
        uint32_t at = f->pos;
        item ch = read_item(f);
        if (ch.kind == IT_ENDG) {
            free_item(&ch);
            break;
        }
        attach(f, &grp, &ch);
        free_item(&ch);
        if (f->pos <= at)
            break;
    }
    return grp;
}

static gaddr toc_object(q3file *f, uint32_t refid)
{
    for (int i = 0; i < f->ntoc; i++) {
        tocent *e = &f->toc[i];
        if (e->refid != refid)
            continue;
        if (e->obj)
            return q3_ref(e->obj);
        if (!e->pos)
            return 0;
        uint32_t pos = f->pos;
        int depth = f->depth, nfr = f->nfr;
        f->pos = e->pos;
        f->nfr = 0;
        item it = read_item(f);
        gaddr g = finish(&it);
        f->pos = pos;
        f->depth = depth;
        f->nfr = nfr;
        e->obj = g ? q3_ref(g) : 0;
        return g;
    }
    q3_warning(kQ3WarningUnresolvableReference);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Opening */

static bool read_toc_binary(q3file *f, uint32_t at)
{
    for (int guard = 0; at && guard < 16; guard++) {
        if (at + 36 > f->n)
            return false;
        f->pos = at;
        uint64_t type = 0, size = 0, next = 0, x = 0, etype = 0, esize = 0, nent = 0;
        bin_u(f, 4, &type);
        bin_u(f, 4, &size);
        if (type != T4("toc "))
            return false;
        bin_u(f, 8, &next);
        bin_u(f, 4, &x);
        bin_u(f, 4, &x);
        bin_u(f, 4, &etype);
        bin_u(f, 4, &esize);
        bin_u(f, 4, &nent);
        if (nent > (1u << 20))
            return false;
        for (uint64_t i = 0; i < nent; i++) {
            uint64_t id = 0, loc = 0, ot = 0;
            if (!bin_u(f, 4, &id) || !bin_u(f, 8, &loc))
                break;
            if (etype == 1)
                bin_u(f, 4, &ot);
            tocent *n = realloc(f->toc, (size_t)(f->ntoc + 1) * sizeof *n);
            if (!n)
                break;
            f->toc = n;
            memset(&f->toc[f->ntoc], 0, sizeof f->toc[0]);
            f->toc[f->ntoc].refid = (uint32_t)id;
            f->toc[f->ntoc].pos = (uint32_t)loc;
            f->ntoc++;
        }
        /* A table of contents at the end isn't an object to read. */
        if (at + 8 + size >= f->n && at < f->eof)
            f->eof = at;
        at = (uint32_t)next;
    }
    return true;
}

/* Text: the labels where objects are, and the table of contents' refIDs
 * for them. */
static void scan_text_labels(q3file *f)
{
    char t[256];
    uint32_t save = f->pos;
    int depth = f->depth;
    struct {
        char label[64];
        uint32_t pos;
    } *labels = NULL;
    int nl = 0;
    while (f->pos < f->eof) {
        int k = token(f, t, sizeof t, true);
        if (k == TK_END)
            break;
        size_t len = strlen(t);
        if (k == TK_WORD && len > 1 && t[len - 1] == ':') {
            void *n = realloc(labels, (size_t)(nl + 1) * sizeof *labels);
            if (!n)
                break;
            labels = n;
            t[len - 1] = 0;
            snprintf(labels[nl].label, sizeof labels[0].label, "%s", t);
            labels[nl].pos = f->pos;
            nl++;
        } else if (k == TK_WORD && !strcasecmp(t, "TableOfContents")) {
            /* refID then label> pairs. */
            uint32_t last = 0;
            bool have = false;
            int d0 = f->depth;
            token(f, t, sizeof t, true);
            while (f->depth > d0 - 0 && f->pos < f->eof) {
                k = token(f, t, sizeof t, true);
                if (k == TK_RP && f->depth <= d0)
                    break;
                len = strlen(t);
                if (k == TK_NUM) {
                    last = (uint32_t)strtoul(t, NULL, 10);
                    have = true;
                } else if (k == TK_WORD && len > 1 && t[len - 1] == '>' && have) {
                    tocent *n = realloc(f->toc, (size_t)(f->ntoc + 1) * sizeof *n);
                    if (!n)
                        break;
                    f->toc = n;
                    memset(&f->toc[f->ntoc], 0, sizeof f->toc[0]);
                    f->toc[f->ntoc].refid = last;
                    t[len - 1] = 0;
                    snprintf(f->toc[f->ntoc].label, sizeof f->toc[0].label, "%s", t);
                    f->ntoc++;
                    have = false;
                }
            }
        }
    }
    for (int i = 0; i < f->ntoc; i++)
        for (int j = 0; j < nl; j++)
            if (!strcmp(f->toc[i].label, labels[j].label))
                f->toc[i].pos = labels[j].pos;
    free(labels);
    f->pos = save;
    f->depth = depth;
}

static bool open_bytes(q3file *f, uint8_t *b, uint32_t n)
{
    f->b = b;
    f->n = f->eof = n;
    f->pos = 0;
    f->depth = 0;
    f->nfr = 0;
    if (n >= 24 && (!memcmp(b, "3DMF", 4) || !memcmp(b, "FMD3", 4))) {
        f->text = false;
        f->swap = !memcmp(b, "FMD3", 4);
        f->pos = 8;
        uint64_t ver = 0, mode = 0, toc = 0;
        bin_u(f, 4, &ver);
        bin_u(f, 4, &mode);
        bin_u(f, 8, &toc);
        f->version = (uint32_t)ver;
        f->mode = (uint32_t)mode & 3;
        if (toc && toc < n)
            read_toc_binary(f, (uint32_t)toc);
        f->pos = 24;
        return true;
    }
    /* Text: "3DMetafile ( major minor mode label> )". */
    f->text = true;
    char t[256];
    if (token(f, t, sizeof t, true) != TK_WORD || strcasecmp(t, "3DMetafile") ||
        token(f, t, sizeof t, true) != TK_LP)
        return false;
    uint32_t major = 1, minor = 0, mode = 0;
    push(f, FR_OBJECT, 0);
    r_u32(f, &major);
    r_u32(f, &minor);
    r_u32(f, &mode);
    pop(f);
    close_paren(f, 1);
    f->version = major << 16 | (minor & 0xFFFF);
    f->mode = (mode & 3) | kQ3FileModeText;
    scan_text_labels(f);
    return true;
}

static bool at_file_end(q3file *f)
{
    if (f->text) {
        skip_space(f);
        if (f->pos >= f->eof)
            return true;
        /* Only a table of contents left. */
        uint32_t save = f->pos;
        int depth = f->depth;
        char t[64];
        int k = token(f, t, sizeof t, true);
        if (k == TK_WORD && t[strlen(t) - 1] == ':')
            k = token(f, t, sizeof t, true);
        bool toc = k == TK_WORD && !strcasecmp(t, "TableOfContents");
        if (toc) {
            token(f, t, sizeof t, true);
            close_paren(f, f->depth);
            skip_space(f);
            toc = f->pos >= f->eof;
        }
        f->pos = save;
        f->depth = depth;
        return toc;
    }
    return f->pos + 8 > f->eof;
}

/* ---------------------------------------------------------------------- */
/* Writing */

static bool out(q3file *f, const void *p, uint32_t n)
{
    if (f->olen + n > f->ocap) {
        uint32_t cap = f->ocap ? f->ocap : 4096;
        while (cap < f->olen + n)
            cap *= 2;
        uint8_t *nb = realloc(f->o, cap);
        if (!nb)
            return false;
        f->o = nb;
        f->ocap = cap;
    }
    memcpy(f->o + f->olen, p, n);
    f->olen += n;
    return true;
}

static void text_out(q3file *f, const char *s)
{
    if (f->line_start) {
        for (int i = 0; i < f->tdepth; i++)
            out(f, "\t", 1);
        f->line_start = false;
    } else if (f->olen && f->o[f->olen - 1] != '(' && f->o[f->olen - 1] != '\t') {
        out(f, " ", 1);
    }
    out(f, s, (uint32_t)strlen(s));
}

static void newline(q3file *f)
{
    out(f, "\n", 1);
    f->line_start = true;
}

static void w_u(q3file *f, int bytes, uint64_t v, bool is_signed)
{
    if (f->text) {
        char b[32];
        if (is_signed)
            snprintf(b, sizeof b, "%lld", (long long)(bytes == 1 ? (int8_t)v : bytes == 2 ? (int16_t)v : bytes == 4 ? (int32_t)v : (int64_t)v));
        else
            snprintf(b, sizeof b, "%llu", (unsigned long long)v);
        text_out(f, b);
        return;
    }
    uint8_t b[8];
    for (int i = 0; i < bytes; i++)
        b[i] = (uint8_t)(v >> (8 * (bytes - 1 - i)));
    out(f, b, (uint32_t)bytes);
}

static void w_u32(q3file *f, uint32_t v) { w_u(f, 4, v, false); }

static void w_f32(q3file *f, float v)
{
    if (f->text) {
        char b[32];
        snprintf(b, sizeof b, "%.9g", (double)v);
        text_out(f, b);
        return;
    }
    uint32_t u;
    memcpy(&u, &v, 4);
    w_u(f, 4, u, false);
}

static void w_wordf(q3file *f, const uint32_t *w, int n)
{
    for (int i = 0; i < n; i++)
        w_f32(f, q3d_f(w, i));
}

static void w_flag(q3file *f, uint32_t v, const char *const *namesv, uint32_t count)
{
    if (f->text && namesv && v < count) {
        text_out(f, namesv[v]);
        return;
    }
    w_u32(f, v);
}

static const char *const booleans[] = { "False", "True" };

static void pad4(q3file *f)
{
    static const uint8_t zero[4] = { 0 };
    while (!f->text && (f->olen & 3))
        out(f, zero, 1);
}

static void w_str(q3file *f, const char *s)
{
    if (f->text) {
        out(f, " \"", f->line_start ? 1 : 2);
        f->line_start = false;
        for (; *s; s++) {
            if (*s == '"' || *s == '\\')
                out(f, "\\", 1);
            out(f, s, 1);
        }
        out(f, "\"", 1);
        return;
    }
    out(f, s, (uint32_t)strlen(s) + 1);
    pad4(f);
}

static void w_raw(q3file *f, const uint8_t *b, uint32_t n)
{
    if (f->text) {
        for (uint32_t i = 0; i < n; i += 32) {
            char line[80] = "0x";
            for (uint32_t j = i; j < n && j < i + 32; j++)
                snprintf(line + 2 + 2 * (j - i), 3, "%02X", b[j]);
            text_out(f, line);
            if (i + 32 < n)
                newline(f);
        }
        return;
    }
    out(f, b, n);
    pad4(f);
}

static void begin_obj(q3file *f, uint32_t type)
{
    if (f->text) {
        const char *n = name_of_type(type);
        char b[64];
        if (!n) {
            snprintf(b, sizeof b, "Unknown%08X", type);
            n = b;
        }
        if (!f->line_start)
            newline(f);
        text_out(f, n);
        text_out(f, "(");
        f->tdepth++;
        return;
    }
    w_u32(f, type);
    if (f->nw < 64)
        f->wsizes[f->nw] = f->olen;
    f->nw++;
    w_u32(f, 0);
}

static void end_obj(q3file *f)
{
    if (f->text) {
        f->tdepth--;
        text_out(f, ")");
        newline(f);
        return;
    }
    pad4(f);
    if (--f->nw < 64 && f->nw >= 0) {
        uint32_t at = f->wsizes[f->nw], size = f->olen - at - 4;
        f->o[at] = (uint8_t)(size >> 24), f->o[at + 1] = (uint8_t)(size >> 16);
        f->o[at + 2] = (uint8_t)(size >> 8), f->o[at + 3] = (uint8_t)size;
    }
}

static void write_obj(q3file *f, gaddr g);

static void write_set_elements(q3file *f, const q3set *s)
{
    static const struct {
        int32_t etype;
        uint32_t code;
    } map[] = {
        { kQ3AttributeTypeSurfaceUV, T4("sruv") }, { kQ3AttributeTypeShadingUV, T4("shuv") },
        { kQ3AttributeTypeNormal, T4("nrml") },     { kQ3AttributeTypeAmbientCoefficient, T4("camb") },
        { kQ3AttributeTypeDiffuseColor, T4("kdif") }, { kQ3AttributeTypeSpecularColor, T4("kspc") },
        { kQ3AttributeTypeSpecularControl, T4("cspc") }, { kQ3AttributeTypeTransparencyColor, T4("kxpr") },
        { kQ3AttributeTypeSurfaceTangent, T4("srtn") }, { kQ3AttributeTypeHighlightState, T4("hlst") },
    };
    for (int i = 0; s && i < s->n; i++) {
        const q3elem *e = &s->e[i];
        if (e->type == kQ3AttributeTypeSurfaceShader && e->size >= 4) {
            write_obj(f, (uint32_t)e->bytes[0] << 24 | (uint32_t)e->bytes[1] << 16 | (uint32_t)e->bytes[2] << 8 |
                             e->bytes[3]);
            continue;
        }
        for (unsigned m = 0; m < sizeof map / sizeof map[0]; m++)
            if (map[m].etype == e->type) {
                begin_obj(f, map[m].code);
                for (uint32_t k = 0; k + 4 <= e->size; k += 4) {
                    uint32_t u = (uint32_t)e->bytes[k] << 24 | (uint32_t)e->bytes[k + 1] << 16 |
                                 (uint32_t)e->bytes[k + 2] << 8 | e->bytes[k + 3];
                    if (e->type == kQ3AttributeTypeHighlightState)
                        w_flag(f, u, booleans, 2);
                    else {
                        float fl;
                        memcpy(&fl, &u, 4);
                        w_f32(f, fl);
                    }
                }
                end_obj(f);
            }
    }
}

/* An attribute set written whole (container and all). */
static void write_set(q3file *f, gaddr g)
{
    q3obj *o = q3_get(g);
    q3set *s = o ? o->data : NULL;
    if (!s)
        return;
    if (!s->n) {
        begin_obj(f, o->type == Q3_ATTRIBUTE_SET ? T4("attr") : T4("set "));
        end_obj(f);
        return;
    }
    begin_obj(f, T4("cntr"));
    begin_obj(f, o->type == Q3_ATTRIBUTE_SET ? T4("attr") : T4("set "));
    end_obj(f);
    write_set_elements(f, s);
    end_obj(f);
}

/* An attribute set list of these sets (0 for none), if any is there. */
static void write_asl(q3file *f, uint32_t code, const gaddr *sets, uint32_t n)
{
    uint32_t have = 0;
    for (uint32_t i = 0; i < n; i++)
        have += sets[i] != 0;
    if (!have)
        return;
    begin_obj(f, T4("cntr"));
    begin_obj(f, code);
    w_u32(f, n);
    static const char *const packs[] = { "Include", "Exclude" };
    if (have == n) {
        w_flag(f, 1, packs, 2);
        w_u32(f, 0);
    } else {
        w_flag(f, 0, packs, 2);
        w_u32(f, have);
        int bytes = n >= 0x10000 ? 4 : n >= 0x100 ? 2 : 1;
        for (uint32_t i = 0; i < n; i++)
            if (sets[i])
                w_u(f, bytes, i, false);
    }
    end_obj(f);
    for (uint32_t i = 0; i < n; i++)
        if (sets[i])
            write_set(f, sets[i]);
    end_obj(f);
}

static void w_index(q3file *f, uint32_t count, uint32_t v)
{
    int bytes = count >= 0x10000 ? 4 : count >= 0x100 ? 2 : 1;
    if (v == 0xFFFFFFFF)
        v = bytes == 4 ? v : bytes == 2 ? 0xFFFF : 0xFF;
    w_u(f, bytes, v, false);
}

/* Sets in a vertex array (4 words a vertex). */
static gaddr *vsets_of(const q3node *vs)
{
    if (!vs || !vs->n)
        return NULL;
    gaddr *s = calloc(vs->n, sizeof *s);
    for (uint32_t i = 0; s && i < vs->n; i++)
        s[i] = vs->w[i * 4 + 3];
    return s;
}

static void write_points(q3file *f, const q3node *vs, uint32_t stride)
{
    for (uint32_t i = 0; vs && i < vs->n; i++)
        w_wordf(f, vs->w + i * stride, 3);
}

static void write_pixmap(q3file *f, const uint32_t *w)
{
    for (int k = 1; k < 8; k++)
        w_u32(f, w[k]);
    uint32_t size = 0;
    uint8_t *b = q3_storage_bytes(w[0], &size);
    uint32_t n = w[2] * w[3];
    uint8_t *img = calloc(n + 1, 1);
    if (img && b)
        memcpy(img, b, size < n ? size : n);
    w_raw(f, img ? img : (uint8_t *)"", img ? n : 0);
    free(img);
    free(b);
}

/* A geometry or other data object: its data, then its children in a
 * container when it has any. */
static void write_data(q3file *f, q3obj *o)
{
    q3node *d = o->data;
    const q3schema *s = q3_schema_of(o->type);
    uint32_t t = o->type, *w = d->w;
    /* Whether a container is needed: sets, lists, children. */
    int sw = set_word(t);
    bool kids = (sw >= 0 && w[sw]) || o->set;
    gaddr *vsets = NULL, *fsets = NULL, *gsets = NULL;
    uint32_t nv = 0, nf = 0, ng = 0;
    q3node *vs = NULL;
    if (t == T4("trng") || t == T4("line")) {
        nv = t == T4("trng") ? 3 : 2;
        vsets = calloc(nv, sizeof *vsets);
        for (uint32_t i = 0; vsets && i < nv; i++)
            vsets[i] = w[i * 4 + 3];
    } else if (t == T4("plyg") || t == T4("plyl") || t == T4("trig") || t == T4("plhd")) {
        vs = q3d_kid(d, s, 0, 0);
        vsets = vsets_of(vs);
        nv = vs ? vs->n : 0;
    } else if (t == T4("gpgn")) {
        q3node *cs = q3d_kid(d, s, 0, 0);
        for (uint32_t c = 0; cs && c < cs->n; c++) {
            q3node *cv = q3d_kid(cs, s->f[0].elem, c, 0);
            for (uint32_t i = 0; cv && i < cv->n; i++) {
                gaddr *n = realloc(vsets, (nv + 1) * sizeof *n);
                if (!n)
                    break;
                vsets = n;
                vsets[nv++] = cv->w[i * 4 + 3];
            }
        }
    }
    if (t == T4("box ") || t == T4("trig") || t == T4("plhd")) {
        q3node *fa = q3d_kid(d, s, 0, t == T4("box ") ? 0 : t == T4("trig") ? 1 : 2);
        nf = fa ? fa->n : 0;
        fsets = nf ? calloc(nf, sizeof *fsets) : NULL;
        for (uint32_t i = 0; fsets && i < nf; i++)
            fsets[i] = t == T4("plhd") ? fa->w[i * 5 + 4] : fa->w[i];
    }
    if (t == T4("plyl")) {
        q3node *sa = q3d_kid(d, s, 0, 1);
        ng = sa ? sa->n : 0;
        gsets = ng ? calloc(ng, sizeof *gsets) : NULL;
        for (uint32_t i = 0; gsets && i < ng; i++)
            gsets[i] = sa->w[i];
    }
    for (uint32_t i = 0; vsets && i < nv; i++)
        kids |= vsets[i] != 0;
    for (uint32_t i = 0; fsets && i < nf; i++)
        kids |= fsets[i] != 0;
    for (uint32_t i = 0; gsets && i < ng; i++)
        kids |= gsets[i] != 0;
    bool trimesh_arrays = t == T4("tmsh") && (w[3] || w[7] || w[11]);
    bool camera = q3_type_is(t, Q3_CAMERA), light = q3_type_is(t, Q3_LIGHT);
    bool shader = q3_type_is(t, Q3_SHADER), caps = false, capsets = false;
    if (t == T4("cone") || t == T4("cyln") || t == T4("elpd") || t == T4("tors")) {
        caps = w[t == T4("tors") ? 17 : 16] != 0;
        for (int k = 17; k < (t == T4("cyln") ? 21 : t == T4("cone") ? 20 : t == T4("tors") ? 19 : 18); k++)
            capsets |= w[k] != 0;
    }
    if (trimesh_arrays || camera || light || caps || capsets || (t == T4("txsu") && w[11]) ||
        (t == T4("gpgn") && w[2]) || t == T4("vwhn") || t == T4("high"))
        kids = true;
    if (kids)
        begin_obj(f, T4("cntr"));
    begin_obj(f, t);
    static const char *const pixel_types[] = { "RGB32", "ARGB32", "RGB16", "ARGB16", "RGB16_565", "RGB24" };
    switch (t) {
    case T4("box "): case T4("cone"): case T4("cyln"): case T4("elpd"): case T4("tors"):
        w_wordf(f, w + 3, 3);
        w_wordf(f, w + 6, 3);
        w_wordf(f, w + 9, 3);
        w_wordf(f, w, 3);
        if (t == T4("tors"))
            w_wordf(f, w + 12, 5);
        else if (t != T4("box "))
            w_wordf(f, w + 12, 4);
        break;
    case T4("disk"): case T4("elps"):
        w_wordf(f, w + 3, 3);
        w_wordf(f, w + 6, 3);
        w_wordf(f, w, 3);
        w_wordf(f, w + 9, t == T4("disk") ? 4 : 2);
        break;
    case T4("gpgn"): {
        q3node *cs = q3d_kid(d, s, 0, 0);
        w_u32(f, cs ? cs->n : 0);
        for (uint32_t c = 0; cs && c < cs->n; c++) {
            q3node *cv = q3d_kid(cs, s->f[0].elem, c, 0);
            w_u32(f, cv ? cv->n : 0);
            write_points(f, cv, 4);
        }
        break;
    }
    case T4("line"): w_wordf(f, w, 3), w_wordf(f, w + 4, 3); break;
    case T4("trng"): w_wordf(f, w, 3), w_wordf(f, w + 4, 3), w_wordf(f, w + 8, 3); break;
    case T4("pnt "): w_wordf(f, w, 3); break;
    case T4("plyg"): case T4("plyl"): w_u32(f, vs ? vs->n : 0), write_points(f, vs, 4); break;
    case T4("trig"): w_u32(f, w[0]), w_u32(f, w[1]), write_points(f, vs, 4); break;
    case T4("plhd"): {
        w_u32(f, w[0]);
        write_points(f, vs, 4);
        q3node *ts = q3d_kid(d, s, 0, 2), *es = q3d_kid(d, s, 0, 1);
        w_u32(f, ts ? ts->n : 0);
        for (uint32_t i = 0; ts && i < ts->n; i++) {
            for (int k = 0; k < 3; k++)
                w_index(f, w[0], ts->w[i * 5 + (uint32_t)k]);
            w_u32(f, ts->w[i * 5 + 3]);
        }
        w_u32(f, es ? es->n : 0);
        for (uint32_t i = 0; es && i < es->n; i++) {
            w_index(f, w[0], es->w[i * 5]);
            w_index(f, w[0], es->w[i * 5 + 1]);
            w_index(f, w[4], es->w[i * 5 + 2]);
            w_index(f, w[4], es->w[i * 5 + 3]);
        }
        break;
    }
    case T4("tmsh"): {
        q3node *tris = q3d_kid(d, s, 0, 1), *edges = q3d_kid(d, s, 0, 3), *pts = q3d_kid(d, s, 0, 5);
        uint32_t np = pts ? pts->n : 0;
        w_u32(f, tris ? tris->n : 0), w_u32(f, w[3]), w_u32(f, edges ? edges->n : 0), w_u32(f, w[7]);
        w_u32(f, np), w_u32(f, w[11]);
        for (uint32_t i = 0; tris && i < tris->n * 3; i++)
            w_index(f, np, tris->w[i]);
        for (uint32_t i = 0; edges && i < edges->n * 4; i++)
            w_index(f, np, edges->w[i]);
        write_points(f, pts, 3);
        w_wordf(f, w + 13, 6);
        w_flag(f, w[19], booleans, 2);
        break;
    }
    case T4("nrbc"): {
        q3node *cp = q3d_kid(d, s, 0, 0), *kn = q3d_kid(d, s, 0, 1);
        w_u32(f, w[0]), w_u32(f, w[1]);
        if (cp)
            w_wordf(f, cp->w, (int)(cp->n * 4));
        if (kn)
            w_wordf(f, kn->w, (int)kn->n);
        break;
    }
    case T4("nrbp"): {
        for (int k = 0; k < 4; k++)
            w_u32(f, w[k]);
        for (int fl = 0; fl < 3; fl++) {
            q3node *a = q3d_kid(d, s, 0, fl);
            if (a)
                w_wordf(f, a->w, (int)(a->n * a->ew));
        }
        break;
    }
    case T4("mrkr"): {
        w_wordf(f, w, 3);
        w_u(f, 4, w[3], true), w_u(f, 4, w[4], true);
        w_u32(f, w[6]), w_u32(f, w[7]), w_u32(f, w[8]), w_u32(f, w[9]);
        q3node *img = q3d_kid(d, s, 0, 0);
        w_raw(f, img ? img->bytes : (const uint8_t *)"", img ? img->nbytes : 0);
        break;
    }
    case T4("mrkp"):
        w_wordf(f, w, 3);
        w_u(f, 4, w[3], true), w_u(f, 4, w[4], true);
        write_pixmap(f, w + 5);
        break;
    case T4("drct"): w_wordf(f, w + 6, 3), w_flag(f, w[5], booleans, 2); break;
    case T4("pntl"): w_wordf(f, w + 7, 3), w_u32(f, w[6]), w_flag(f, w[5], booleans, 2); break;
    case T4("spot"):
        w_wordf(f, w + 7, 3), w_wordf(f, w + 10, 3), w_flag(f, w[5], booleans, 2), w_u32(f, w[6]);
        w_wordf(f, w + 13, 2), w_u32(f, w[15]);
        break;
    case T4("orth"): w_wordf(f, w + 15, 4); break;
    case T4("vwpl"): w_wordf(f, w + 15, 5); break;
    case T4("vana"): w_wordf(f, w + 15, 2); break;
    case T4("bckf"): case T4("intp"): case T4("fist"): case T4("pkid"): case T4("rcsh"): case T4("ofdr"):
    case T4("pkpt"):
        w_u32(f, w[0]);
        break;
    case T4("sbdv"): w_u32(f, w[0]), w_wordf(f, w + 1, 2); break;
    case T4("anti"): w_u32(f, w[0]), w_u32(f, w[1]), w_wordf(f, w + 2, 1); break;
    case T4("fogg"): w_u32(f, w[0]), w_u32(f, w[1]), w_wordf(f, w + 2, 7); break;
    case T4("mtrx"): w_wordf(f, w, 16); break;
    case T4("scal"): case T4("trns"): w_wordf(f, w, 3); break;
    case T4("rott"): w_u32(f, w[0]), w_wordf(f, w + 1, 1); break;
    case T4("rtap"): w_u32(f, w[0]), w_wordf(f, w + 1, 4); break;
    case T4("rtaa"): w_wordf(f, w, 7); break;
    case T4("qtrn"): w_wordf(f, w, 4); break;
    case T4("txpm"): write_pixmap(f, w); break;
    case T4("txmm"): {
        w_flag(f, w[1], booleans, 2);
        w_flag(f, w[2], pixel_types, 6);
        w_u32(f, w[3]), w_u32(f, w[4]);
        w_u32(f, w[6]), w_u32(f, w[7]), w_u32(f, w[8]), w_u32(f, w[9]);
        uint32_t size = 0, n = w[7] * w[8];
        uint8_t *b = q3_storage_bytes(w[0], &size), *img = calloc(n + 1, 1);
        if (img && b)
            memcpy(img, b, size < n ? size : n);
        w_raw(f, img ? img : (uint8_t *)"", img ? n : 0);
        free(img);
        free(b);
        break;
    }
    default: break;
    }
    end_obj(f);
    if (!kids)
        goto done;
    /* Children. */
    if (light) {
        begin_obj(f, T4("lght"));
        w_flag(f, w[0], booleans, 2);
        w_wordf(f, w + 1, 4);
        end_obj(f);
    }
    if (camera) {
        begin_obj(f, T4("cmpl")), w_wordf(f, w, 9), end_obj(f);
        begin_obj(f, T4("cmrg")), w_wordf(f, w + 9, 2), end_obj(f);
        begin_obj(f, T4("cmvp")), w_wordf(f, w + 11, 4), end_obj(f);
    }
    if (shader) {
        bool ident = q3d_f(w, 0) == 1 && q3d_f(w, 4) == 1 && q3d_f(w, 8) == 1 && !w[1] && !w[2] && !w[3] && !w[5] &&
                     !w[6] && !w[7];
        if (!ident)
            begin_obj(f, T4("sduv")), w_wordf(f, w, 9), end_obj(f);
        if (w[9] || w[10])
            begin_obj(f, T4("shdr")), w_u32(f, w[9]), w_u32(f, w[10]), end_obj(f);
        if (t == T4("txsu") && w[11])
            write_obj(f, w[11]);
    }
    if (t == T4("gpgn") && w[2])
        begin_obj(f, T4("gplh")), w_u32(f, w[2]), end_obj(f);
    if (caps)
        begin_obj(f, T4("caps")), w_u32(f, w[t == T4("tors") ? 17 : 16]), end_obj(f);
    if (capsets) {
        uint32_t codes[4] = { 0 };
        int offs[4] = { -1, -1, -1, -1 };
        if (t == T4("cone")) {
            codes[0] = T4("icap"), offs[0] = 17;
            codes[1] = T4("fcap"), offs[1] = 18;
            codes[2] = T4("bcap"), offs[2] = 19;
        } else if (t == T4("cyln")) {
            codes[0] = T4("icap"), offs[0] = 17;
            codes[1] = T4("tcap"), offs[1] = 18;
            codes[2] = T4("fcap"), offs[2] = 19;
            codes[3] = T4("bcap"), offs[3] = 20;
        } else {
            codes[0] = T4("icap"), offs[0] = t == T4("tors") ? 18 : 17;
        }
        for (int i = 0; i < 4; i++)
            if (offs[i] >= 0 && w[offs[i]]) {
                begin_obj(f, T4("cntr"));
                begin_obj(f, codes[i]);
                end_obj(f);
                write_set(f, w[offs[i]]);
                end_obj(f);
            }
    }
    if (t == T4("high") && w[0])
        write_set(f, w[0]);
    if (t == T4("vwhn")) {
        for (int k = 0; k < 3; k++)
            if (w[k])
                write_obj(f, w[k]);
        if (w[4])
            begin_obj(f, T4("imdm")), w_u32(f, w[5]), w_u32(f, w[6]), end_obj(f);
        if (w[13])
            begin_obj(f, T4("imcc")), w_wordf(f, w + 14, 4), end_obj(f);
        if (w[3])
            write_set(f, w[3]);
    }
    if (trimesh_arrays) {
        for (int pos = 0; pos < 3; pos++) {
            int field = pos == 0 ? 2 : pos == 1 ? 4 : 6;
            q3node *arr = q3d_kid(d, s, 0, field);
            const q3schema *es = s->f[field].elem;
            for (uint32_t i = 0; arr && i < arr->n; i++) {
                q3node *data = q3d_kid(arr, es, i, 0), *use = q3d_kid(arr, es, i, 1);
                int32_t type = (int32_t)arr->w[i * 3];
                int32_t size = q3_element_size(type);
                if (!data || size <= 0)
                    continue;
                begin_obj(f, T4("atar"));
                w_u32(f, (uint32_t)type), w_u32(f, 0), w_u32(f, (uint32_t)pos), w_u32(f, i), w_u32(f, use ? 1 : 0);
                if (use) {
                    for (uint32_t k = 0; k < use->nbytes; k++)
                        w_u(f, 1, use->bytes[k], false);
                }
                for (uint32_t k = 0; k + 4 <= data->nbytes; k += 4) {
                    uint32_t u = (uint32_t)data->bytes[k] << 24 | (uint32_t)data->bytes[k + 1] << 16 |
                                 (uint32_t)data->bytes[k + 2] << 8 | data->bytes[k + 3];
                    if (type == kQ3AttributeTypeHighlightState)
                        w_u32(f, u);
                    else {
                        float fl;
                        memcpy(&fl, &u, 4);
                        w_f32(f, fl);
                    }
                }
                end_obj(f);
            }
        }
    }
    if (vsets)
        write_asl(f, T4("vasl"), vsets, nv);
    if (fsets)
        write_asl(f, T4("fasl"), fsets, nf);
    if (gsets)
        write_asl(f, T4("gasl"), gsets, ng);
    if (sw >= 0 && w[sw] && t != T4("high") && t != T4("vwhn"))
        write_set(f, w[sw]);
    else if (o->set)
        write_set(f, o->set);
    end_obj(f);
done:
    free(vsets);
    free(fsets);
    free(gsets);
}

/* A class of the program's: a type declaring its name first (binary
 * custom types are negative), then its traverse and write methods. */
static int32_t custom_type_id(q3file *f, q3class *k)
{
    for (int i = 0; i < f->ntypes; i++)
        if (!strcmp(f->types[i].name, k->name))
            return f->types[i].id;
    int32_t id = --f->next_custom;
    typeent *n = realloc(f->types, (size_t)(f->ntypes + 1) * sizeof *n);
    if (n) {
        f->types = n;
        f->types[f->ntypes].id = id;
        snprintf(f->types[f->ntypes].name, sizeof f->types[0].name, "%s", k->name);
        f->ntypes++;
    }
    if (!f->text) {
        begin_obj(f, T4("type"));
        w_u32(f, (uint32_t)id);
        w_str(f, k->name);
        end_obj(f);
    }
    return id;
}

static gaddr method(q3class *k, uint32_t m)
{
    for (q3class *c = k; c; c = c->parent ? q3_class(c->parent) : NULL)
        if (c->registered && c->metahandler) {
            gaddr fn = ppc_call(c->metahandler, &m, 1);
            if (fn)
                return fn;
        }
    return 0;
}

static gaddr writing_file; /* the file object being written, for Q3XView_SubmitWriteData */
static q3file *writing;
static q3obj *writing_obj;

static void write_custom(q3file *f, q3obj *o)
{
    gaddr trvs = method(o->cls, Q3T('t', 'r', 'v', 's'));
    q3obj *saved = writing_obj;
    writing_obj = o;
    if (trvs) {
        uint32_t args[3] = { o->g, o->instance, 0 };
        ppc_call(trvs, args, 3); /* it calls Q3XView_SubmitWriteData */
    } else {
        int32_t id = custom_type_id(f, o->cls);
        if (f->text)
            begin_obj(f, 0), f->o[f->olen - 1] = ' ';
        else
            begin_obj(f, (uint32_t)id);
        end_obj(f);
    }
    writing_obj = saved;
}

/* A mesh: its vertices, faces (holes as negative counts after their
 * face) and the vertex and face sets. */
typedef struct {
    uint32_t nv, nf, nc, nidx, capidx;
    float (*pts)[3];
    gaddr *vsets, *fsets;
    int32_t *counts;
    uint32_t *idx;
    uint32_t ncounts, capcounts;
} meshw;

static void mw_vertex(void *ctx, uint32_t i, const float p[3], gaddr set)
{
    meshw *m = ctx;
    if (i >= m->nv)
        return;
    memcpy(m->pts[i], p, 12);
    m->vsets[i] = set;
}

static void mw_face(void *ctx, uint32_t i, uint32_t ncont, const uint32_t *nverts, const uint32_t *verts, gaddr set)
{
    meshw *m = ctx;
    if (i < m->nf)
        m->fsets[i] = set;
    uint32_t at = 0;
    for (uint32_t c = 0; c < ncont; c++) {
        if (m->ncounts == m->capcounts) {
            m->capcounts = m->capcounts ? m->capcounts * 2 : 64;
            int32_t *n = realloc(m->counts, m->capcounts * sizeof *n);
            if (!n)
                return;
            m->counts = n;
        }
        m->counts[m->ncounts++] = c ? -(int32_t)nverts[c] : (int32_t)nverts[c];
        m->nc += c > 0;
        for (uint32_t k = 0; k < nverts[c]; k++) {
            if (m->nidx == m->capidx) {
                m->capidx = m->capidx ? m->capidx * 2 : 256;
                uint32_t *n = realloc(m->idx, m->capidx * sizeof *n);
                if (!n)
                    return;
                m->idx = n;
            }
            m->idx[m->nidx++] = verts[at + k];
        }
        at += nverts[c];
    }
}

static void mw_count_v(void *ctx, uint32_t i, const float p[3], gaddr set) { ((meshw *)ctx)->nv = i + 1; }
static void mw_count_f(void *ctx, uint32_t i, uint32_t n, const uint32_t *nv, const uint32_t *v, gaddr set)
{
    ((meshw *)ctx)->nf = i + 1;
}

static void write_mesh(q3file *f, q3obj *o)
{
    meshw m;
    memset(&m, 0, sizeof m);
    q3_mesh_walk(o, &(q3mesh_walker){ mw_count_v, mw_count_f, NULL, NULL }, &m);
    m.pts = calloc(m.nv + 1, sizeof *m.pts);
    m.vsets = calloc(m.nv + 1, sizeof *m.vsets);
    m.fsets = calloc(m.nf + 1, sizeof *m.fsets);
    if (m.pts && m.vsets && m.fsets) {
        q3_mesh_walk(o, &(q3mesh_walker){ mw_vertex, mw_face, NULL, NULL }, &m);
        gaddr set = q3_mesh_set(o);
        begin_obj(f, T4("cntr"));
        begin_obj(f, T4("mesh"));
        w_u32(f, m.nv);
        for (uint32_t i = 0; i < m.nv; i++)
            for (int k = 0; k < 3; k++)
                w_f32(f, m.pts[i][k]);
        w_u32(f, m.nf);
        w_u32(f, m.nc);
        for (uint32_t i = 0, at = 0; i < m.ncounts; i++) {
            uint32_t n = (uint32_t)(m.counts[i] < 0 ? -m.counts[i] : m.counts[i]);
            w_u(f, 4, (uint32_t)m.counts[i], true);
            for (uint32_t k = 0; k < n; k++)
                w_u32(f, m.idx[at++]);
        }
        end_obj(f);
        write_asl(f, T4("vasl"), m.vsets, m.nv);
        write_asl(f, T4("fasl"), m.fsets, m.nf);
        if (set)
            write_set(f, set);
        end_obj(f);
    }
    free(m.pts);
    free(m.vsets);
    free(m.fsets);
    free(m.counts);
    free(m.idx);
}

static void write_obj(q3file *f, gaddr g)
{
    q3obj *o = q3_get(g);
    if (!o)
        return;
    uint32_t t = o->type;
    if (o->cls->registered) {
        write_custom(f, o);
        return;
    }
    if (q3_type_is(t, Q3_SET)) {
        write_set(f, g);
        return;
    }
    if (q3_type_is(t, Q3_GROUP)) {
        q3group *gr = o->data;
        begin_obj(f, T4("bgng"));
        begin_obj(f, t);
        end_obj(f);
        end_obj(f);
        if (q3_type_is(t, Q3_DISPLAY_GROUP) && gr->state != (1u | 4 | 8 | 16 | 32)) {
            uint32_t x = 0, st = gr->state;
            x |= st & 1 ? 0 : 1;
            x |= st & 2 ? 2 : 0;
            x |= st & 4 ? 0 : 4;
            x |= st & 8 ? 0 : 8;
            x |= st & 16 ? 0 : 16;
            begin_obj(f, T4("dgst")), w_u32(f, x), end_obj(f);
        }
        for (q3pos *p = gr->first; p; p = p->next)
            write_obj(f, p->obj);
        begin_obj(f, T4("endg"));
        end_obj(f);
        return;
    }
    if (t == Q3_CSTRING) {
        begin_obj(f, T4("strc"));
        w_str(f, o->data ? o->data : "");
        end_obj(f);
        return;
    }
    if (t == UNKNOWN_BINARY || t == UNKNOWN_TEXT) {
        unknown *u = o->data;
        if (t == UNKNOWN_BINARY && !f->text) {
            w_u32(f, u->type);
            w_u32(f, u->size);
            out(f, u->contents, u->size);
            pad4(f);
        } else if (t == UNKNOWN_TEXT && f->text) {
            if (!f->line_start)
                newline(f);
            text_out(f, u->name ? u->name : "Unknown");
            text_out(f, "(");
            out(f, u->contents, u->size);
            text_out(f, ")");
            newline(f);
        }
        return;
    }
    if (q3_type_is(t, Q3_RENDERER)) {
        begin_obj(f, t);
        end_obj(f);
        return;
    }
    if (o->type == Q3T('m', 'e', 's', 'h')) {
        write_mesh(f, o);
        return;
    }
    if (q3_schema_of(t) && o->data && !q3_type_is(t, Q3_DRAW_CONTEXT)) {
        write_data(f, o);
        return;
    }
    /* Shaders with no data of their own, state operators and the rest. */
    if (q3_type_is(t, Q3_STATE_OPERATOR)) {
        begin_obj(f, t);
        end_obj(f);
    }
}

/* ---------------------------------------------------------------------- */
/* Open and close */

static void free_read(q3file *f)
{
    for (int i = 0; i < f->ntoc; i++)
        if (f->toc[i].obj)
            q3_release(f->toc[i].obj);
    free(f->toc);
    f->toc = NULL;
    f->ntoc = 0;
    free(f->types);
    f->types = NULL;
    f->ntypes = 0;
    free(f->b);
    f->b = NULL;
    f->n = f->eof = f->pos = 0;
}

static void close_file(q3file *f, bool write_out)
{
    if (!f->open)
        return;
    if (f->writing && write_out && f->storage) {
        q3_storage_write(f->storage, 0, f->olen, f->o);
        q3_storage_truncate(f->storage, f->olen);
    }
    free(f->o);
    f->o = NULL;
    f->olen = f->ocap = 0;
    free_read(f);
    if (f->storage)
        q3_storage_close(f->storage);
    f->open = f->writing = false;
}

static void start_write(q3file *f, uint32_t mode)
{
    f->writing = true;
    f->text = mode & kQ3FileModeText;
    f->mode = mode;
    f->version = 0x00010006;
    f->olen = 0;
    f->nw = 0;
    f->tdepth = 0;
    f->line_start = true;
    f->next_custom = 0;
    if (f->text) {
        static const char *const modes[] = { "Normal", "Stream", "Database" };
        char head[96];
        snprintf(head, sizeof head, "3DMetafile ( 1 6 %s tableofcontents0> )\n", modes[mode & 3 ? (mode & 2 ? 2 : 1) : 0]);
        out(f, head, (uint32_t)strlen(head));
        return;
    }
    out(f, "3DMF", 4);
    w_u32(f, 16);
    w_u32(f, f->version);
    w_u32(f, mode & 3);
    w_u(f, 8, 0, false);
}

/* ---------------------------------------------------------------------- */
/* What the rest of QuickDraw 3D asks of files */

void q3_file_write_object(gaddr file, gaddr object, q3view *v)
{
    q3file *f = file_of(file);
    if (f && f->open && f->writing) {
        writing_file = file;
        writing = f;
        write_obj(f, object);
        writing = NULL;
    }
}

bool q3_file_start_writing(gaddr file)
{
    q3file *f = file_of(file);
    return f && f->open && f->writing;
}

bool q3_file_end_writing(gaddr file) { return file_of(file) != NULL; }

gaddr q3_read_3dmf(const uint8_t *bytes, uint32_t size)
{
    q3group *gr = calloc(1, sizeof *gr);
    q3obj *g = gr ? q3_new(Q3_DISPLAY_GROUP, gr) : NULL;
    if (!g) {
        free(gr);
        return 0;
    }
    gr->state = 1 | 4 | 8 | 16 | 32;
    q3file f;
    memset(&f, 0, sizeof f);
    uint8_t *copy = malloc(size ? size : 1);
    if (!copy || !open_bytes(&f, (memcpy(copy, bytes, size), copy), size)) {
        free(f.b ? f.b : copy);
        q3_release(g->g);
        return 0;
    }
    while (!at_file_end(&f)) {
        uint32_t at = f.pos;
        item it = read_item(&f);
        gaddr o = finish(&it);
        if (o) {
            q3_group_add(g->g, o);
            q3_release(o);
        }
        if (f.pos <= at)
            break;
    }
    free_read(&f);
    return g->g;
}

uint8_t *q3_write_3dmf(gaddr object, uint32_t *size)
{
    q3file f;
    memset(&f, 0, sizeof f);
    f.open = true;
    start_write(&f, kQ3FileModeNormal);
    write_obj(&f, object);
    *size = f.olen;
    return f.o;
}

static void file_classes_only(void)
{
    q3_class_ops(FILE_TYPE, file_dispose, NULL, NULL, false);
    q3_class_ops(UNKNOWN_TEXT, unknown_dispose, unknown_duplicate, NULL, false);
    q3_class_ops(UNKNOWN_BINARY, unknown_dispose, unknown_duplicate, NULL, false);
}

void q3_file_classes(void)
{
    file_classes_only();
    q3_storage_classes();
}

/* ---------------------------------------------------------------------- */
/* The calls */

static q3file *file_arg(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), FILE_TYPE);
    return o ? o->data : NULL;
}

static void Q3File_New(ppc_cpu *c)
{
    q3file *f = calloc(1, sizeof *f);
    q3obj *o = f ? q3_new(FILE_TYPE, f) : NULL;
    if (!o)
        free(f);
    Q3RETURN(o ? o->g : 0);
}

static void Q3File_GetStorage(ppc_cpu *c)
{
    q3file *f = file_arg(c);
    if (f && Q3R(1))
        wr32(Q3R(1), f->storage ? q3_ref(f->storage) : 0);
    Q3RETURN(f && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3File_SetStorage(ppc_cpu *c)
{
    q3file *f = file_arg(c);
    if (f && f->open) {
        q3_error(kQ3ErrorFileIsOpen);
        Q3RETURN(kQ3Failure);
        return;
    }
    if (f && Q3R(1) && !q3_of(Q3R(1), Q3_STORAGE)) {
        Q3RETURN(kQ3Failure);
        return;
    }
    if (f)
        q3_assign(&f->storage, Q3R(1));
    Q3RETURN(f ? kQ3Success : kQ3Failure);
}

static void Q3File_OpenRead(ppc_cpu *c)
{
    q3file *f = file_arg(c);
    if (!f || f->open || !f->storage) {
        q3_error(!f ? kQ3ErrorInvalidObject : f->open ? kQ3ErrorFileAlreadyOpen : kQ3ErrorNoStorageSetForFile);
        Q3RETURN(kQ3Failure);
        return;
    }
    if (!q3_storage_open(f->storage)) {
        Q3RETURN(kQ3Failure);
        return;
    }
    uint32_t n = 0;
    uint8_t *b = q3_storage_bytes(f->storage, &n);
    if (!b || !open_bytes(f, b, n)) {
        free(f->b ? f->b : b);
        f->b = NULL;
        q3_storage_close(f->storage);
        q3_error(kQ3ErrorInvalidMetafile);
        Q3RETURN(kQ3Failure);
        return;
    }
    f->open = true;
    f->writing = false;
    f->in_group = 0;
    if (Q3R(1))
        wr32(Q3R(1), f->mode);
    Q3RETURN(kQ3Success);
}

static void Q3File_OpenWrite(ppc_cpu *c)
{
    q3file *f = file_arg(c);
    if (!f || f->open || !f->storage) {
        q3_error(!f ? kQ3ErrorInvalidObject : f->open ? kQ3ErrorFileAlreadyOpen : kQ3ErrorNoStorageSetForFile);
        Q3RETURN(kQ3Failure);
        return;
    }
    if (!q3_storage_open(f->storage)) {
        Q3RETURN(kQ3Failure);
        return;
    }
    f->open = true;
    start_write(f, Q3R(1));
    Q3RETURN(kQ3Success);
}

static void Q3File_IsOpen(ppc_cpu *c)
{
    q3file *f = file_arg(c);
    if (f && Q3R(1))
        wr32(Q3R(1), f->open);
    Q3RETURN(f && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3File_GetMode(ppc_cpu *c)
{
    q3file *f = file_arg(c);
    if (f && !f->open)
        q3_error(kQ3ErrorFileNotOpen);
    if (f && f->open && Q3R(1))
        wr32(Q3R(1), f->mode);
    Q3RETURN(f && f->open && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3File_GetVersion(ppc_cpu *c)
{
    q3file *f = file_arg(c);
    if (f && !f->open)
        q3_error(kQ3ErrorFileNotOpen);
    if (f && f->open && Q3R(1))
        wr32(Q3R(1), f->version);
    Q3RETURN(f && f->open && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3File_Close(ppc_cpu *c)
{
    q3file *f = file_arg(c);
    if (f && !f->open)
        q3_error(kQ3ErrorFileNotOpen);
    bool ok = f && f->open;
    if (ok)
        close_file(f, true);
    Q3RETURN(ok ? kQ3Success : kQ3Failure);
}

static void Q3File_Cancel(ppc_cpu *c)
{
    q3file *f = file_arg(c);
    if (f && f->open) {
        close_file(f, false);
        q3_notice(-28085); /* kQ3NoticeFileCancelled */
    }
    Q3RETURN(f ? kQ3Success : kQ3Failure);
}

static q3file *reading_arg(ppc_cpu *c)
{
    q3file *f = file_arg(c);
    if (f && (!f->open || f->writing)) {
        q3_error(!f->open ? kQ3ErrorFileNotOpen : kQ3ErrorFileModeRestriction);
        return NULL;
    }
    return f;
}

/* The type of the next object, without reading it. */
static uint32_t peek_type(q3file *f)
{
    uint32_t pos = f->pos;
    int depth = f->depth;
    uint32_t type = 0;
    if (f->text) {
        char t[256];
        for (int guard = 0; guard < 8; guard++) {
            int k = token(f, t, sizeof t, true);
            if (k == TK_WORD && t[strlen(t) - 1] == ':')
                continue;
            if (k != TK_WORD)
                break;
            type = type_of_name(t);
            if (type != T4("cntr") && type != T4("bgng"))
                break;
            if (token(f, t, sizeof t, true) != TK_LP)
                break;
        }
    } else {
        for (int guard = 0; guard < 8 && f->pos + 8 <= limit_of(f); guard++) {
            uint64_t t = 0, s = 0;
            bin_u(f, 4, &t);
            bin_u(f, 4, &s);
            type = (uint32_t)t;
            if (type == T4("type") || type == T4("toc ")) {
                f->pos += (uint32_t)s;
                continue;
            }
            if (type == T4("rfrn")) {
                uint64_t id = 0;
                bin_u(f, 4, &id);
                type = 0;
                for (int i = 0; i < f->ntoc; i++)
                    if (f->toc[i].refid == (uint32_t)id && f->toc[i].pos) {
                        f->pos = f->toc[i].pos;
                        type = 1;
                    }
                if (!type)
                    break;
                continue;
            }
            if (type != T4("cntr") && type != T4("bgng"))
                break;
        }
        if ((int32_t)type < 0 && custom_name(f, (int32_t)type)) {
            q3class *k = q3_class_named(custom_name(f, (int32_t)type));
            type = k ? k->type : type;
        }
    }
    f->pos = pos;
    f->depth = depth;
    static const struct {
        uint32_t code, type;
    } support[] = { { T4("attr"), Q3_ATTRIBUTE_SET } };
    for (unsigned i = 0; i < sizeof support / sizeof support[0]; i++)
        if (support[i].code == type)
            type = support[i].type;
    return type;
}

static void Q3File_GetNextObjectType(ppc_cpu *c)
{
    q3file *f = reading_arg(c);
    Q3RETURN(f && !at_file_end(f) ? peek_type(f) : 0);
}

static void Q3File_IsNextObjectOfType(ppc_cpu *c)
{
    q3file *f = reading_arg(c);
    uint32_t t = f && !at_file_end(f) ? peek_type(f) : 0;
    Q3RETURN(t && (t == Q3R(1) || q3_type_is(t, Q3R(1))));
}

static void Q3File_ReadObject(ppc_cpu *c)
{
    q3file *f = reading_arg(c);
    if (!f || at_file_end(f)) {
        if (f)
            q3_error(kQ3ErrorEndOfFile);
        Q3RETURN(0);
        return;
    }
    gaddr saved = current_file;
    current_file = Q3R(0);
    item it = read_item(f);
    if (it.kind == IT_ENDG && f->in_group > 0)
        f->in_group--;
    gaddr g = finish(&it);
    current_file = saved;
    if (f->idle) {
        uint32_t args[2] = { Q3R(0), f->idle_data };
        if (ppc_call(f->idle, args, 2) != kQ3Success)
            close_file(f, false);
    }
    Q3RETURN(g);
}

static void Q3File_SkipObject(ppc_cpu *c)
{
    q3file *f = reading_arg(c);
    if (!f || at_file_end(f)) {
        Q3RETURN(kQ3Failure);
        return;
    }
    item it = read_item(f);
    gaddr g = finish(&it);
    if (g)
        q3_release(g);
    Q3RETURN(kQ3Success);
}

static void Q3File_IsEndOfData(ppc_cpu *c)
{
    q3file *f = reading_arg(c);
    Q3RETURN(!f || at_data_end(f));
}

static void Q3File_IsEndOfContainer(ppc_cpu *c)
{
    q3file *f = reading_arg(c);
    if (!f) {
        Q3RETURN(kQ3True);
        return;
    }
    for (int i = f->nfr - 1; i >= 0; i--)
        if (f->fr[i].kind == FR_CONTAINER) {
            if (f->text) {
                char t[8];
                int k = token(f, t, sizeof t, false);
                Q3RETURN(k == TK_RP || k == TK_END);
            } else {
                Q3RETURN(f->pos + 8 > f->fr[i].end);
            }
            return;
        }
    Q3RETURN(kQ3True);
}

static void Q3File_IsEndOfFile(ppc_cpu *c)
{
    q3file *f = reading_arg(c);
    Q3RETURN(!f || at_file_end(f));
}

static void Q3File_MarkAsExternalReference(ppc_cpu *c)
{
    q3file *f = file_arg(c);
    if (f && !f->ext_refs) {
        q3group *g = calloc(1, sizeof *g);
        q3obj *o = g ? q3_new(Q3_GROUP, g) : NULL;
        if (!o)
            free(g);
        f->ext_refs = o ? o->g : 0;
    }
    if (f && f->ext_refs && Q3R(1))
        q3_group_add(f->ext_refs, Q3R(1));
    Q3RETURN(f && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3File_GetExternalReferences(ppc_cpu *c)
{
    q3file *f = file_arg(c);
    Q3RETURN(f && f->ext_refs ? q3_ref(f->ext_refs) : 0);
}

static void Q3File_SetReadInGroup(ppc_cpu *c)
{
    q3file *f = file_arg(c);
    if (f)
        f->read_group = Q3R(1) & kQ3FileReadObjectsInGroup;
    Q3RETURN(f ? kQ3Success : kQ3Failure);
}

static void Q3File_GetReadInGroup(ppc_cpu *c)
{
    q3file *f = file_arg(c);
    if (f && Q3R(1))
        wr32(Q3R(1), f->read_group | (f->in_group ? kQ3FileCurrentlyInsideGroup : 0));
    Q3RETURN(f && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3File_SetIdleMethod(ppc_cpu *c)
{
    q3file *f = file_arg(c);
    if (f) {
        f->idle = Q3R(1);
        f->idle_data = Q3R(2);
    }
    Q3RETURN(f ? kQ3Success : kQ3Failure);
}

/* Edit tracking: the edit index when cleared, per object. */
static struct {
    gaddr g;
    uint32_t edit;
} tracked[256];

static void Q3Shared_ClearEditTracking(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_SHARED);
    if (o) {
        int slot = -1;
        for (int i = 0; i < 256; i++)
            if (tracked[i].g == o->g || (slot < 0 && !q3_get(tracked[i].g)))
                slot = i;
        if (slot < 0)
            slot = (int)(o->g / 4 % 256);
        tracked[slot].g = o->g;
        tracked[slot].edit = o->edit;
    }
    Q3RETURN(o ? kQ3Success : kQ3Failure);
}

static void Q3Shared_GetEditTrackingState(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_SHARED);
    if (!o) {
        Q3RETURN(kQ3False);
        return;
    }
    for (int i = 0; i < 256; i++)
        if (tracked[i].g == o->g) {
            Q3RETURN(tracked[i].edit != o->edit);
            return;
        }
    Q3RETURN(kQ3True);
}

/* ---- The primitives ---- */

static q3file *prim_file(gaddr g, bool write)
{
    q3file *f = file_of(g);
    if (f && (!f->open || f->writing != write)) {
        q3_error(!f->open ? kQ3ErrorFileNotOpen : kQ3ErrorFileModeRestriction);
        return NULL;
    }
    return f;
}

#define READ_INT(Name, BYTES, SIGNED)                                                                                \
    static void Q3##Name##_Read(ppc_cpu *c)                                                                          \
    {                                                                                                                \
        q3file *f = prim_file(Q3R(1), false);                                                                        \
        uint64_t v = 0;                                                                                              \
        bool ok = f && Q3R(0) && r_u(f, BYTES, &v);                                                                  \
        if (ok) {                                                                                                    \
            if ((BYTES) == 1)                                                                                        \
                wr8(Q3R(0), (uint8_t)v);                                                                             \
            else if ((BYTES) == 2)                                                                                   \
                wr16(Q3R(0), (uint16_t)v);                                                                           \
            else if ((BYTES) == 4)                                                                                   \
                wr32(Q3R(0), (uint32_t)v);                                                                           \
            else {                                                                                                   \
                wr32(Q3R(0), (uint32_t)(v >> 32));                                                                   \
                wr32(Q3R(0) + 4, (uint32_t)v);                                                                       \
            }                                                                                                        \
        }                                                                                                            \
        Q3RETURN(ok ? kQ3Success : kQ3Failure);                                                                      \
    }                                                                                                                \
    static void Q3##Name##_Write(ppc_cpu *c)                                                                         \
    {                                                                                                                \
        q3file *f = prim_file(Q3R(1), true);                                                                         \
        if (f) {                                                                                                     \
            uint64_t v = (BYTES) == 8 ? (uint64_t)rd32(Q3R(0)) << 32 | rd32(Q3R(0) + 4) : Q3R(0);                    \
            w_u(f, BYTES, v, SIGNED);                                                                                \
        }                                                                                                            \
        Q3RETURN(f ? kQ3Success : kQ3Failure);                                                                       \
    }

READ_INT(Uns8, 1, false)
READ_INT(Uns16, 2, false)
READ_INT(Uns32, 4, false)
READ_INT(Int8, 1, true)
READ_INT(Int16, 2, true)
READ_INT(Int32, 4, true)

/* 64-bit values come by value in two registers (hi, lo); the file next. */
static void Q3Uns64_Write(ppc_cpu *c)
{
    q3file *f = prim_file(Q3R(2), true);
    if (f)
        w_u(f, 8, (uint64_t)Q3R(0) << 32 | Q3R(1), false);
    Q3RETURN(f ? kQ3Success : kQ3Failure);
}
static void Q3Int64_Write(ppc_cpu *c)
{
    q3file *f = prim_file(Q3R(2), true);
    if (f)
        w_u(f, 8, (uint64_t)Q3R(0) << 32 | Q3R(1), true);
    Q3RETURN(f ? kQ3Success : kQ3Failure);
}
static void Q3Uns64_Read(ppc_cpu *c)
{
    q3file *f = prim_file(Q3R(1), false);
    uint64_t v = 0;
    bool ok = f && Q3R(0) && r_u(f, 8, &v);
    if (ok) {
        wr32(Q3R(0), (uint32_t)(v >> 32));
        wr32(Q3R(0) + 4, (uint32_t)v);
    }
    Q3RETURN(ok ? kQ3Success : kQ3Failure);
}
static void Q3Int64_Read(ppc_cpu *c) { Q3Uns64_Read(c); }

static void Q3Float32_Read(ppc_cpu *c)
{
    q3file *f = prim_file(Q3R(1), false);
    float v = 0;
    bool ok = f && Q3R(0) && r_f32(f, &v);
    if (ok)
        wrf(Q3R(0), v);
    Q3RETURN(ok ? kQ3Success : kQ3Failure);
}

/* The float comes in f1 (and takes r3's slot): the file is in r4. */
static void Q3Float32_Write(ppc_cpu *c)
{
    q3file *f = prim_file(Q3R(1), true);
    if (f)
        w_f32(f, q3_farg(c, 0));
    Q3RETURN(f ? kQ3Success : kQ3Failure);
}

static void Q3Float64_Read(ppc_cpu *c)
{
    q3file *f = prim_file(Q3R(1), false);
    double v = 0;
    bool ok = f && Q3R(0) && r_f64(f, &v);
    if (ok) {
        uint64_t u;
        memcpy(&u, &v, 8);
        wr32(Q3R(0), (uint32_t)(u >> 32));
        wr32(Q3R(0) + 4, (uint32_t)u);
    }
    Q3RETURN(ok ? kQ3Success : kQ3Failure);
}

/* A double takes two slots: the file is in r5. */
static void Q3Float64_Write(ppc_cpu *c)
{
    q3file *f = prim_file(Q3R(2), true);
    if (f) {
        double d = ppc_fpr(c, 1);
        if (f->text) {
            char b[40];
            snprintf(b, sizeof b, "%.17g", d);
            text_out(f, b);
        } else {
            uint64_t u;
            memcpy(&u, &d, 8);
            w_u(f, 8, u, false);
        }
    }
    Q3RETURN(f ? kQ3Success : kQ3Failure);
}

static void Q3Size_Pad(ppc_cpu *c) { Q3RETURN((Q3R(0) + 3) & ~3u); }

static void Q3String_Read(ppc_cpu *c)
{
    q3file *f = prim_file(Q3R(2), false);
    char s[1024];
    uint32_t len = 0;
    bool ok = f && r_str(f, s, sizeof s, &len);
    if (ok && Q3R(0))
        gput(Q3R(0), s, (uint32_t)strlen(s) + 1);
    if (ok && Q3R(1))
        wr32(Q3R(1), len);
    Q3RETURN(ok ? kQ3Success : kQ3Failure);
}

static void Q3String_Write(ppc_cpu *c)
{
    q3file *f = prim_file(Q3R(1), true);
    char s[1024];
    q3_gstr(Q3R(0), s, sizeof s);
    if (f)
        w_str(f, s);
    Q3RETURN(f ? kQ3Success : kQ3Failure);
}

static void Q3RawData_Read(ppc_cpu *c)
{
    q3file *f = prim_file(Q3R(2), false);
    uint32_t n = Q3R(1);
    uint8_t *b = f && n < (64u << 20) ? malloc(n ? n : 1) : NULL;
    bool ok = b && r_raw(f, b, n);
    if (ok && Q3R(0))
        gput(Q3R(0), b, n);
    free(b);
    Q3RETURN(ok ? kQ3Success : kQ3Failure);
}

static void Q3RawData_Write(ppc_cpu *c)
{
    q3file *f = prim_file(Q3R(2), true);
    uint32_t n = Q3R(1);
    uint8_t *b = f && n < (64u << 20) ? malloc(n ? n : 1) : NULL;
    if (b && n)
        gget(b, Q3R(0), n);
    if (b)
        w_raw(f, b, n);
    free(b);
    Q3RETURN(b ? kQ3Success : kQ3Failure);
}

/* Structures of floats: n of them at the pointer. */
static void floats_read(ppc_cpu *c, int n)
{
    q3file *f = prim_file(Q3R(1), false);
    float v[16];
    bool ok = f && Q3R(0) && r_fs(f, v, n);
    for (int i = 0; ok && i < n; i++)
        wrf(Q3R(0) + 4 * (uint32_t)i, v[i]);
    Q3RETURN(ok ? kQ3Success : kQ3Failure);
}

static void floats_write(ppc_cpu *c, int n)
{
    q3file *f = prim_file(Q3R(1), true);
    for (int i = 0; f && Q3R(0) && i < n; i++)
        w_f32(f, rdf(Q3R(0) + 4 * (uint32_t)i));
    Q3RETURN(f && Q3R(0) ? kQ3Success : kQ3Failure);
}

#define FLOATS(Name, N)                                                                                              \
    static void Q3##Name##_Read(ppc_cpu *c) { floats_read(c, N); }                                                   \
    static void Q3##Name##_Write(ppc_cpu *c) { floats_write(c, N); }
FLOATS(Point2D, 2)
FLOATS(Point3D, 3)
FLOATS(RationalPoint3D, 3)
FLOATS(RationalPoint4D, 4)
FLOATS(Vector2D, 2)
FLOATS(Vector3D, 3)
FLOATS(Matrix4x4, 16)
FLOATS(Tangent2D, 6)
FLOATS(Tangent3D, 9)

static void Q3Comment_Write(ppc_cpu *c)
{
    q3file *f = prim_file(Q3R(1), true);
    if (f && f->text) {
        char s[1024];
        q3_gstr(Q3R(0), s, sizeof s);
        if (!f->line_start)
            newline(f);
        text_out(f, "#");
        text_out(f, s);
        newline(f);
    }
    Q3RETURN(f ? kQ3Success : kQ3Failure);
}

static void Q3NewLine_Write(ppc_cpu *c)
{
    q3file *f = prim_file(Q3R(0), true);
    if (f && f->text)
        newline(f);
    Q3RETURN(f ? kQ3Success : kQ3Failure);
}

/* A program's class written: its data, by its write method. */
static void Q3XView_SubmitWriteData(ppc_cpu *c)
{
    q3view *v = q3v_of(Q3R(0));
    uint32_t size = Q3R(1);
    gaddr data = Q3R(2), del = Q3R(3);
    q3file *f = writing;
    q3obj *o = writing_obj;
    if (!v || !f || !o) {
        Q3RETURN(kQ3Failure);
        return;
    }
    int32_t id = custom_type_id(f, o->cls);
    if (f->text) {
        if (!f->line_start)
            newline(f);
        text_out(f, o->cls->name);
        text_out(f, "(");
        f->tdepth++;
    } else {
        begin_obj(f, (uint32_t)id);
    }
    gaddr writ = method(o->cls, Q3T('w', 'r', 'i', 't'));
    if (writ && size) {
        uint32_t args[2] = { data, writing_file };
        ppc_call(writ, args, 2);
    }
    end_obj(f);
    if (del) {
        uint32_t arg = data;
        ppc_call(del, &arg, 1);
    }
    Q3RETURN(kQ3Success);
}

/* A sub-object of an element or class: written after with its own
 * class's write method. */
static void Q3XView_SubmitSubObjectData(ppc_cpu *c)
{
    q3view *v = q3v_of(Q3R(0));
    q3class *k = NULL;
    extern q3class *q3_class_of_handle(gaddr);
    (void)k;
    if (!v || !writing) {
        Q3RETURN(kQ3Failure);
        return;
    }
    gaddr del = Q3R(4);
    if (del) {
        uint32_t arg = Q3R(3);
        ppc_call(del, &arg, 1);
    }
    Q3RETURN(kQ3Success);
}

/* ---- Unknown objects ---- */

static void Q3Unknown_GetType(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_UNKNOWN);
    Q3RETURN(o ? q3_type_below(o->type, Q3_UNKNOWN) : 0);
}

static void Q3Unknown_GetDirtyState(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_UNKNOWN);
    if (o && Q3R(1))
        wr32(Q3R(1), ((unknown *)o->data)->dirty);
    Q3RETURN(o && Q3R(1) ? kQ3Success : kQ3Failure);
}

static void Q3Unknown_SetDirtyState(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_UNKNOWN);
    if (o)
        ((unknown *)o->data)->dirty = Q3R(1) != 0;
    Q3RETURN(o ? kQ3Success : kQ3Failure);
}

static gaddr gdup(const char *s, uint32_t n)
{
    gaddr g = q3_galloc(n + 1);
    if (g) {
        if (n)
            gput(g, s, n);
        wr8(g + n, 0);
    }
    return g;
}

static void Q3UnknownText_GetData(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), UNKNOWN_TEXT);
    gaddr d = Q3R(1);
    if (!o || !d) {
        Q3RETURN(kQ3Failure);
        return;
    }
    unknown *u = o->data;
    wr32(d, gdup(u->name ? u->name : "", u->name ? (uint32_t)strlen(u->name) : 0));
    wr32(d + 4, gdup((const char *)u->contents, u->size));
    Q3RETURN(kQ3Success);
}

static void Q3UnknownText_EmptyData(ppc_cpu *c)
{
    gaddr d = Q3R(0);
    if (d) {
        q3_gfree(rd32(d));
        q3_gfree(rd32(d + 4));
        wr32(d, 0);
        wr32(d + 4, 0);
    }
    Q3RETURN(d ? kQ3Success : kQ3Failure);
}

static void Q3UnknownBinary_GetData(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), UNKNOWN_BINARY);
    gaddr d = Q3R(1);
    if (!o || !d) {
        Q3RETURN(kQ3Failure);
        return;
    }
    unknown *u = o->data;
    wr32(d, u->type);
    wr32(d + 4, u->size);
    wr32(d + 8, u->byte_order);
    gaddr g = q3_galloc(u->size);
    if (g && u->size)
        gput(g, u->contents, u->size);
    wr32(d + 12, g);
    Q3RETURN(kQ3Success);
}

static void Q3UnknownBinary_EmptyData(ppc_cpu *c)
{
    gaddr d = Q3R(0);
    if (d) {
        q3_gfree(rd32(d + 12));
        wr32(d + 12, 0);
    }
    Q3RETURN(d ? kQ3Success : kQ3Failure);
}

static void Q3UnknownBinary_GetTypeString(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), UNKNOWN_BINARY);
    if (!o || !Q3R(1)) {
        Q3RETURN(kQ3Failure);
        return;
    }
    unknown *u = o->data;
    wr32(Q3R(1), u->name ? gdup(u->name, (uint32_t)strlen(u->name)) : 0);
    Q3RETURN(kQ3Success);
}

static void Q3UnknownBinary_EmptyTypeString(ppc_cpu *c)
{
    if (Q3R(0)) {
        q3_gfree(rd32(Q3R(0)));
        wr32(Q3R(0), 0);
    }
    Q3RETURN(kQ3Success);
}

#define E(n) { #n, n }
const ppc_export q3_file_exports[] = {
    E(Q3XView_SubmitWriteData), E(Q3XView_SubmitSubObjectData), E(Q3File_New), E(Q3File_GetStorage),
    E(Q3File_SetStorage), E(Q3File_OpenRead), E(Q3File_OpenWrite), E(Q3File_IsOpen), E(Q3File_GetMode),
    E(Q3File_GetVersion), E(Q3File_Close), E(Q3File_Cancel), E(Q3File_GetNextObjectType),
    E(Q3File_IsNextObjectOfType), E(Q3File_ReadObject), E(Q3File_SkipObject), E(Q3File_IsEndOfData),
    E(Q3File_IsEndOfContainer), E(Q3File_IsEndOfFile), E(Q3File_MarkAsExternalReference),
    E(Q3File_GetExternalReferences), E(Q3Shared_ClearEditTracking), E(Q3Shared_GetEditTrackingState),
    E(Q3File_SetReadInGroup), E(Q3File_GetReadInGroup), E(Q3File_SetIdleMethod), E(Q3NewLine_Write),
    E(Q3Uns8_Read), E(Q3Uns8_Write), E(Q3Uns16_Read), E(Q3Uns16_Write), E(Q3Uns32_Read), E(Q3Uns32_Write),
    E(Q3Int8_Read), E(Q3Int8_Write), E(Q3Int16_Read), E(Q3Int16_Write), E(Q3Int32_Read), E(Q3Int32_Write),
    E(Q3Uns64_Read), E(Q3Uns64_Write), E(Q3Int64_Read), E(Q3Int64_Write), E(Q3Float32_Read), E(Q3Float32_Write),
    E(Q3Float64_Read), E(Q3Float64_Write), E(Q3Size_Pad), E(Q3String_Read), E(Q3String_Write), E(Q3RawData_Read),
    E(Q3RawData_Write), E(Q3Point2D_Read), E(Q3Point2D_Write), E(Q3Point3D_Read), E(Q3Point3D_Write),
    E(Q3RationalPoint3D_Read), E(Q3RationalPoint3D_Write), E(Q3RationalPoint4D_Read), E(Q3RationalPoint4D_Write),
    E(Q3Vector2D_Read), E(Q3Vector2D_Write), E(Q3Vector3D_Read), E(Q3Vector3D_Write), E(Q3Matrix4x4_Read),
    E(Q3Matrix4x4_Write), E(Q3Tangent2D_Read), E(Q3Tangent2D_Write), E(Q3Tangent3D_Read), E(Q3Tangent3D_Write),
    E(Q3Comment_Write), E(Q3Unknown_GetType), E(Q3Unknown_GetDirtyState), E(Q3Unknown_SetDirtyState),
    E(Q3UnknownText_GetData), E(Q3UnknownText_EmptyData), E(Q3UnknownBinary_GetData), E(Q3UnknownBinary_EmptyData),
    E(Q3UnknownBinary_GetTypeString), E(Q3UnknownBinary_EmptyTypeString),
};
const unsigned q3_file_export_count = sizeof q3_file_exports / sizeof q3_file_exports[0];
