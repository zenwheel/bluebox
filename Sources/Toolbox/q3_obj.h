/*
 * QuickDraw 3D's object model (q3_object.c), shared by its files.
 *
 * An object is a host record named by a handle: a guest address the
 * program holds as its TQ3Object, unique while the object lives.  Shared
 * objects are reference counted (Q3Shared_GetReference adds one,
 * Q3Object_Dispose takes one away); views, elements and picks are owned
 * by whoever made them.
 */
#ifndef Q3_OBJ_H
#define Q3_OBJ_H

#include "q3_internal.h"

#define Q3T(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

enum {
    kQ3Failure = 0, kQ3Success = 1,
    kQ3False = 0, kQ3True = 1,
};

/* Object types (QD3D.h). */
#define Q3_VIEW Q3T('v', 'i', 'e', 'w')
#define Q3_ELEMENT Q3T('e', 'l', 'm', 'n')
#define Q3_PICK Q3T('p', 'i', 'c', 'k')
#define Q3_SHARED Q3T('s', 'h', 'r', 'd')
#define Q3_RENDERER Q3T('r', 'd', 'd', 'r')
#define Q3_SHAPE Q3T('s', 'h', 'a', 'p')
#define Q3_GEOMETRY Q3T('g', 'm', 't', 'r')
#define Q3_SHADER Q3T('s', 'h', 'd', 'r')
#define Q3_SURFACE_SHADER Q3T('s', 'u', 's', 'h')
#define Q3_ILLUMINATION Q3T('i', 'l', 's', 'h')
#define Q3_STYLE Q3T('s', 't', 'y', 'l')
#define Q3_TRANSFORM Q3T('x', 'f', 'r', 'm')
#define Q3_LIGHT Q3T('l', 'g', 'h', 't')
#define Q3_CAMERA Q3T('c', 'm', 'r', 'a')
#define Q3_STATE_OPERATOR Q3T('s', 't', 'o', 'p')
#define Q3_GROUP Q3T('g', 'r', 'u', 'p')
#define Q3_DISPLAY_GROUP Q3T('d', 's', 'p', 'g')
#define Q3_LIGHT_GROUP Q3T('l', 'g', 'h', 'g')
#define Q3_UNKNOWN Q3T('u', 'n', 'k', 'n')
#define Q3_REFERENCE Q3T('r', 'f', 'r', 'n')
#define Q3_SET Q3T('s', 'e', 't', ' ')
#define Q3_ATTRIBUTE_SET Q3T('a', 't', 't', 'r')
#define Q3_DRAW_CONTEXT Q3T('d', 'c', 't', 'x')
#define Q3_TEXTURE Q3T('t', 'x', 't', 'r')
#define Q3_FILE Q3T('f', 'i', 'l', 'e')
#define Q3_STORAGE Q3T('s', 't', 'r', 'g')
#define Q3_STRING Q3T('s', 't', 'r', 'n')
#define Q3_CSTRING Q3T('s', 't', 'r', 'c')
#define Q3_SHAPE_PART Q3T('s', 'p', 'r', 't')
#define Q3_VIEW_HINTS Q3T('v', 'w', 'h', 'n')

/* Errors, warnings and notices (QD3DErrors.h) that the files post. */
enum {
    kQ3ErrorNotInitialized = -28490, kQ3ErrorUnimplemented = -28488, kQ3ErrorOutOfMemory = -28482,
    kQ3ErrorNULLParameter = -28481, kQ3ErrorParameterOutOfRange = -28480, kQ3ErrorInvalidParameter = -28479,
    kQ3ErrorInvalidData = -28478, kQ3ErrorInvalidObject = -28474, kQ3ErrorInvalidObjectClass = -28473,
    kQ3ErrorInvalidObjectType = -28472, kQ3ErrorInvalidObjectName = -28471, kQ3ErrorUnknownElementType = -28465,
    kQ3ErrorNotSupported = -28464, kQ3ErrorDegenerateGeometry = -28460, kQ3ErrorNoStorageSetForFile = -28458,
    kQ3ErrorEndOfFile = -28457, kQ3ErrorInvalidMetafile = -28455, kQ3ErrorInvalidMetafileObject = -28452,
    kQ3ErrorStorageNotOpen = -28445, kQ3ErrorStorageIsOpen = -28444, kQ3ErrorFileNotOpen = -28442,
    kQ3ErrorFileIsOpen = -28441, kQ3ErrorFileModeRestriction = -28433, kQ3ErrorViewNotStarted = -28419,
    kQ3ErrorViewIsStarted = -28418, kQ3ErrorRendererNotSet = -28417, kQ3ErrorImmediateModeUnderflow = -28415,
    kQ3ErrorCameraNotSet = -28413, kQ3ErrorDrawContextNotSet = -28412, kQ3ErrorNonInvertibleMatrix = -28411,
    kQ3ErrorNothingToPop = -28406, kQ3ErrorInvalidPositionForGroup = -28399,
    kQ3ErrorInvalidObjectForGroup = -28398, kQ3ErrorAttributeNotContained = -28394,
    kQ3ErrorBadDrawContextType = -28391, kQ3ErrorBadDrawContext = -28389, kQ3ErrorUnsupportedPixelDepth = -28388,
    kQ3WarningUnknownElementType = -28295, kQ3WarningUnknownObject = -28288,
    kQ3WarningInvalidMetafileObject = -28287, kQ3WarningUnresolvableReference = -28283,
    kQ3WarningNonInvertibleMatrix = -28273, kQ3NoticeDataAlreadyEmpty = -28100,
    kQ3NoticeMethodNotSupported = -28099, kQ3NoticeSystemAlreadyInitialized = -28087,
};

/* Attribute types (QD3DSet.h). */
enum {
    kQ3AttributeTypeSurfaceUV = 1, kQ3AttributeTypeShadingUV, kQ3AttributeTypeNormal,
    kQ3AttributeTypeAmbientCoefficient, kQ3AttributeTypeDiffuseColor, kQ3AttributeTypeSpecularColor,
    kQ3AttributeTypeSpecularControl, kQ3AttributeTypeTransparencyColor, kQ3AttributeTypeSurfaceTangent,
    kQ3AttributeTypeHighlightState, kQ3AttributeTypeSurfaceShader, kQ3AttributeTypeNumTypes,
};

typedef struct q3obj q3obj;
typedef struct q3view q3view;

typedef struct q3class {
    uint32_t type, parent;
    const char *name;
    /* Free the object's data (not the object). */
    void (*dispose)(q3obj *o);
    /* Fill `to` (its data NULL) as a copy of `from`; false if it can't. */
    bool (*duplicate)(q3obj *to, const q3obj *from);
    /* Submitted to a view: drawn, picked, bounded or written, as the
     * view's loop is. */
    void (*submit)(q3obj *o, q3view *v);
    bool drawable;
    /* A class a program registered (Q3XObjectHierarchy_RegisterClass). */
    bool registered;
    gaddr metahandler, virtual_metahandler;
    uint32_t instance_size;
    gaddr guest; /* its TQ3XObjectClass */
    char *own_name;
} q3class;

struct q3obj {
    gaddr g;
    uint32_t type;      /* the leaf type */
    q3class *cls;
    int refs;
    uint32_t edit;
    gaddr set;          /* a shape's set, a reference we hold */
    void *data;
    gaddr instance;     /* a registered class's private data */
};

/* Classes. */
q3class *q3_class(uint32_t type);
q3class *q3_class_named(const char *name);
/* Fill in a class's methods (each file, for its own). */
void q3_class_ops(uint32_t type, void (*dispose)(q3obj *), bool (*duplicate)(q3obj *, const q3obj *),
                  void (*submit)(q3obj *, q3view *), bool drawable);
/* Only the submit method (a class whose data another file keeps). */
void q3_class_submit(uint32_t type, void (*submit)(q3obj *, q3view *));
bool q3_type_is(uint32_t leaf, uint32_t type);
/* A method from a registered class's metahandler (or its parents'); 0
 * if none gives it. */
gaddr q3_class_method(const q3class *k, uint32_t method);
/* An object of a registered class, its New method called with param. */
gaddr q3_registered_new(q3class *k, gaddr param);
/* The type on leaf's line just below `above` (Q3Shape_GetType: below
 * kQ3SharedTypeShape), or 0. */
uint32_t q3_type_below(uint32_t leaf, uint32_t above);

/* Objects. */
q3obj *q3_new(uint32_t type, void *data);
q3obj *q3_get(gaddr g);
/* An object of (or under) type, or NULL with kQ3ErrorInvalidObject. */
q3obj *q3_of(gaddr g, uint32_t type);
/* A new reference to a shared object (Q3Shared_GetReference); 0 for 0. */
gaddr q3_ref(gaddr g);
/* Q3Object_Dispose's work. */
void q3_release(gaddr g);
/* Replace *slot's reference with one to g (each may be 0). */
void q3_assign(gaddr *slot, gaddr g);
gaddr q3_duplicate(gaddr g);
void q3_edited(q3obj *o);
bool q3_is_shared(const q3obj *o);
bool q3_initialized(void);

/* Errors as Q3Error_Get reports them, and the program's methods called. */
void q3_error(int32_t error);
void q3_warning(int32_t warning);
void q3_notice(int32_t notice);

/* Handles that aren't objects (group positions, mesh parts): a guest
 * address naming a host pointer.  kind tells them apart. */
gaddr q3_handle_new(int kind, void *p);
void *q3_handle_get(gaddr g, int kind);
void q3_handle_free(gaddr g);
enum { Q3H_OBJECT = 1, Q3H_POSITION, Q3H_MESH_VERTEX, Q3H_MESH_FACE, Q3H_MESH_EDGE, Q3H_MESH_CONTOUR,
       Q3H_MESH_COMPONENT, Q3H_CLASS };

/* Sets: elements kept as their bytes, as the program gave them.  A
 * surface shader attribute holds a reference to its shader. */
typedef struct {
    int32_t type;
    uint32_t size;
    uint8_t *bytes;
    gaddr g; /* a guest copy, for Q3XAttributeSet_GetPointer (0: none yet) */
} q3elem;
typedef struct {
    int n;
    q3elem *e;
} q3set;

int32_t q3_element_size(int32_t type); /* -1: not a type */
const q3elem *q3set_find(const q3set *s, int32_t type);
/* Add (or replace) an element from guest memory, or from host bytes in
 * guest byte order. */
bool q3set_add(q3set *s, int32_t type, gaddr data);
bool q3set_add_bytes(q3set *s, int32_t type, const uint8_t *bytes, uint32_t size);
void q3set_clear(q3set *s, int32_t type);
void q3set_empty(q3set *s);
void q3set_copy(q3set *to, const q3set *from);
/* A registered element type of this name (registered now if need be). */
int32_t q3_register_element(const char *name, uint32_t size);
/* Registered element classes (Q3XElementClass_Register). */
bool q3_element_class_named(const char *name, int32_t *type, gaddr *meta);
bool q3_element_class_of(int32_t type, const char **name, gaddr *meta);
/* The elements of a set or shape (a shape's set made if make). */
q3set *q3_elements_of(q3obj *o, bool make);
/* An attribute set object's set (NULL for none). */
q3set *q3_set_of(gaddr set);

/* Reading attributes: whether set g has one, its value decoded. */
bool q3_attr_color(gaddr set, int32_t type, float rgb[3]);
bool q3_attr_float(gaddr set, int32_t type, float *f);
bool q3_attr_vector(gaddr set, int32_t type, float v[3]);
bool q3_attr_uv(gaddr set, int32_t type, float uv[2]);
gaddr q3_attr_shader(gaddr set);

/* Strings in guest memory. */
void q3_gstr(gaddr s, char *out, size_t size);

/* Guest blocks for data handed to the program (GetData), freed by
 * EmptyData. */
gaddr q3_galloc(uint32_t size);
void q3_gfree(gaddr p);

/* Each file's classes, filled in at Q3Initialize. */
void q3_geom_classes(void);
void q3_view_classes(void);
void q3_file_classes(void);

#endif
