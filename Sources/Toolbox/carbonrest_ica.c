/*
 * The Image Capture Architecture (ICAApplication.h and ICADevice.h): the
 * tree of objects and properties that cameras and scanners present to
 * applications, and the calls a device module uses to build it.  The
 * device list is the root.  No camera or scanner is here, so it starts
 * empty and stays so unless a device module in the program builds objects
 * with the ICD calls; those objects are then what the application calls
 * see, with events to whoever registered for them.  A message to a device
 * that no module serves is kICACommunicationErr.
 *
 * Parameter blocks are packed to two bytes: a 6-byte header (err, refcon)
 * and then the 4-byte fields at offsets 6, 10, ...  A call with a
 * completion routine calls it after, with the block, as an asynchronous
 * one would.
 */
#include <stdlib.h>
#include <string.h>

#include "carbonrest.h"

#define T(s) fourcc(s)
enum {
    kICACommunicationErr = -9900,
    kICAInvalidObjectErr = -9905,
    kICAInvalidPropertyErr = -9906,
    kICAIndexOutOfRangeErr = -9907,
    kICAPropertyTypeNotFoundErr = -9908,
};
enum { kICAFlagReadWriteAccess = 1, kICAFlagReadAccess = 2 };
enum { OBJ_TAG = 0x49434100, PROP_TAG = 0x49435000 }; /* 'ICA' and 'ICP' and an index */

typedef struct {
    bool used;
    uint32_t parent, type, subtype, refcon;
} ica_object;
typedef struct {
    bool used;
    uint32_t object, ptype, dtype, flags, refcon;
    uint8_t *data;
    uint32_t size;
} ica_property;
typedef struct {
    uint32_t object, type;
    gaddr proc, pb;
} ica_notify;

enum { MAX_OBJECTS = 512, MAX_PROPERTIES = 2048, MAX_NOTIFY = 32 };
static ica_object objects[MAX_OBJECTS];
static ica_property props[MAX_PROPERTIES];
static ica_notify notifies[MAX_NOTIFY];
static uint32_t device_list;

void rest_ica_reset(void)
{
    for (int i = 0; i < MAX_PROPERTIES; i++)
        free(props[i].data);
    memset(objects, 0, sizeof objects);
    memset(props, 0, sizeof props);
    memset(notifies, 0, sizeof notifies);
    device_list = 0;
}

static ica_object *obj_of(uint32_t ref)
{
    uint32_t i = ref - OBJ_TAG;
    return (ref & 0xFFFFFF00u) == OBJ_TAG && i >= 1 && i < MAX_OBJECTS && objects[i].used ? &objects[i] : NULL;
}

static ica_property *prop_of(uint32_t ref)
{
    uint32_t i = ref - PROP_TAG;
    return (ref & 0xFFFFF000u) == PROP_TAG && i >= 1 && i < MAX_PROPERTIES && props[i].used ? &props[i] : NULL;
}

static uint32_t new_object(uint32_t parent, uint32_t type, uint32_t subtype)
{
    for (uint32_t i = 1; i < MAX_OBJECTS; i++)
        if (!objects[i].used) {
            objects[i] = (ica_object){ true, parent, type, subtype, 0 };
            return OBJ_TAG + i;
        }
    return 0;
}

/* The device list, made when first wanted. */
static uint32_t the_list(void)
{
    if (!device_list)
        device_list = new_object(0, T("objl"), 0);
    return device_list;
}

/* A block's header done: the error, and the completion routine called. */
static void finish(gaddr pb, gaddr completion, int16_t err)
{
    if (pb)
        wr16(pb, (uint16_t)err);
    if (completion && pb) {
        uint32_t arg = pb;
        ppc_call(completion, &arg, 1);
    }
}

#define PB A(0)
#define DONE_WITH(err)                                                                                                 \
    do {                                                                                                               \
        int16_t e_ = (err);                                                                                            \
        finish(PB, A(1), e_);                                                                                          \
        RET((int32_t)e_);                                                                                              \
    } while (0)

static void put_info(gaddr at, const ica_object *o)
{
    wr32(at, o->type);
    wr32(at + 4, o->subtype);
}

static void put_prop_info(gaddr at, const ica_property *p)
{
    wr32(at, p->ptype);
    wr32(at + 4, p->dtype);
    wr32(at + 8, p->size);
    wr32(at + 12, p->flags);
}

static uint32_t child_count(uint32_t parent)
{
    uint32_t n = 0;
    for (uint32_t i = 1; i < MAX_OBJECTS; i++)
        n += objects[i].used && objects[i].parent == parent;
    return n;
}

static uint32_t prop_count(uint32_t object)
{
    uint32_t n = 0;
    for (uint32_t i = 1; i < MAX_PROPERTIES; i++)
        n += props[i].used && props[i].object == object;
    return n;
}

static void notify(uint32_t type, uint32_t object)
{
    for (int i = 0; i < MAX_NOTIFY; i++)
        if (notifies[i].proc && notifies[i].type == type && (!notifies[i].object || notifies[i].object == object ||
                                                             notifies[i].object == (obj_of(object) ? objects[object - OBJ_TAG].parent : 0))) {
            wr32(notifies[i].pb + 6, object);
            wr32(notifies[i].pb + 10, type);
            uint32_t arg = notifies[i].pb;
            ppc_call(notifies[i].proc, &arg, 1);
        }
}

/* ---- ICAApplication.h ---- */

static void ica_GetDeviceList(ppc_cpu *c)
{
    uint32_t list = the_list();
    wr32(PB + 6, list);
    DONE_WITH(list ? noErr : memFullErr);
}

/* ICAGetChildCount: object@6, count@10 */
static void ica_GetChildCount(ppc_cpu *c)
{
    ica_object *o = obj_of(rd32(PB + 6));
    if (o)
        wr32(PB + 10, child_count(rd32(PB + 6)));
    DONE_WITH(o ? noErr : kICAInvalidObjectErr);
}

/* ICAGetNthChild: parent@6, index@10 (from 0), child@14, childInfo@18 */
static void ica_GetNthChild(ppc_cpu *c)
{
    ica_object *o = obj_of(rd32(PB + 6));
    int16_t err = o ? noErr : kICAInvalidObjectErr;
    if (!err) {
        uint32_t want = rd32(PB + 10), k = 0;
        err = kICAIndexOutOfRangeErr;
        for (uint32_t i = 1; i < MAX_OBJECTS; i++)
            if (objects[i].used && objects[i].parent == rd32(PB + 6) && k++ == want) {
                wr32(PB + 14, OBJ_TAG + i);
                put_info(PB + 18, &objects[i]);
                err = noErr;
                break;
            }
    }
    DONE_WITH(err);
}

/* ICAGetObjectInfo: object@6, info@10 */
static void ica_GetObjectInfo(ppc_cpu *c)
{
    ica_object *o = obj_of(rd32(PB + 6));
    if (o)
        put_info(PB + 10, o);
    DONE_WITH(o ? noErr : kICAInvalidObjectErr);
}

/* ICAGetParentOfObject: object@6, parent@10, parentInfo@14 (none, for the device list) */
static void ica_GetParentOfObject(ppc_cpu *c)
{
    ica_object *o = obj_of(rd32(PB + 6));
    ica_object *p = o ? obj_of(o->parent) : NULL;
    if (p) {
        wr32(PB + 10, o->parent);
        put_info(PB + 14, p);
    }
    DONE_WITH(p ? noErr : kICAInvalidObjectErr);
}

/* ICAGetRootOfObject: object@6, root@10, rootInfo@14 */
static void ica_GetRootOfObject(ppc_cpu *c)
{
    uint32_t ref = rd32(PB + 6);
    ica_object *o = obj_of(ref);
    while (o && obj_of(o->parent)) {
        ref = o->parent;
        o = obj_of(ref);
    }
    if (o) {
        wr32(PB + 10, ref);
        put_info(PB + 14, o);
    }
    DONE_WITH(o ? noErr : kICAInvalidObjectErr);
}

static void ica_GetObjectRefCon(ppc_cpu *c)
{
    ica_object *o = obj_of(rd32(PB + 6));
    if (o)
        wr32(PB + 10, o->refcon);
    DONE_WITH(o ? noErr : kICAInvalidObjectErr);
}

static void ica_SetObjectRefCon(ppc_cpu *c)
{
    ica_object *o = obj_of(rd32(PB + 6));
    if (o)
        o->refcon = rd32(PB + 10);
    DONE_WITH(o ? noErr : kICAInvalidObjectErr);
}

/* ICAGetPropertyCount: object@6, count@10 */
static void ica_GetPropertyCount(ppc_cpu *c)
{
    ica_object *o = obj_of(rd32(PB + 6));
    if (o)
        wr32(PB + 10, prop_count(rd32(PB + 6)));
    DONE_WITH(o ? noErr : kICAInvalidObjectErr);
}

/* ICAGetNthProperty: object@6, index@10 (from 0), property@14, info@18 */
static void ica_GetNthProperty(ppc_cpu *c)
{
    ica_object *o = obj_of(rd32(PB + 6));
    int16_t err = o ? noErr : kICAInvalidObjectErr;
    if (!err) {
        uint32_t want = rd32(PB + 10), k = 0;
        err = kICAIndexOutOfRangeErr;
        for (uint32_t i = 1; i < MAX_PROPERTIES; i++)
            if (props[i].used && props[i].object == rd32(PB + 6) && k++ == want) {
                wr32(PB + 14, PROP_TAG + i);
                put_prop_info(PB + 18, &props[i]);
                err = noErr;
                break;
            }
    }
    DONE_WITH(err);
}

/* ICAGetPropertyByType: object@6, propertyType@10, property@14, info@18 */
static void ica_GetPropertyByType(ppc_cpu *c)
{
    ica_object *o = obj_of(rd32(PB + 6));
    int16_t err = o ? noErr : kICAInvalidObjectErr;
    if (!err) {
        err = kICAPropertyTypeNotFoundErr;
        for (uint32_t i = 1; i < MAX_PROPERTIES; i++)
            if (props[i].used && props[i].object == rd32(PB + 6) && props[i].ptype == rd32(PB + 10)) {
                wr32(PB + 14, PROP_TAG + i);
                put_prop_info(PB + 18, &props[i]);
                err = noErr;
                break;
            }
    }
    DONE_WITH(err);
}

/* ICAGetPropertyInfo: property@6, info@10 */
static void ica_GetPropertyInfo(ppc_cpu *c)
{
    ica_property *p = prop_of(rd32(PB + 6));
    if (p)
        put_prop_info(PB + 10, p);
    DONE_WITH(p ? noErr : kICAInvalidPropertyErr);
}

/* ICAGetPropertyData: property@6, startByte@10, requestedSize@14, dataPtr@18, actualSize@22, dataType@26 */
static void ica_GetPropertyData(ppc_cpu *c)
{
    ica_property *p = prop_of(rd32(PB + 6));
    int16_t err = p ? noErr : kICAInvalidPropertyErr;
    if (!err) {
        uint32_t start = rd32(PB + 10), want = rd32(PB + 14), n = 0;
        if (start > p->size)
            err = kICAIndexOutOfRangeErr;
        else {
            n = p->size - start;
            if (want != (uint32_t)-1 && want < n)
                n = want;
            if (n && rd32(PB + 18))
                gput(rd32(PB + 18), p->data + start, n);
            wr32(PB + 22, n);
            wr32(PB + 26, p->dtype);
        }
    }
    DONE_WITH(err);
}

/* ICASetPropertyData: property@6, startByte@10, dataPtr@14, dataSize@18, dataType@22 */
static void ica_SetPropertyData(ppc_cpu *c)
{
    ica_property *p = prop_of(rd32(PB + 6));
    int16_t err = p ? noErr : kICAInvalidPropertyErr;
    if (!err && !(p->flags & kICAFlagReadWriteAccess))
        err = kICAInvalidPropertyErr; /* read only */
    if (!err) {
        uint32_t start = rd32(PB + 10), n = rd32(PB + 18);
        if (start > p->size || (n && !rd32(PB + 14)) || n > (16u << 20))
            err = start > p->size ? kICAIndexOutOfRangeErr : paramErr;
        else {
            if (start + n > p->size) {
                p->data = realloc(p->data, start + n ? start + n : 1);
                p->size = start + n;
            }
            if (n)
                gget(p->data + start, rd32(PB + 14), n);
            if (rd32(PB + 22))
                p->dtype = rd32(PB + 22);
            notify(T("edpc"), p->object);
        }
    }
    DONE_WITH(err);
}

static void ica_GetParentOfProperty(ppc_cpu *c)
{
    ica_property *p = prop_of(rd32(PB + 6));
    ica_object *o = p ? obj_of(p->object) : NULL;
    if (o) {
        wr32(PB + 10, p->object);
        put_info(PB + 14, o);
    }
    DONE_WITH(o ? noErr : kICAInvalidPropertyErr);
}

static void ica_GetRootOfProperty(ppc_cpu *c)
{
    ica_property *p = prop_of(rd32(PB + 6));
    uint32_t ref = p ? p->object : 0;
    ica_object *o = obj_of(ref);
    while (o && obj_of(o->parent)) {
        ref = o->parent;
        o = obj_of(ref);
    }
    if (o) {
        wr32(PB + 10, ref);
        put_info(PB + 14, o);
    }
    DONE_WITH(o ? noErr : kICAInvalidPropertyErr);
}

static void ica_GetPropertyRefCon(ppc_cpu *c)
{
    ica_property *p = prop_of(rd32(PB + 6));
    if (p)
        wr32(PB + 10, p->refcon);
    DONE_WITH(p ? noErr : kICAInvalidPropertyErr);
}

static void ica_SetPropertyRefCon(ppc_cpu *c)
{
    ica_property *p = prop_of(rd32(PB + 6));
    if (p)
        p->refcon = rd32(PB + 10);
    DONE_WITH(p ? noErr : kICAInvalidPropertyErr);
}

/* ICAObjectSendMessage: object@6, message@10 (type, startByte, dataPtr,
 * dataSize, dataType), result@30.  A device's module would answer it. */
static void ica_ObjectSendMessage(ppc_cpu *c)
{
    ica_object *o = obj_of(rd32(PB + 6));
    if (o)
        wr32(PB + 30, 0);
    DONE_WITH(o ? kICACommunicationErr : kICAInvalidObjectErr);
}

/* ICARegisterEventNotification: object@6 (0: any), notifyType@10, notifyProc@14 */
static void ica_RegisterEventNotification(ppc_cpu *c)
{
    uint32_t object = rd32(PB + 6);
    if (object && !obj_of(object))
        DONE_WITH(kICAInvalidObjectErr);
    int16_t err = memFullErr;
    for (int i = 0; i < MAX_NOTIFY; i++) {
        if (notifies[i].proc && !(notifies[i].object == object && notifies[i].type == rd32(PB + 10)))
            continue;
        if (!notifies[i].pb) {
            notifies[i].pb = tb_NewPtr(32, false, true);
            if (!notifies[i].pb)
                break;
        }
        notifies[i].object = object;
        notifies[i].type = rd32(PB + 10);
        notifies[i].proc = rd32(PB + 14); /* no routine takes the registration back */
        err = noErr;
        break;
    }
    DONE_WITH(err);
}

/* ---- ICADevice.h: a device module's calls ---- */

/* ICDNewObject: parentObject@6, objectInfo@10 (type, subtype), object@18 */
static void icd_NewObject(ppc_cpu *c)
{
    uint32_t parent = rd32(PB + 6);
    int16_t err = noErr;
    if (!parent)
        parent = the_list();
    if (!obj_of(parent))
        err = kICAInvalidObjectErr;
    uint32_t o = err ? 0 : new_object(parent, rd32(PB + 10), rd32(PB + 14));
    if (!err && !o)
        err = memFullErr;
    wr32(PB + 18, o);
    if (!err)
        notify(T("eoba"), o);
    DONE_WITH(err);
}

static void dispose_object(uint32_t ref)
{
    for (uint32_t i = 1; i < MAX_OBJECTS; i++)
        if (objects[i].used && objects[i].parent == ref)
            dispose_object(OBJ_TAG + i);
    for (uint32_t i = 1; i < MAX_PROPERTIES; i++)
        if (props[i].used && props[i].object == ref) {
            free(props[i].data);
            memset(&props[i], 0, sizeof props[i]);
        }
    notify(T("eobr"), ref);
    memset(&objects[ref - OBJ_TAG], 0, sizeof objects[0]);
    for (int i = 0; i < MAX_NOTIFY; i++)
        if (notifies[i].object == ref)
            notifies[i].proc = 0;
    if (ref == device_list)
        device_list = 0;
}

/* ICDDisposeObject: object@6, with its children and properties */
static void icd_DisposeObject(ppc_cpu *c)
{
    uint32_t ref = rd32(PB + 6);
    bool known = obj_of(ref) != NULL;
    if (known)
        dispose_object(ref);
    DONE_WITH(known ? noErr : kICAInvalidObjectErr);
}

/* ICDNewProperty: object@6, propertyInfo@10 (type, dataType, dataSize,
 * dataFlags), property@26 */
static void icd_NewProperty(ppc_cpu *c)
{
    uint32_t object = rd32(PB + 6);
    int16_t err = obj_of(object) ? noErr : kICAInvalidObjectErr;
    uint32_t ref = 0;
    if (!err) {
        err = memFullErr;
        for (uint32_t i = 1; i < MAX_PROPERTIES; i++)
            if (!props[i].used) {
                uint32_t size = rd32(PB + 18);
                if (size > (16u << 20))
                    break;
                props[i] = (ica_property){ true, object, rd32(PB + 10), rd32(PB + 14), rd32(PB + 22), 0, NULL, size };
                props[i].data = calloc(size ? size : 1, 1);
                ref = PROP_TAG + i;
                err = noErr;
                break;
            }
    }
    wr32(PB + 26, ref);
    if (!err)
        notify(T("eoic"), object);
    DONE_WITH(err);
}

/* ICDDisposeProperty: property@6 */
static void icd_DisposeProperty(ppc_cpu *c)
{
    ica_property *p = prop_of(rd32(PB + 6));
    if (p) {
        uint32_t object = p->object;
        free(p->data);
        memset(p, 0, sizeof *p);
        notify(T("eoic"), object);
    }
    DONE_WITH(p ? noErr : kICAInvalidPropertyErr);
}

const ppc_export rest_ica_exports[] = {
    { "ICAGetChildCount", ica_GetChildCount },
    { "ICAGetDeviceList", ica_GetDeviceList },
    { "ICAGetNthChild", ica_GetNthChild },
    { "ICAGetNthProperty", ica_GetNthProperty },
    { "ICAGetObjectInfo", ica_GetObjectInfo },
    { "ICAGetObjectRefCon", ica_GetObjectRefCon },
    { "ICAGetParentOfObject", ica_GetParentOfObject },
    { "ICAGetParentOfProperty", ica_GetParentOfProperty },
    { "ICAGetPropertyByType", ica_GetPropertyByType },
    { "ICAGetPropertyCount", ica_GetPropertyCount },
    { "ICAGetPropertyData", ica_GetPropertyData },
    { "ICAGetPropertyInfo", ica_GetPropertyInfo },
    { "ICAGetPropertyRefCon", ica_GetPropertyRefCon },
    { "ICAGetRootOfObject", ica_GetRootOfObject },
    { "ICAGetRootOfProperty", ica_GetRootOfProperty },
    { "ICAObjectSendMessage", ica_ObjectSendMessage },
    { "ICARegisterEventNotification", ica_RegisterEventNotification },
    { "ICASetObjectRefCon", ica_SetObjectRefCon },
    { "ICASetPropertyData", ica_SetPropertyData },
    { "ICASetPropertyRefCon", ica_SetPropertyRefCon },
    { "ICDDisposeObject", icd_DisposeObject },
    { "ICDDisposeProperty", icd_DisposeProperty },
    { "ICDNewObject", icd_NewObject },
    { "ICDNewProperty", icd_NewProperty },
};
const unsigned rest_ica_export_count = sizeof rest_ica_exports / sizeof rest_ica_exports[0];
