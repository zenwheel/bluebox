/*
 * QuickDraw 3D's object model: Q3Initialize and Q3Exit, the class tree
 * (QD3D.h's types and the classes programs register), objects and their
 * handles, shared references, sets and attribute sets, strings, errors,
 * warnings and notices.
 *
 * Handles are words of guest memory, taken in chunks from the system
 * heap: each names a host record (an object, a group position, a mesh
 * part) and nothing else while it lives.
 */
#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>

#include "filemgr_internal.h"
#include "forks.h"
#include "q3_data.h"
#include "q3_obj.h"
#include "q3_view.h"
#include "tb_internal.h"

/* ---------------------------------------------------------------------- */
/* Handles */

enum { CHUNK = 4096, MAX_CHUNKS = 256 };

typedef struct {
    int kind;
    void *p;
} handle;

static gaddr chunk_base[MAX_CHUNKS];
static handle *chunk_slots[MAX_CHUNKS];
static int nchunks;
static uint32_t *free_list; /* chunk << 16 | slot */
static int nfree, capfree;

gaddr q3_handle_new(int kind, void *p)
{
    if (!nfree) {
        if (nchunks == MAX_CHUNKS)
            return 0;
        gaddr base = tb_NewPtr(CHUNK * 4, true, true);
        if (!base)
            base = tb_NewPtr(CHUNK * 4, false, true);
        handle *slots = base ? calloc(CHUNK, sizeof *slots) : NULL;
        if (!slots)
            return 0;
        if (capfree < CHUNK) {
            uint32_t *n = realloc(free_list, CHUNK * sizeof *n);
            if (!n) {
                free(slots);
                return 0;
            }
            free_list = n;
            capfree = CHUNK;
        }
        chunk_base[nchunks] = base;
        chunk_slots[nchunks] = slots;
        for (int i = CHUNK - 1; i >= 0; i--)
            free_list[nfree++] = (uint32_t)nchunks << 16 | (uint32_t)i;
        nchunks++;
    }
    uint32_t id = free_list[--nfree];
    int c = (int)(id >> 16), i = (int)(id & 0xFFFF);
    chunk_slots[c][i] = (handle){ kind, p };
    gaddr g = chunk_base[c] + (uint32_t)i * 4;
    wr32(g, 0x51336F62); /* 'Q3ob', for anyone looking */
    return g;
}

static handle *slot_of(gaddr g)
{
    if (!g || (g & 3))
        return NULL;
    for (int c = 0; c < nchunks; c++)
        if (g >= chunk_base[c] && g < chunk_base[c] + CHUNK * 4)
            return &chunk_slots[c][(g - chunk_base[c]) / 4];
    return NULL;
}

void *q3_handle_get(gaddr g, int kind)
{
    handle *h = slot_of(g);
    return h && h->kind == kind ? h->p : NULL;
}

void q3_handle_free(gaddr g)
{
    handle *h = slot_of(g);
    if (!h || !h->kind)
        return;
    h->kind = 0;
    h->p = NULL;
    wr32(g, 0);
    for (int c = 0; c < nchunks; c++)
        if (g >= chunk_base[c] && g < chunk_base[c] + CHUNK * 4) {
            if (nfree < capfree)
                free_list[nfree++] = (uint32_t)c << 16 | (g - chunk_base[c]) / 4;
            break;
        }
}

/* ---------------------------------------------------------------------- */
/* Classes */

#define T(a) Q3T(a[0], a[1], a[2], a[3])

static q3class builtin[] = {
    { T("view"), 0, "View" },
    { T("elmn"), 0, "Element" },
    { T("eatt"), T("elmn"), "Attribute" },
    { T("pick"), 0, "Pick" },
    { T("pkwp"), T("pick"), "WindowPointPick" },
    { T("pkwr"), T("pick"), "WindowRectPick" },
    { T("pkry"), T("pick"), "WorldRayPick" },
    { T("shrd"), 0, "Shared" },
    { T("rddr"), T("shrd"), "Renderer" },
    { T("wrfr"), T("rddr"), "WireFrame" },
    { T("gnrr"), T("rddr"), "Generic" },
    { T("ctwn"), T("rddr"), "InteractiveRenderer" },
    { T("shap"), T("shrd"), "Shape" },
    { T("gmtr"), T("shap"), "Geometry" },
    { T("box "), T("gmtr"), "Box" },
    { T("gpgn"), T("gmtr"), "GeneralPolygon" },
    { T("line"), T("gmtr"), "Line" },
    { T("mrkr"), T("gmtr"), "Marker" },
    { T("mrkp"), T("gmtr"), "PixmapMarker" },
    { T("mesh"), T("gmtr"), "Mesh" },
    { T("nrbc"), T("gmtr"), "NURBCurve" },
    { T("nrbp"), T("gmtr"), "NURBPatch" },
    { T("pnt "), T("gmtr"), "Point" },
    { T("plyg"), T("gmtr"), "Polygon" },
    { T("plyl"), T("gmtr"), "PolyLine" },
    { T("trng"), T("gmtr"), "Triangle" },
    { T("trig"), T("gmtr"), "TriGrid" },
    { T("cone"), T("gmtr"), "Cone" },
    { T("cyln"), T("gmtr"), "Cylinder" },
    { T("disk"), T("gmtr"), "Disk" },
    { T("elps"), T("gmtr"), "Ellipse" },
    { T("elpd"), T("gmtr"), "Ellipsoid" },
    { T("plhd"), T("gmtr"), "Polyhedron" },
    { T("tors"), T("gmtr"), "Torus" },
    { T("tmsh"), T("gmtr"), "TriMesh" },
    { T("shdr"), T("shap"), "Shader" },
    { T("sush"), T("shdr"), "SurfaceShader" },
    { T("txsu"), T("sush"), "TextureShader" },
    { T("ilsh"), T("shdr"), "IlluminationShader" },
    { T("phil"), T("ilsh"), "PhongIllumination" },
    { T("lmil"), T("ilsh"), "LambertIllumination" },
    { T("nuil"), T("ilsh"), "NULLIllumination" },
    { T("styl"), T("shap"), "Style" },
    { T("bckf"), T("styl"), "BackfacingStyle" },
    { T("intp"), T("styl"), "InterpolationStyle" },
    { T("fist"), T("styl"), "FillStyle" },
    { T("pkid"), T("styl"), "PickIDStyle" },
    { T("rcsh"), T("styl"), "ReceiveShadowsStyle" },
    { T("high"), T("styl"), "HighlightStyle" },
    { T("sbdv"), T("styl"), "SubdivisionStyle" },
    { T("ofdr"), T("styl"), "OrientationStyle" },
    { T("pkpt"), T("styl"), "PickPartsStyle" },
    { T("anti"), T("styl"), "AntiAliasStyle" },
    { T("fogg"), T("styl"), "FogStyle" },
    { T("xfrm"), T("shap"), "Transform" },
    { T("mtrx"), T("xfrm"), "Matrix" },
    { T("scal"), T("xfrm"), "Scale" },
    { T("trns"), T("xfrm"), "Translate" },
    { T("rott"), T("xfrm"), "Rotate" },
    { T("rtap"), T("xfrm"), "RotateAboutPoint" },
    { T("rtaa"), T("xfrm"), "RotateAboutAxis" },
    { T("qtrn"), T("xfrm"), "Quaternion" },
    { T("rset"), T("xfrm"), "Reset" },
    { T("lght"), T("shap"), "Light" },
    { T("ambn"), T("lght"), "AmbientLight" },
    { T("drct"), T("lght"), "DirectionalLight" },
    { T("pntl"), T("lght"), "PointLight" },
    { T("spot"), T("lght"), "SpotLight" },
    { T("cmra"), T("shap"), "Camera" },
    { T("orth"), T("cmra"), "OrthographicCamera" },
    { T("vwpl"), T("cmra"), "ViewPlaneCamera" },
    { T("vana"), T("cmra"), "ViewAngleAspectCamera" },
    { T("stop"), T("shap"), "StateOperator" },
    { T("push"), T("stop"), "Push" },
    { T("pop "), T("stop"), "Pop" },
    { T("grup"), T("shap"), "Group" },
    { T("dspg"), T("grup"), "DisplayGroup" },
    { T("ordg"), T("dspg"), "OrderedDisplayGroup" },
    { T("iopx"), T("dspg"), "IOProxyDisplayGroup" },
    { T("lghg"), T("grup"), "LightGroup" },
    { T("info"), T("grup"), "InfoGroup" },
    { T("unkn"), T("shap"), "Unknown" },
    { T("uktx"), T("unkn"), "UnknownText" },
    { T("ukbn"), T("unkn"), "UnknownBinary" },
    { T("rfrn"), T("shap"), "Reference" },
    { T("rfex"), T("rfrn"), "ExternalReference" },
    { T("set "), T("shrd"), "Set" },
    { T("attr"), T("set "), "AttributeSet" },
    { T("dctx"), T("shrd"), "DrawContext" },
    { T("dpxp"), T("dctx"), "PixmapDrawContext" },
    { T("dmac"), T("dctx"), "MacDrawContext" },
    { T("txtr"), T("shrd"), "Texture" },
    { T("txpm"), T("txtr"), "PixmapTexture" },
    { T("txmm"), T("txtr"), "MipmapTexture" },
    { T("txcp"), T("txtr"), "CompressedPixmapTexture" },
    { T("file"), T("shrd"), "File" },
    { T("strg"), T("shrd"), "Storage" },
    { T("mems"), T("strg"), "MemoryStorage" },
    { T("hndl"), T("mems"), "HandleStorage" },
    { T("macn"), T("strg"), "MacintoshStorage" },
    { T("macp"), T("macn"), "FSSpecStorage" },
    { T("strn"), T("shrd"), "String" },
    { T("strc"), T("strn"), "CString" },
    { T("sprt"), T("shrd"), "ShapePart" },
    { T("spmh"), T("sprt"), "MeshPart" },
    { T("mfac"), T("spmh"), "MeshFacePart" },
    { T("medg"), T("spmh"), "MeshEdgePart" },
    { T("mvtx"), T("spmh"), "MeshVertexPart" },
    { T("ctst"), T("shrd"), "ControllerState" },
    { T("trkr"), T("shrd"), "Tracker" },
    { T("vwhn"), T("shrd"), "ViewHints" },
    { T("endg"), T("shrd"), "EndGroup" },
};
enum { NBUILTIN = sizeof builtin / sizeof builtin[0] };

static q3class **registered;
static int nregistered;

q3class *q3_class(uint32_t type)
{
    for (int i = 0; i < NBUILTIN; i++)
        if (builtin[i].type == type)
            return &builtin[i];
    for (int i = 0; i < nregistered; i++)
        if (registered[i]->type == type)
            return registered[i];
    return NULL;
}

q3class *q3_class_named(const char *name)
{
    for (int i = 0; i < NBUILTIN; i++)
        if (!strcmp(builtin[i].name, name))
            return &builtin[i];
    for (int i = 0; i < nregistered; i++)
        if (!strcmp(registered[i]->name, name))
            return registered[i];
    return NULL;
}

void q3_class_ops(uint32_t type, void (*dispose)(q3obj *), bool (*duplicate)(q3obj *, const q3obj *),
                  void (*submit)(q3obj *, q3view *), bool drawable)
{
    q3class *k = q3_class(type);
    if (!k)
        return;
    k->dispose = dispose;
    k->duplicate = duplicate;
    k->submit = submit;
    k->drawable = drawable;
}

void q3_class_submit(uint32_t type, void (*submit)(q3obj *, q3view *))
{
    q3class *k = q3_class(type);
    if (k)
        k->submit = submit;
}

bool q3_type_is(uint32_t leaf, uint32_t type)
{
    for (int depth = 0; leaf && depth < 32; depth++) {
        if (leaf == type)
            return true;
        q3class *k = q3_class(leaf);
        if (!k)
            return false;
        leaf = k->parent;
    }
    return false;
}

uint32_t q3_type_below(uint32_t leaf, uint32_t above)
{
    for (int depth = 0; leaf && depth < 32; depth++) {
        q3class *k = q3_class(leaf);
        if (!k)
            return 0;
        if (k->parent == above)
            return leaf;
        leaf = k->parent;
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Objects */

static int initialized;
static int32_t err_first, err_last, warn_first, warn_last, note_first, note_last;
static int16_t mac_first, mac_last;
static gaddr err_method, warn_method, note_method;
static uint32_t err_ref, warn_ref, note_ref;
static bool classes_ready;

bool q3_initialized(void) { return initialized > 0; }

static void ready_classes(void);

q3obj *q3_new(uint32_t type, void *data)
{
    ready_classes();
    q3class *k = q3_class(type);
    q3obj *o = calloc(1, sizeof *o);
    if (!o || !k) {
        free(o);
        q3_error(k ? kQ3ErrorOutOfMemory : kQ3ErrorInvalidObjectClass);
        return NULL;
    }
    o->g = q3_handle_new(Q3H_OBJECT, o);
    if (!o->g) {
        free(o);
        q3_error(kQ3ErrorOutOfMemory);
        return NULL;
    }
    o->type = type;
    o->cls = k;
    o->refs = 1;
    o->edit = 1;
    o->data = data;
    return o;
}

q3obj *q3_get(gaddr g) { return q3_handle_get(g, Q3H_OBJECT); }

q3obj *q3_of(gaddr g, uint32_t type)
{
    q3obj *o = q3_get(g);
    if (!o) {
        q3_error(g ? kQ3ErrorInvalidObject : kQ3ErrorNULLParameter);
        return NULL;
    }
    if (type && !q3_type_is(o->type, type)) {
        q3_error(kQ3ErrorInvalidObjectType);
        return NULL;
    }
    return o;
}

bool q3_is_shared(const q3obj *o) { return q3_type_is(o->type, Q3_SHARED); }

gaddr q3_ref(gaddr g)
{
    q3obj *o = q3_get(g);
    if (!o)
        return 0;
    if (q3_is_shared(o)) {
        o->refs++;
        return g;
    }
    return q3_duplicate(g);
}

static void call_delete(q3obj *o);

static void destroy(q3obj *o)
{
    call_delete(o);
    if (o->cls->dispose)
        o->cls->dispose(o);
    gaddr set = o->set;
    o->set = 0;
    if (o->instance)
        tb_DisposePtr(o->instance);
    q3_handle_free(o->g);
    free(o);
    if (set)
        q3_release(set);
}

void q3_release(gaddr g)
{
    q3obj *o = q3_get(g);
    if (!o)
        return;
    if (q3_is_shared(o) && --o->refs > 0)
        return;
    destroy(o);
}

void q3_assign(gaddr *slot, gaddr g)
{
    gaddr old = *slot;
    *slot = g ? q3_ref(g) : 0;
    if (old)
        q3_release(old);
}

static void call_duplicate(q3obj *from, q3obj *to);

gaddr q3_duplicate(gaddr g)
{
    q3obj *o = q3_get(g);
    if (!o)
        return 0;
    q3obj *n = q3_new(o->type, NULL);
    if (!n)
        return 0;
    if (o->cls->duplicate && !o->cls->duplicate(n, o)) {
        q3_handle_free(n->g);
        free(n);
        q3_error(kQ3ErrorOutOfMemory);
        return 0;
    }
    if (o->set)
        n->set = q3_duplicate(o->set);
    call_duplicate(o, n);
    return n->g;
}

void q3_edited(q3obj *o)
{
    if (o)
        o->edit++;
}

/* ---------------------------------------------------------------------- */
/* Errors */

static void post(gaddr method, uint32_t ref, int32_t first, int32_t last)
{
    if (method) {
        uint32_t args[3] = { (uint32_t)first, (uint32_t)last, ref };
        ppc_call(method, args, 3);
    }
}

void q3_error(int32_t e)
{
    if (!err_first)
        err_first = e;
    err_last = e;
    post(err_method, err_ref, err_first, e);
}

void q3_warning(int32_t w)
{
    if (!warn_first)
        warn_first = w;
    warn_last = w;
    post(warn_method, warn_ref, warn_first, w);
}

void q3_notice(int32_t n)
{
    if (!note_first)
        note_first = n;
    note_last = n;
    post(note_method, note_ref, note_first, n);
}

static void Q3Error_Register(ppc_cpu *c)
{
    err_method = Q3R(0);
    err_ref = Q3R(1);
    Q3RETURN(kQ3Success);
}
static void Q3Warning_Register(ppc_cpu *c)
{
    warn_method = Q3R(0);
    warn_ref = Q3R(1);
    Q3RETURN(kQ3Success);
}
static void Q3Notice_Register(ppc_cpu *c)
{
    note_method = Q3R(0);
    note_ref = Q3R(1);
    Q3RETURN(kQ3Success);
}

static void Q3Error_Get(ppc_cpu *c)
{
    if (Q3R(0))
        wr32(Q3R(0), (uint32_t)err_first);
    int32_t e = err_last;
    err_first = err_last = 0;
    Q3RETURN(e);
}
static void Q3Warning_Get(ppc_cpu *c)
{
    if (Q3R(0))
        wr32(Q3R(0), (uint32_t)warn_first);
    int32_t w = warn_last;
    warn_first = warn_last = 0;
    Q3RETURN(w);
}
static void Q3Notice_Get(ppc_cpu *c)
{
    if (Q3R(0))
        wr32(Q3R(0), (uint32_t)note_first);
    int32_t n = note_last;
    note_first = note_last = 0;
    Q3RETURN(n);
}
static void Q3MacintoshError_Get(ppc_cpu *c)
{
    if (Q3R(0))
        wr16(Q3R(0), (uint16_t)mac_first);
    int16_t e = mac_last;
    mac_first = mac_last = 0;
    Q3RETURN((int32_t)e);
}
static void Q3Error_IsFatalError(ppc_cpu *c)
{
    int32_t e = (int32_t)Q3R(0);
    /* kQ3ErrorInternalError through kQ3ErrorLastFatalError */
    Q3RETURN(e >= -28500 && e <= -28498);
}
static void Q3XError_Post(ppc_cpu *c)
{
    q3_error((int32_t)Q3R(0));
    Q3RETURN(0);
}
static void Q3XWarning_Post(ppc_cpu *c)
{
    q3_warning((int32_t)Q3R(0));
    Q3RETURN(0);
}
static void Q3XNotice_Post(ppc_cpu *c)
{
    q3_notice((int32_t)Q3R(0));
    Q3RETURN(0);
}
static void Q3XMacintoshError_Post(ppc_cpu *c)
{
    int16_t e = (int16_t)Q3R(0);
    if (!mac_first)
        mac_first = e;
    mac_last = e;
    q3_error(-28485); /* kQ3ErrorMacintoshError */
    Q3RETURN(0);
}

/* ---------------------------------------------------------------------- */
/* The library */

static void object_classes(void);
void q3_controller_classes(void);

static void ready_classes(void)
{
    if (classes_ready)
        return;
    classes_ready = true;
    object_classes();
    q3_geom_classes();
    q3_view_classes();
    q3_file_classes();
    q3_controller_classes();
}

/* Plug-ins in the Extensions folder (files of type 'shlb', creator
 * 'Q3XT'), loaded once, as QuickDraw 3D loaded them at its first
 * Q3Initialize: each registers its classes through
 * Q3XSharedLibrary_Register.  Apple's own two (the interactive renderer
 * and Custom Elements) are QuickDraw 3D's here already. */
static void load_plugins(void)
{
    static bool loaded;
    char dir[1024];
    if (loaded || fm_find_folder(Q3T('e', 'x', 't', 'n'), false, dir, sizeof dir) != 0)
        return;
    loaded = true;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || strstr(e->d_name, "QuickDraw\xE2\x84\xA2 3D IR") ||
            !strcmp(e->d_name, "QD3DCustomElements"))
            continue;
        char path[2048], err[256];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        mac_file mf;
        memset(&mf, 0, sizeof mf);
        if (!appledouble_read_meta(path, &mf))
            continue;
        bool plugin = mf.has_finder_info && !memcmp(mf.finder_info, "shlbQ3XT", 8);
        mac_file_free(&mf);
        if (plugin && !cfm_load_library_file(path, err, sizeof err))
            LOGF(LOG_TRAPS, "  QuickDraw 3D plug-in %s: %s\n", e->d_name, err);
    }
    if (d)
        closedir(d);
}

static void Q3Initialize(ppc_cpu *c)
{
    ready_classes();
    if (initialized++)
        q3_notice(kQ3NoticeSystemAlreadyInitialized);
    else
        load_plugins();
    Q3RETURN(kQ3Success);
}

static void Q3Exit(ppc_cpu *c)
{
    if (initialized > 0)
        initialized--;
    Q3RETURN(kQ3Success);
}

static void Q3IsInitialized(ppc_cpu *c) { Q3RETURN(initialized > 0); }

static void Q3GetVersion(ppc_cpu *c)
{
    if (Q3R(0))
        wr32(Q3R(0), 1);
    if (Q3R(1))
        wr32(Q3R(1), 6);
    Q3RETURN(kQ3Success);
}

static void Q3GetReleaseVersion(ppc_cpu *c)
{
    if (Q3R(0))
        wr32(Q3R(0), 0x01608000); /* 1.6, final */
    Q3RETURN(kQ3Success);
}

void q3_gstr(gaddr s, char *out, size_t size)
{
    size_t i = 0;
    for (; s && i + 1 < size && rd8(s + (uint32_t)i); i++)
        out[i] = (char)rd8(s + (uint32_t)i);
    out[i] = 0;
}

static void Q3ObjectHierarchy_GetTypeFromString(ppc_cpu *c)
{
    char name[256];
    q3_gstr(Q3R(0), name, sizeof name);
    q3class *k = q3_class_named(name);
    if (Q3R(1))
        wr32(Q3R(1), k ? k->type : 0);
    if (!k)
        q3_error(kQ3ErrorInvalidObjectName);
    Q3RETURN(k ? kQ3Success : kQ3Failure);
}

static void Q3ObjectHierarchy_GetStringFromType(ppc_cpu *c)
{
    q3class *k = q3_class(Q3R(0));
    if (!k || !Q3R(1)) {
        if (Q3R(1))
            wr8(Q3R(1), 0);
        q3_error(kQ3ErrorInvalidObjectClass);
        Q3RETURN(kQ3Failure);
        return;
    }
    gput(Q3R(1), k->name, (uint32_t)strlen(k->name) + 1);
    Q3RETURN(kQ3Success);
}

static void Q3ObjectHierarchy_IsTypeRegistered(ppc_cpu *c) { Q3RETURN(q3_class(Q3R(0)) != NULL); }

static void Q3ObjectHierarchy_IsNameRegistered(ppc_cpu *c)
{
    char name[256];
    q3_gstr(Q3R(0), name, sizeof name);
    Q3RETURN(q3_class_named(name) != NULL);
}

static void Q3ObjectHierarchy_GetSubClassData(ppc_cpu *c)
{
    uint32_t parent = Q3R(0);
    gaddr out = Q3R(1);
    if (!out || !q3_class(parent)) {
        q3_error(out ? kQ3ErrorInvalidObjectClass : kQ3ErrorNULLParameter);
        Q3RETURN(kQ3Failure);
        return;
    }
    uint32_t types[256], n = 0;
    for (int i = 0; i < NBUILTIN && n < 256; i++)
        if (builtin[i].parent == parent)
            types[n++] = builtin[i].type;
    for (int i = 0; i < nregistered && n < 256; i++)
        if (registered[i]->parent == parent)
            types[n++] = registered[i]->type;
    gaddr arr = n ? q3_galloc(n * 4) : 0;
    for (uint32_t i = 0; i < n && arr; i++)
        wr32(arr + i * 4, types[i]);
    wr32(out, arr ? n : 0);
    wr32(out + 4, arr);
    Q3RETURN(kQ3Success);
}

static void Q3ObjectHierarchy_EmptySubClassData(ppc_cpu *c)
{
    gaddr d = Q3R(0);
    if (d) {
        q3_gfree(rd32(d + 4));
        wr32(d, 0);
        wr32(d + 4, 0);
    }
    Q3RETURN(kQ3Success);
}

gaddr q3_galloc(uint32_t size)
{
    gaddr p = tb_NewPtr((int32_t)(size ? size : 4), false, true);
    if (!p)
        p = tb_NewPtr((int32_t)(size ? size : 4), true, true);
    if (!p)
        q3_error(kQ3ErrorOutOfMemory);
    return p;
}

void q3_gfree(gaddr p)
{
    if (p)
        tb_DisposePtr(p);
}

static void Q3Object_Dispose(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), 0);
    if (o)
        q3_release(o->g);
    Q3RETURN(o ? kQ3Success : kQ3Failure);
}

static void Q3Object_Duplicate(ppc_cpu *c) { Q3RETURN(q3_of(Q3R(0), 0) ? q3_duplicate(Q3R(0)) : 0); }

static void Q3Object_IsDrawable(ppc_cpu *c)
{
    q3obj *o = q3_get(Q3R(0));
    Q3RETURN(o && o->cls->drawable);
}

static void Q3Object_IsWritable(ppc_cpu *c)
{
    q3obj *o = q3_get(Q3R(0));
    Q3RETURN(o && q3_type_is(o->type, Q3_SHARED) && !q3_type_is(o->type, Q3_DRAW_CONTEXT) &&
             !q3_type_is(o->type, Q3_STORAGE) && !q3_type_is(o->type, Q3_FILE));
}

static void Q3Object_GetType(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), 0);
    Q3RETURN(o ? q3_type_below(o->type, 0) : 0);
}

static void Q3Object_GetLeafType(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), 0);
    Q3RETURN(o ? o->type : 0);
}

static void Q3Object_IsType(ppc_cpu *c)
{
    q3obj *o = q3_get(Q3R(0));
    Q3RETURN(o && q3_type_is(o->type, Q3R(1)));
}

static void Q3Shared_GetType(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_SHARED);
    Q3RETURN(o ? q3_type_below(o->type, Q3_SHARED) : 0);
}

static void Q3Shared_GetReference(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_SHARED);
    Q3RETURN(o ? q3_ref(o->g) : 0);
}

static void Q3Shared_IsReferenced(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_SHARED);
    Q3RETURN(o && o->refs > 1);
}

static void Q3Shared_GetEditIndex(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_SHARED);
    Q3RETURN(o ? o->edit : 0);
}

static void Q3Shared_Edited(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_SHARED);
    q3_edited(o);
    Q3RETURN(o ? kQ3Success : kQ3Failure);
}

static void Q3Shape_GetType(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_SHAPE);
    Q3RETURN(o ? q3_type_below(o->type, Q3_SHAPE) : 0);
}

/* ---------------------------------------------------------------------- */
/* Elements and sets */

typedef struct {
    int32_t type;
    uint32_t size;
    char name[64];
    gaddr metahandler;
    gaddr guest; /* its TQ3XObjectClass */
    bool attribute;
} element_class;

static element_class *elements;
static int nelements;
static int32_t next_element = 1000;

int32_t q3_element_size(int32_t type)
{
    static const int8_t attr[] = { 0, 8, 8, 12, 4, 12, 12, 4, 12, 24, 4, 4 };
    if (type > 0 && type < kQ3AttributeTypeNumTypes)
        return attr[type];
    for (int i = 0; i < nelements; i++)
        if (elements[i].type == type)
            return (int32_t)elements[i].size;
    return -1;
}

const q3elem *q3set_find(const q3set *s, int32_t type)
{
    for (int i = 0; s && i < s->n; i++)
        if (s->e[i].type == type)
            return &s->e[i];
    return NULL;
}

static void elem_free(q3elem *e)
{
    if (e->type == kQ3AttributeTypeSurfaceShader && e->size >= 4) {
        gaddr sh = (uint32_t)e->bytes[0] << 24 | (uint32_t)e->bytes[1] << 16 | (uint32_t)e->bytes[2] << 8 | e->bytes[3];
        q3_release(sh);
    }
    free(e->bytes);
    if (e->g)
        tb_DisposePtr(e->g);
    e->g = 0;
}

bool q3set_add_bytes(q3set *s, int32_t type, const uint8_t *bytes, uint32_t size)
{
    uint8_t *copy = malloc(size ? size : 1);
    if (!copy)
        return false;
    memcpy(copy, bytes, size);
    if (type == kQ3AttributeTypeSurfaceShader && size >= 4) {
        gaddr sh = (uint32_t)copy[0] << 24 | (uint32_t)copy[1] << 16 | (uint32_t)copy[2] << 8 | copy[3];
        q3_ref(sh);
    }
    for (int i = 0; i < s->n; i++)
        if (s->e[i].type == type) {
            elem_free(&s->e[i]);
            s->e[i].bytes = copy;
            s->e[i].size = size;
            return true;
        }
    q3elem *n = realloc(s->e, (size_t)(s->n + 1) * sizeof *n);
    if (!n) {
        free(copy);
        return false;
    }
    s->e = n;
    s->e[s->n++] = (q3elem){ type, size, copy, 0 };
    return true;
}

bool q3set_add(q3set *s, int32_t type, gaddr data)
{
    int32_t size = q3_element_size(type);
    if (size < 0 || !data)
        return false;
    return q3set_add_bytes(s, type, gptr(data), (uint32_t)size);
}

void q3set_clear(q3set *s, int32_t type)
{
    for (int i = 0; i < s->n; i++)
        if (s->e[i].type == type) {
            elem_free(&s->e[i]);
            memmove(&s->e[i], &s->e[i + 1], (size_t)(s->n - i - 1) * sizeof s->e[0]);
            s->n--;
            return;
        }
}

void q3set_empty(q3set *s)
{
    for (int i = 0; i < s->n; i++)
        elem_free(&s->e[i]);
    free(s->e);
    s->e = NULL;
    s->n = 0;
}

void q3set_copy(q3set *to, const q3set *from)
{
    for (int i = 0; from && i < from->n; i++)
        q3set_add_bytes(to, from->e[i].type, from->e[i].bytes, from->e[i].size);
}

q3set *q3_set_of(gaddr g)
{
    q3obj *o = q3_get(g);
    return o && q3_type_is(o->type, Q3_SET) ? o->data : NULL;
}

static uint32_t be(const uint8_t *b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }
static float bef(const uint8_t *b)
{
    uint32_t u = be(b);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

bool q3_attr_color(gaddr set, int32_t type, float rgb[3])
{
    const q3elem *e = q3set_find(q3_set_of(set), type);
    if (!e || e->size < 12)
        return false;
    for (int i = 0; i < 3; i++)
        rgb[i] = bef(e->bytes + 4 * i);
    return true;
}
bool q3_attr_vector(gaddr set, int32_t type, float v[3]) { return q3_attr_color(set, type, v); }

bool q3_attr_float(gaddr set, int32_t type, float *f)
{
    const q3elem *e = q3set_find(q3_set_of(set), type);
    if (!e || e->size < 4)
        return false;
    *f = bef(e->bytes);
    return true;
}

bool q3_attr_uv(gaddr set, int32_t type, float uv[2])
{
    const q3elem *e = q3set_find(q3_set_of(set), type);
    if (!e || e->size < 8)
        return false;
    uv[0] = bef(e->bytes);
    uv[1] = bef(e->bytes + 4);
    return true;
}

gaddr q3_attr_shader(gaddr set)
{
    const q3elem *e = q3set_find(q3_set_of(set), kQ3AttributeTypeSurfaceShader);
    return e && e->size >= 4 ? be(e->bytes) : 0;
}

static void set_dispose(q3obj *o)
{
    q3set *s = o->data;
    if (s) {
        q3set_empty(s);
        free(s);
    }
}

static bool set_duplicate(q3obj *to, const q3obj *from)
{
    q3set *s = calloc(1, sizeof *s);
    if (!s)
        return false;
    q3set_copy(s, from->data);
    to->data = s;
    return true;
}

static gaddr new_set(uint32_t type)
{
    q3set *s = calloc(1, sizeof *s);
    q3obj *o = s ? q3_new(type, s) : NULL;
    if (!o)
        free(s);
    return o ? o->g : 0;
}

static q3set *set_arg(gaddr g, uint32_t type)
{
    q3obj *o = q3_of(g, type);
    return o ? o->data : NULL;
}

/* An element's value out to the program: a surface shader as a new
 * reference. */
static void elem_out(const q3elem *e, gaddr data)
{
    gput(data, e->bytes, e->size);
    if (e->type == kQ3AttributeTypeSurfaceShader && e->size >= 4)
        q3_ref(be(e->bytes));
}

static void set_add(ppc_cpu *c, uint32_t type)
{
    q3obj *o = q3_of(Q3R(0), type);
    q3set *s = o ? o->data : NULL;
    int32_t t = (int32_t)Q3R(1);
    if (s && q3_element_size(t) < 0)
        q3_error(kQ3ErrorUnknownElementType);
    bool ok = s && Q3R(2) && q3set_add(s, t, Q3R(2));
    if (ok)
        q3_edited(o);
    Q3RETURN(ok ? kQ3Success : kQ3Failure);
}

static void set_get(ppc_cpu *c, uint32_t type)
{
    q3set *s = set_arg(Q3R(0), type);
    const q3elem *e = q3set_find(s, (int32_t)Q3R(1));
    if (s && !e)
        q3_error(kQ3ErrorAttributeNotContained);
    if (e && Q3R(2))
        elem_out(e, Q3R(2));
    Q3RETURN(e ? kQ3Success : kQ3Failure);
}

static void set_contains(ppc_cpu *c, uint32_t type)
{
    q3set *s = set_arg(Q3R(0), type);
    Q3RETURN(q3set_find(s, (int32_t)Q3R(1)) != NULL);
}

static void set_clear(ppc_cpu *c, uint32_t type)
{
    q3obj *o = q3_of(Q3R(0), type);
    if (o) {
        q3set_clear(o->data, (int32_t)Q3R(1));
        q3_edited(o);
    }
    Q3RETURN(o ? kQ3Success : kQ3Failure);
}

static void set_empty(ppc_cpu *c, uint32_t type)
{
    q3obj *o = q3_of(Q3R(0), type);
    if (o) {
        q3set_empty(o->data);
        q3_edited(o);
    }
    Q3RETURN(o ? kQ3Success : kQ3Failure);
}

/* The next element type after *type (kQ3ElementTypeNone: the first), or
 * kQ3ElementTypeNone after the last. */
static void set_next(ppc_cpu *c, uint32_t type)
{
    q3set *s = set_arg(Q3R(0), type);
    gaddr t = Q3R(1);
    if (!s || !t) {
        Q3RETURN(kQ3Failure);
        return;
    }
    int32_t cur = (int32_t)rd32(t), next = 0;
    if (!cur) {
        next = s->n ? s->e[0].type : 0;
    } else {
        for (int i = 0; i < s->n; i++)
            if (s->e[i].type == cur) {
                next = i + 1 < s->n ? s->e[i + 1].type : 0;
                break;
            }
    }
    wr32(t, (uint32_t)next);
    Q3RETURN(kQ3Success);
}

static void Q3Set_New(ppc_cpu *c) { Q3RETURN(new_set(Q3_SET)); }
static void Q3Set_GetType(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_SET);
    Q3RETURN(o ? q3_type_below(o->type, Q3_SET) : 0);
}
static void Q3Set_Add(ppc_cpu *c) { set_add(c, Q3_SET); }
static void Q3Set_Get(ppc_cpu *c) { set_get(c, Q3_SET); }
static void Q3Set_Contains(ppc_cpu *c) { set_contains(c, Q3_SET); }
static void Q3Set_Clear(ppc_cpu *c) { set_clear(c, Q3_SET); }
static void Q3Set_Empty(ppc_cpu *c) { set_empty(c, Q3_SET); }
static void Q3Set_GetNextElementType(ppc_cpu *c) { set_next(c, Q3_SET); }
static void Q3AttributeSet_New(ppc_cpu *c) { Q3RETURN(new_set(Q3_ATTRIBUTE_SET)); }
static void Q3AttributeSet_Add(ppc_cpu *c) { set_add(c, Q3_ATTRIBUTE_SET); }
static void Q3AttributeSet_Get(ppc_cpu *c) { set_get(c, Q3_ATTRIBUTE_SET); }
static void Q3AttributeSet_Contains(ppc_cpu *c) { set_contains(c, Q3_ATTRIBUTE_SET); }
static void Q3AttributeSet_Clear(ppc_cpu *c) { set_clear(c, Q3_ATTRIBUTE_SET); }
static void Q3AttributeSet_Empty(ppc_cpu *c) { set_empty(c, Q3_ATTRIBUTE_SET); }
static void Q3AttributeSet_GetNextAttributeType(ppc_cpu *c) { set_next(c, Q3_ATTRIBUTE_SET); }

/* result gets the parent's attributes, then the child's over them. */
static void Q3AttributeSet_Inherit(ppc_cpu *c)
{
    q3set *p = set_arg(Q3R(0), Q3_ATTRIBUTE_SET), *ch = set_arg(Q3R(1), Q3_ATTRIBUTE_SET);
    q3obj *r = q3_of(Q3R(2), Q3_ATTRIBUTE_SET);
    if (!p || !ch || !r) {
        Q3RETURN(kQ3Failure);
        return;
    }
    q3set merged = { 0, NULL };
    q3set_copy(&merged, p);
    q3set_copy(&merged, ch);
    q3set_empty(r->data);
    *(q3set *)r->data = merged;
    q3_edited(r);
    Q3RETURN(kQ3Success);
}

/* A shape's set, made on first use. */
static q3set *shape_set(q3obj *o, bool make)
{
    if (!o->set && make)
        o->set = new_set(Q3_SET);
    return q3_set_of(o->set);
}

static void Q3Shape_GetSet(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_SHAPE);
    if (o && Q3R(1))
        wr32(Q3R(1), o->set ? q3_ref(o->set) : 0);
    Q3RETURN(o ? kQ3Success : kQ3Failure);
}

static void Q3Shape_SetSet(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_SHAPE);
    if (o) {
        q3_assign(&o->set, Q3R(1));
        q3_edited(o);
    }
    Q3RETURN(o ? kQ3Success : kQ3Failure);
}

static void Q3Shape_AddElement(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_SHAPE);
    q3set *s = o ? shape_set(o, true) : NULL;
    bool ok = s && q3set_add(s, (int32_t)Q3R(1), Q3R(2));
    if (o && !ok)
        q3_error(kQ3ErrorUnknownElementType);
    Q3RETURN(ok ? kQ3Success : kQ3Failure);
}

static void Q3Shape_GetElement(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_SHAPE);
    const q3elem *e = o ? q3set_find(shape_set(o, false), (int32_t)Q3R(1)) : NULL;
    if (e && Q3R(2))
        elem_out(e, Q3R(2));
    Q3RETURN(e ? kQ3Success : kQ3Failure);
}

static void Q3Shape_ContainsElement(ppc_cpu *c)
{
    q3obj *o = q3_get(Q3R(0));
    Q3RETURN(o && q3set_find(shape_set(o, false), (int32_t)Q3R(1)) != NULL);
}

static void Q3Shape_GetNextElementType(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_SHAPE);
    if (!o) {
        Q3RETURN(kQ3Failure);
        return;
    }
    if (!o->set) {
        if (Q3R(1))
            wr32(Q3R(1), 0);
        Q3RETURN(kQ3Success);
        return;
    }
    c->gpr[3] = o->set;
    set_next(c, Q3_SET);
}

static void Q3Shape_EmptyElements(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_SHAPE);
    if (o && o->set)
        q3set_empty(shape_set(o, false));
    Q3RETURN(o ? kQ3Success : kQ3Failure);
}

static void Q3Shape_ClearElement(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_SHAPE);
    if (o && o->set)
        q3set_clear(shape_set(o, false), (int32_t)Q3R(1));
    Q3RETURN(o ? kQ3Success : kQ3Failure);
}

static void Q3Bitmap_Empty(ppc_cpu *c)
{
    gaddr b = Q3R(0);
    if (b) {
        q3_gfree(rd32(b));
        wr32(b, 0);
    }
    Q3RETURN(b ? kQ3Success : kQ3Failure);
}

static void Q3Bitmap_GetImageSize(ppc_cpu *c) { Q3RETURN(((Q3R(0) + 7) / 8) * Q3R(1)); }

static gaddr new_element_class(int32_t *type, const char *name, uint32_t size, gaddr meta, bool attribute)
{
    element_class *n = realloc(elements, (size_t)(nelements + 1) * sizeof *n);
    if (!n)
        return 0;
    elements = n;
    element_class *e = &elements[nelements++];
    memset(e, 0, sizeof *e);
    e->type = next_element++;
    e->size = size;
    snprintf(e->name, sizeof e->name, "%s", name);
    e->metahandler = meta;
    e->attribute = attribute;
    e->guest = q3_handle_new(Q3H_CLASS, NULL);
    *type = e->type;
    return e->guest;
}

int32_t q3_register_element(const char *name, uint32_t size)
{
    for (int i = 0; i < nelements; i++)
        if (!strcmp(elements[i].name, name))
            return elements[i].type;
    int32_t type = 0;
    new_element_class(&type, name, size, 0, false);
    return type;
}

/* A registered element class: by name (its type and metahandler) or by
 * type (its name and metahandler).  False if there is none. */
bool q3_element_class_named(const char *name, int32_t *type, gaddr *meta)
{
    for (int i = 0; i < nelements; i++)
        if (!strcmp(elements[i].name, name)) {
            *type = elements[i].type;
            *meta = elements[i].metahandler;
            return true;
        }
    return false;
}

bool q3_element_class_of(int32_t type, const char **name, gaddr *meta)
{
    for (int i = 0; i < nelements; i++)
        if (elements[i].type == type) {
            *name = elements[i].name;
            *meta = elements[i].metahandler;
            return true;
        }
    return false;
}

q3set *q3_elements_of(q3obj *o, bool make)
{
    if (q3_type_is(o->type, Q3_SET))
        return o->data;
    if (q3_type_is(o->type, Q3_SHAPE))
        return shape_set(o, make);
    return NULL;
}

static void register_element(ppc_cpu *c, bool attribute)
{
    char name[64];
    q3_gstr(Q3R(1), name, sizeof name);
    /* Registered again (a program run twice): the same type. */
    for (int i = 0; i < nelements; i++)
        if (!strcmp(elements[i].name, name)) {
            if (Q3R(0))
                wr32(Q3R(0), (uint32_t)elements[i].type);
            Q3RETURN(elements[i].guest);
            return;
        }
    int32_t type = 0;
    gaddr k = new_element_class(&type, name, Q3R(2), Q3R(3), attribute);
    if (Q3R(0))
        wr32(Q3R(0), (uint32_t)type);
    Q3RETURN(k);
}
static void Q3XElementClass_Register(ppc_cpu *c) { register_element(c, false); }
static void Q3XAttributeClass_Register(ppc_cpu *c) { register_element(c, true); }

static void Q3XElementType_GetElementSize(ppc_cpu *c)
{
    int32_t size = q3_element_size((int32_t)Q3R(0));
    if (Q3R(1))
        wr32(Q3R(1), size < 0 ? 0 : (uint32_t)size);
    Q3RETURN(size < 0 ? kQ3Failure : kQ3Success);
}

/* ---------------------------------------------------------------------- */
/* Strings */

static void string_dispose(q3obj *o) { free(o->data); }
static bool string_duplicate(q3obj *to, const q3obj *from)
{
    to->data = strdup(from->data ? from->data : "");
    return to->data != NULL;
}

static char *gstrdup(gaddr s)
{
    size_t n = 0;
    while (s && rd8(s + (uint32_t)n))
        n++;
    char *out = malloc(n + 1);
    if (out) {
        if (n)
            gget(out, s, (uint32_t)n);
        out[n] = 0;
    }
    return out;
}

static void Q3String_GetType(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_STRING);
    Q3RETURN(o ? q3_type_below(o->type, Q3_STRING) : 0);
}

static void Q3CString_New(ppc_cpu *c)
{
    char *s = gstrdup(Q3R(0));
    q3obj *o = s ? q3_new(Q3_CSTRING, s) : NULL;
    if (!o)
        free(s);
    Q3RETURN(o ? o->g : 0);
}

static void Q3CString_GetLength(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_CSTRING);
    if (o && Q3R(1))
        wr32(Q3R(1), (uint32_t)strlen(o->data));
    Q3RETURN(o ? kQ3Success : kQ3Failure);
}

static void Q3CString_SetString(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_CSTRING);
    char *s = o ? gstrdup(Q3R(1)) : NULL;
    if (s) {
        free(o->data);
        o->data = s;
        q3_edited(o);
    }
    Q3RETURN(s ? kQ3Success : kQ3Failure);
}

static void Q3CString_GetString(ppc_cpu *c)
{
    q3obj *o = q3_of(Q3R(0), Q3_CSTRING);
    if (!o || !Q3R(1)) {
        Q3RETURN(kQ3Failure);
        return;
    }
    uint32_t n = (uint32_t)strlen(o->data) + 1;
    gaddr s = q3_galloc(n);
    if (s)
        gput(s, o->data, n);
    wr32(Q3R(1), s);
    Q3RETURN(s ? kQ3Success : kQ3Failure);
}

static void Q3CString_EmptyData(ppc_cpu *c)
{
    if (Q3R(0)) {
        q3_gfree(rd32(Q3R(0)));
        wr32(Q3R(0), 0);
    }
    Q3RETURN(kQ3Success);
}

/* ---------------------------------------------------------------------- */
/* Classes programs register (QD3DExtension.h).  The metahandler gives
 * the methods; those called are a new object's (newo), its deletion's
 * (dlte) and its duplication's (dupl), with its private data. */

enum {
    kQ3XMethodTypeObjectNew = 0x6E65776F,       /* 'newo' */
    kQ3XMethodTypeObjectDelete = 0x646C7465,    /* 'dlte' */
    kQ3XMethodTypeObjectDuplicate = 0x6475706C, /* 'dupl' */
    kQ3XMethodTypeObjectClassVersion = 0x7672736E, /* 'vrsn' */
    kQ3XMethodTypeObjectClassRegister = 0x72677374, /* 'rgst' */
};

static gaddr method_of(const q3class *k, uint32_t method)
{
    for (int depth = 0; k && depth < 32; depth++) {
        if (k->registered && k->metahandler) {
            uint32_t arg = method;
            gaddr fn = ppc_call(k->metahandler, &arg, 1);
            if (fn)
                return fn;
        }
        k = k->parent ? q3_class(k->parent) : NULL;
    }
    return 0;
}

gaddr q3_class_method(const q3class *k, uint32_t method) { return method_of(k, method); }

static q3class *class_of_handle(gaddr g)
{
    for (int i = 0; i < nregistered; i++)
        if (registered[i]->guest == g)
            return registered[i];
    for (int i = 0; g && i < NBUILTIN; i++)
        if (builtin[i].guest == g)
            return &builtin[i];
    return NULL;
}

static gaddr class_handle(q3class *k)
{
    if (k && !k->guest)
        k->guest = q3_handle_new(Q3H_CLASS, k);
    return k ? k->guest : 0;
}

static void call_delete(q3obj *o)
{
    if (!o->instance && !o->cls->registered)
        return;
    gaddr fn = method_of(o->cls, kQ3XMethodTypeObjectDelete);
    if (fn) {
        uint32_t args[2] = { o->g, o->instance };
        ppc_call(fn, args, 2);
    }
}

static void call_duplicate(q3obj *from, q3obj *to)
{
    if (!from->cls->registered)
        return;
    if (from->cls->instance_size) {
        to->instance = tb_NewPtr((int32_t)from->cls->instance_size, false, true);
        if (to->instance && from->instance)
            gmemcpy(to->instance, from->instance, from->cls->instance_size);
    }
    gaddr fn = method_of(from->cls, kQ3XMethodTypeObjectDuplicate);
    if (fn) {
        uint32_t args[4] = { from->g, from->instance, to->g, to->instance };
        ppc_call(fn, args, 4);
    }
}

/* A class registered (QD3DExtension.h's arguments from r3 on): with a
 * new type, or with its own (the libraries' private ByType call). */
static void register_class(ppc_cpu *c, bool by_type)
{
    uint32_t parent = Q3R(0);
    gaddr type_out = by_type ? 0 : Q3R(1);
    char name[256];
    q3_gstr(Q3R(2), name, sizeof name);
    q3class *p = q3_class(parent);
    if (!p || !name[0]) {
        q3_error(p ? kQ3ErrorInvalidObjectName : kQ3ErrorInvalidObjectClass);
        Q3RETURN(0);
        return;
    }
    q3class *old = q3_class_named(name);
    if (old && old->registered) { /* the same class again (the program run twice) */
        if (type_out)
            wr32(type_out, old->type);
        Q3RETURN(class_handle(old));
        return;
    }
    /* A type ours already has stays ours: Apple's interactive renderer,
     * brought as a plug-in, gives way to the one built in, quietly (an
     * error posted would be the program's next Q3Error_Get: Bugdom took
     * it for its model files failing). */
    if (by_type && q3_class(Q3R(1))) {
        Q3RETURN(0);
        return;
    }
    if (old) {
        q3_error(kQ3ErrorInvalidObjectClass);
        Q3RETURN(0);
        return;
    }
    q3class *k = calloc(1, sizeof *k);
    q3class **n = k ? realloc(registered, (size_t)(nregistered + 1) * sizeof *n) : NULL;
    if (!n) {
        free(k);
        q3_error(kQ3ErrorOutOfMemory);
        Q3RETURN(0);
        return;
    }
    registered = n;
    registered[nregistered++] = k;
    static uint32_t next_type = 0xFFFFFFFFu; /* -1 down, as QuickDraw 3D numbered them */
    k->type = by_type ? Q3R(1) : next_type--;
    k->parent = parent;
    k->own_name = strdup(name);
    k->name = k->own_name;
    k->registered = true;
    k->metahandler = Q3R(3);
    k->virtual_metahandler = Q3R(4);
    k->instance_size = Q3R(6);
    /* A subclass draws, disposes and copies as its parent class does. */
    k->dispose = p->dispose;
    k->duplicate = p->duplicate;
    k->submit = p->submit;
    k->drawable = p->drawable;
    if (type_out)
        wr32(type_out, k->type);
    gaddr reg = method_of(k, kQ3XMethodTypeObjectClassRegister);
    if (reg) {
        uint32_t args[2] = { class_handle(k), 0 };
        ppc_call(reg, args, 2);
    }
    Q3RETURN(class_handle(k));
}

static void Q3XObjectHierarchy_RegisterClass(ppc_cpu *c) { register_class(c, false); }
static void EiObjectHierarchy_RegisterClassByType(ppc_cpu *c) { register_class(c, true); }

/* The error method and its reference, for the libraries' own use. */
static void EiErrorClient_GetPostMethod(ppc_cpu *c)
{
    if (Q3R(0))
        wr32(Q3R(0), err_method);
    if (Q3R(1))
        wr32(Q3R(1), err_ref);
    Q3RETURN(kQ3Success);
}

static void Q3XObjectHierarchy_UnregisterClass(ppc_cpu *c)
{
    q3class *k = class_of_handle(Q3R(0));
    if (!k || !k->registered) {
        Q3RETURN(kQ3Failure);
        return;
    }
    for (int i = 0; i < nregistered; i++)
        if (registered[i] == k) {
            memmove(&registered[i], &registered[i + 1], (size_t)(nregistered - i - 1) * sizeof registered[0]);
            nregistered--;
            break;
        }
    q3_handle_free(k->guest);
    free(k->own_name);
    free(k);
    Q3RETURN(kQ3Success);
}

static void Q3XObjectClass_GetMethod(ppc_cpu *c) { Q3RETURN(method_of(class_of_handle(Q3R(0)), Q3R(1))); }

gaddr q3_registered_new(q3class *k, gaddr param)
{
    /* The object has its class's data, a subclass its parent's, as one
     * the class's own New made (Interiors makes its groups this way). */
    void *data = NULL;
    const q3schema *s = k ? q3_schema_of(k->type) : NULL;
    if (k && q3_type_is(k->type, Q3_GROUP))
        data = q3_group_data_new();
    else if (s)
        data = q3d_new(s, 1);
    q3obj *o = k ? q3_new(k->type, data) : NULL;
    if (!o) {
        if (data && s)
            q3d_free(data, s);
        else
            free(data);
        return 0;
    }
    o->cls = k;
    if (k->instance_size)
        o->instance = tb_NewPtr((int32_t)k->instance_size, false, true);
    gaddr fn = method_of(k, kQ3XMethodTypeObjectNew);
    if (fn) {
        uint32_t args[3] = { o->g, o->instance, param };
        if (ppc_call(fn, args, 3) != kQ3Success) {
            if (o->instance)
                tb_DisposePtr(o->instance);
            if (o->data && k->dispose)
                k->dispose(o);
            q3_handle_free(o->g);
            free(o);
            return 0;
        }
    }
    return o->g;
}

static void Q3XObjectHierarchy_NewObject(ppc_cpu *c) { Q3RETURN(q3_registered_new(class_of_handle(Q3R(0)), Q3R(1))); }

static void Q3XObjectClass_GetLeafType(ppc_cpu *c)
{
    q3class *k = class_of_handle(Q3R(0));
    Q3RETURN(k ? k->type : 0);
}

static void Q3XObjectClass_GetType(ppc_cpu *c)
{
    q3class *k = class_of_handle(Q3R(0));
    if (k && Q3R(1))
        wr32(Q3R(1), k->type);
    Q3RETURN(k ? kQ3Success : kQ3Failure);
}

static void Q3XObjectHierarchy_GetClassVersion(ppc_cpu *c)
{
    q3class *k = q3_class(Q3R(0));
    gaddr fn = k ? method_of(k, kQ3XMethodTypeObjectClassVersion) : 0;
    if (k && Q3R(1))
        wr32(Q3R(1), fn ? fn : 0x00010006); /* the version is the method's "pointer" */
    Q3RETURN(k ? kQ3Success : kQ3Failure);
}

static void Q3XObjectHierarchy_FindClassByType(ppc_cpu *c) { Q3RETURN(class_handle(q3_class(Q3R(0)))); }

static void Q3XObjectClass_GetPrivate(ppc_cpu *c)
{
    q3obj *o = q3_get(Q3R(1));
    Q3RETURN(o ? o->instance : 0);
}

static void Q3XObject_GetClass(ppc_cpu *c)
{
    q3obj *o = q3_get(Q3R(0));
    Q3RETURN(o ? class_handle(o->cls) : 0);
}

/* Extensions in shared libraries register their classes through this;
 * the program's own register function is called at once. */
static void Q3XSharedLibrary_Register(ppc_cpu *c)
{
    gaddr info = Q3R(0);
    if (info && rd32(info))
        ppc_call(rd32(info), NULL, 0);
    Q3RETURN(info ? kQ3Success : kQ3Failure);
}

static void Q3XSharedLibrary_Unregister(ppc_cpu *c) { Q3RETURN(kQ3Success); }

const ppc_export q3_exports[] = {
    { "Q3Initialize", Q3Initialize },
    { "Q3Exit", Q3Exit },
    { "Q3IsInitialized", Q3IsInitialized },
    { "Q3GetVersion", Q3GetVersion },
    { "Q3GetReleaseVersion", Q3GetReleaseVersion },
    { "Q3ObjectHierarchy_GetTypeFromString", Q3ObjectHierarchy_GetTypeFromString },
    { "Q3ObjectHierarchy_GetStringFromType", Q3ObjectHierarchy_GetStringFromType },
    { "Q3ObjectHierarchy_IsTypeRegistered", Q3ObjectHierarchy_IsTypeRegistered },
    { "Q3ObjectHierarchy_IsNameRegistered", Q3ObjectHierarchy_IsNameRegistered },
    { "Q3ObjectHierarchy_GetSubClassData", Q3ObjectHierarchy_GetSubClassData },
    { "Q3ObjectHierarchy_EmptySubClassData", Q3ObjectHierarchy_EmptySubClassData },
    { "Q3Object_Dispose", Q3Object_Dispose },
    { "Q3Object_Duplicate", Q3Object_Duplicate },
    { "Q3Object_IsDrawable", Q3Object_IsDrawable },
    { "Q3Object_IsWritable", Q3Object_IsWritable },
    { "Q3Object_GetType", Q3Object_GetType },
    { "Q3Object_GetLeafType", Q3Object_GetLeafType },
    { "Q3Object_IsType", Q3Object_IsType },
    { "Q3Shared_GetType", Q3Shared_GetType },
    { "Q3Shared_GetReference", Q3Shared_GetReference },
    { "Q3Shared_IsReferenced", Q3Shared_IsReferenced },
    { "Q3Shared_GetEditIndex", Q3Shared_GetEditIndex },
    { "Q3Shared_Edited", Q3Shared_Edited },
    { "Q3Shape_GetType", Q3Shape_GetType },
    { "Q3Shape_GetSet", Q3Shape_GetSet },
    { "Q3Shape_SetSet", Q3Shape_SetSet },
    { "Q3Shape_AddElement", Q3Shape_AddElement },
    { "Q3Shape_GetElement", Q3Shape_GetElement },
    { "Q3Shape_ContainsElement", Q3Shape_ContainsElement },
    { "Q3Shape_GetNextElementType", Q3Shape_GetNextElementType },
    { "Q3Shape_EmptyElements", Q3Shape_EmptyElements },
    { "Q3Shape_ClearElement", Q3Shape_ClearElement },
    { "Q3Bitmap_Empty", Q3Bitmap_Empty },
    { "Q3Bitmap_GetImageSize", Q3Bitmap_GetImageSize },
    { "Q3Error_Register", Q3Error_Register },
    { "Q3Warning_Register", Q3Warning_Register },
    { "Q3Notice_Register", Q3Notice_Register },
    { "Q3Error_Get", Q3Error_Get },
    { "Q3Error_IsFatalError", Q3Error_IsFatalError },
    { "Q3Warning_Get", Q3Warning_Get },
    { "Q3Notice_Get", Q3Notice_Get },
    { "Q3MacintoshError_Get", Q3MacintoshError_Get },
    { "Q3XError_Post", Q3XError_Post },
    { "Q3XWarning_Post", Q3XWarning_Post },
    { "Q3XNotice_Post", Q3XNotice_Post },
    { "Q3XMacintoshError_Post", Q3XMacintoshError_Post },
    { "Q3Set_New", Q3Set_New },
    { "Q3Set_GetType", Q3Set_GetType },
    { "Q3Set_Add", Q3Set_Add },
    { "Q3Set_Get", Q3Set_Get },
    { "Q3Set_Contains", Q3Set_Contains },
    { "Q3Set_Clear", Q3Set_Clear },
    { "Q3Set_Empty", Q3Set_Empty },
    { "Q3Set_GetNextElementType", Q3Set_GetNextElementType },
    { "Q3AttributeSet_New", Q3AttributeSet_New },
    { "Q3AttributeSet_Add", Q3AttributeSet_Add },
    { "Q3AttributeSet_Contains", Q3AttributeSet_Contains },
    { "Q3AttributeSet_Get", Q3AttributeSet_Get },
    { "Q3AttributeSet_Clear", Q3AttributeSet_Clear },
    { "Q3AttributeSet_Empty", Q3AttributeSet_Empty },
    { "Q3AttributeSet_GetNextAttributeType", Q3AttributeSet_GetNextAttributeType },
    { "Q3AttributeSet_Inherit", Q3AttributeSet_Inherit },
    { "Q3XElementClass_Register", Q3XElementClass_Register },
    { "Q3ElementClass_Register", Q3XElementClass_Register }, /* QuickDraw 3D 1.0's name */
    { "Q3XElementType_GetElementSize", Q3XElementType_GetElementSize },
    { "Q3XAttributeClass_Register", Q3XAttributeClass_Register },
    { "Q3String_GetType", Q3String_GetType },
    { "Q3CString_New", Q3CString_New },
    { "Q3CString_GetLength", Q3CString_GetLength },
    { "Q3CString_SetString", Q3CString_SetString },
    { "Q3CString_GetString", Q3CString_GetString },
    { "Q3CString_EmptyData", Q3CString_EmptyData },
    { "Q3XObjectHierarchy_RegisterClass", Q3XObjectHierarchy_RegisterClass },
    { "EiObjectHierarchy_RegisterClassByType", EiObjectHierarchy_RegisterClassByType },
    { "EiErrorClient_GetPostMethod", EiErrorClient_GetPostMethod },
    { "Q3XObjectHierarchy_UnregisterClass", Q3XObjectHierarchy_UnregisterClass },
    { "Q3XObjectClass_GetMethod", Q3XObjectClass_GetMethod },
    { "Q3XObjectHierarchy_NewObject", Q3XObjectHierarchy_NewObject },
    { "Q3XObjectClass_GetLeafType", Q3XObjectClass_GetLeafType },
    { "Q3XObjectHierarchy_GetClassVersion", Q3XObjectHierarchy_GetClassVersion },
    { "Q3XObjectClass_GetType", Q3XObjectClass_GetType },
    { "Q3XObjectHierarchy_FindClassByType", Q3XObjectHierarchy_FindClassByType },
    { "Q3XObjectClass_GetPrivate", Q3XObjectClass_GetPrivate },
    { "Q3XObject_GetClass", Q3XObject_GetClass },
    { "Q3XSharedLibrary_Register", Q3XSharedLibrary_Register },
    { "Q3XSharedLibrary_Unregister", Q3XSharedLibrary_Unregister },
};
const unsigned q3_export_count = sizeof q3_exports / sizeof q3_exports[0];

/* Set and string classes are this file's. */
static void object_classes(void)
{
    q3_class_ops(Q3_SET, set_dispose, set_duplicate, NULL, false);
    /* An attribute set is drawn as it changes the attributes in force:
     * display groups take it (Bugdom's MakeObjectTransparent). */
    q3_class_ops(Q3_ATTRIBUTE_SET, set_dispose, set_duplicate, NULL, true);
    q3_class_ops(Q3_CSTRING, string_dispose, string_duplicate, NULL, false);
}

/* A new machine: every handle and object went with guest memory. */
void q3_object_reset(void)
{
    for (int c = 0; c < nchunks; c++) {
        for (int i = 0; i < CHUNK; i++)
            if (chunk_slots[c][i].kind == Q3H_OBJECT)
                free(chunk_slots[c][i].p); /* the record; what it held goes unfreed */
        free(chunk_slots[c]);
    }
    nchunks = 0;
    nfree = 0;
    for (int i = 0; i < nregistered; i++) {
        free(registered[i]->own_name);
        free(registered[i]);
    }
    nregistered = 0;
    for (int i = 0; i < NBUILTIN; i++)
        builtin[i].guest = 0;
    free(elements);
    elements = NULL;
    nelements = 0;
    next_element = 1000;
    initialized = 0;
    classes_ready = false;
    err_first = err_last = warn_first = warn_last = note_first = note_last = 0;
    err_method = warn_method = note_method = 0;
}
