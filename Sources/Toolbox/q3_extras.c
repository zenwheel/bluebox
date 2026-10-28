/*
 * The rest of the QuickDraw 3D libraries' exports: Custom Elements
 * (QD3DCustomElementsLib: name, URL and wire elements on any object with
 * a set), QuickDraw 3D 1.0's names for calls since renamed, its hit data,
 * the libraries' initialisation routines, RAVE's big memory, and the UNIX
 * storage calls a Mac had no use for.
 */
#include <stdlib.h>

#include "q3_view.h"
#include "qd3d.h"
#include "tb_internal.h"

/* ---------------------------------------------------------------------- */
/* Custom Elements: the element's bytes are the data itself (the name's
 * characters; the URL's and description's, then its options; the wire
 * data's atom container), copied out to the program as it asks. */

static int32_t ce_type(int which)
{
    static const char *const names[3] = {
        "Apple Computer, Inc.:NameElement",
        "Apple Computer, Inc.:URLElement",
        "Apple Computer, Inc.:WireElement",
    };
    return q3_register_element(names[which], 4);
}

static q3set *ce_set(gaddr object, bool make)
{
    q3obj *o = q3_of(object, 0);
    q3set *s = o ? q3_elements_of(o, make) : NULL;
    if (o && !s && make)
        q3_error(kQ3ErrorInvalidObjectType);
    return s;
}

static uint32_t gstrlen(gaddr s)
{
    uint32_t n = 0;
    while (s && rd8(s + n))
        n++;
    return n;
}

static gaddr gstrcopy(const uint8_t *s, uint32_t n)
{
    gaddr g = q3_galloc(n + 1);
    if (g) {
        gput(g, s, n);
        wr8(g + n, 0);
    }
    return g;
}

static void CENameElement_SetData(ppc_cpu *c)
{
    q3set *s = ce_set(Q3R(0), true);
    if (!s) {
        Q3RETURN(kQ3Failure);
        return;
    }
    if (!Q3R(1)) {
        q3set_clear(s, ce_type(0));
    } else {
        uint32_t n = gstrlen(Q3R(1));
        q3set_add_bytes(s, ce_type(0), gptr(Q3R(1)), n + 1);
    }
    q3_edited(q3_get(Q3R(0)));
    Q3RETURN(kQ3Success);
}

static void CENameElement_GetData(ppc_cpu *c)
{
    const q3elem *e = q3set_find(ce_set(Q3R(0), false), ce_type(0));
    if (!Q3R(1)) {
        Q3RETURN(kQ3Failure);
        return;
    }
    wr32(Q3R(1), e ? gstrcopy(e->bytes, e->size ? e->size - 1 : 0) : 0);
    Q3RETURN(kQ3Success);
}

static void CE_EmptyString(ppc_cpu *c)
{
    if (Q3R(0)) {
        q3_gfree(rd32(Q3R(0)));
        wr32(Q3R(0), 0);
    }
    Q3RETURN(kQ3Success);
}

static void CEUrlElement_SetData(ppc_cpu *c)
{
    q3set *s = ce_set(Q3R(0), true);
    gaddr d = Q3R(1);
    if (!s) {
        Q3RETURN(kQ3Failure);
        return;
    }
    if (!d || !rd32(d)) {
        q3set_clear(s, ce_type(1));
    } else {
        uint32_t nu = gstrlen(rd32(d)), nd = gstrlen(rd32(d + 4));
        uint8_t *b = malloc(nu + nd + 6);
        if (!b) {
            Q3RETURN(kQ3Failure);
            return;
        }
        gget(b, rd32(d), nu);
        b[nu] = 0;
        if (nd)
            gget(b + nu + 1, rd32(d + 4), nd);
        b[nu + 1 + nd] = 0;
        uint32_t opt = rd32(d + 8);
        b[nu + nd + 2] = (uint8_t)(opt >> 24), b[nu + nd + 3] = (uint8_t)(opt >> 16),
        b[nu + nd + 4] = (uint8_t)(opt >> 8), b[nu + nd + 5] = (uint8_t)opt;
        q3set_add_bytes(s, ce_type(1), b, nu + nd + 6);
        free(b);
    }
    q3_edited(q3_get(Q3R(0)));
    Q3RETURN(kQ3Success);
}

static void CEUrlElement_GetData(ppc_cpu *c)
{
    const q3elem *e = q3set_find(ce_set(Q3R(0), false), ce_type(1));
    if (!Q3R(1)) {
        Q3RETURN(kQ3Failure);
        return;
    }
    gaddr d = 0;
    if (e && e->size >= 6) {
        const uint8_t *u = e->bytes;
        uint32_t nu = (uint32_t)strnlen((const char *)u, e->size);
        const uint8_t *desc = u + nu + 1;
        uint32_t nd = (uint32_t)strnlen((const char *)desc, e->size - nu - 1);
        const uint8_t *o = desc + nd + 1;
        d = q3_galloc(12);
        if (d) {
            wr32(d, gstrcopy(u, nu));
            wr32(d + 4, nd ? gstrcopy(desc, nd) : 0);
            wr32(d + 8, (uint32_t)o[0] << 24 | (uint32_t)o[1] << 16 | (uint32_t)o[2] << 8 | o[3]);
        }
    }
    wr32(Q3R(1), d);
    Q3RETURN(kQ3Success);
}

static void CEUrlElement_EmptyData(ppc_cpu *c)
{
    gaddr p = Q3R(0);
    if (p && rd32(p)) {
        gaddr d = rd32(p);
        q3_gfree(rd32(d));
        q3_gfree(rd32(d + 4));
        q3_gfree(d);
        wr32(p, 0);
    }
    Q3RETURN(kQ3Success);
}

/* A QuickTime atom container is a handle: its contents kept. */
static void CEWireElement_SetData(ppc_cpu *c)
{
    q3set *s = ce_set(Q3R(0), true);
    gaddr h = Q3R(1);
    if (!s) {
        Q3RETURN(kQ3Failure);
        return;
    }
    if (!h || !rd32(h)) {
        q3set_clear(s, ce_type(2));
    } else {
        uint32_t n = (uint32_t)tb_GetHandleSize(h);
        q3set_add_bytes(s, ce_type(2), gptr(rd32(h)), n);
    }
    q3_edited(q3_get(Q3R(0)));
    Q3RETURN(kQ3Success);
}

static void CEWireElement_GetData(ppc_cpu *c)
{
    const q3elem *e = q3set_find(ce_set(Q3R(0), false), ce_type(2));
    if (!Q3R(1)) {
        Q3RETURN(kQ3Failure);
        return;
    }
    gaddr h = 0;
    if (e) {
        h = tb_NewHandle((int32_t)e->size, false, false);
        if (h && e->size)
            gput(rd32(h), e->bytes, e->size);
    }
    wr32(Q3R(1), h);
    Q3RETURN(kQ3Success);
}

static void CEWireElement_EmptyData(ppc_cpu *c)
{
    gaddr p = Q3R(0);
    if (p && rd32(p)) {
        tb_DisposeHandle(rd32(p));
        wr32(p, 0);
    }
    Q3RETURN(kQ3Success);
}

/* ---------------------------------------------------------------------- */
/* QuickDraw 3D 1.0's hit data: TQ3HitData (part, validMask, pickID,
 * path, object, localToWorldMatrix, xyzPoint, distance, normal,
 * shapePart), all at once. */

static void Q3Pick_GetHitData(ppc_cpu *c)
{
    gaddr pick = Q3R(0), out = Q3R(2);
    uint32_t i = Q3R(1);
    if (!out) {
        Q3RETURN(kQ3Failure);
        return;
    }
    static const struct {
        uint32_t mask, off;
    } parts[] = { { 1, 8 }, { 2, 12 }, { 4, 24 }, { 8, 28 }, { 16, 92 }, { 32, 104 }, { 64, 108 } };
    void (*detail)(ppc_cpu *) = qd3d_export_fn("Q3Pick_GetPickDetailData");
    void (*valid)(ppc_cpu *) = qd3d_export_fn("Q3Pick_GetPickDetailValidMask");
    gmemset(out, 0, 124);
    uint32_t lr = c->lr;
    gaddr tmp = q3_galloc(4);
    c->gpr[3] = pick, c->gpr[4] = i, c->gpr[5] = tmp;
    valid(c);
    uint32_t mask = c->gpr[3] == kQ3Success && tmp ? rd32(tmp) : 0;
    for (unsigned k = 0; k < sizeof parts / sizeof parts[0]; k++)
        if (mask & parts[k].mask) {
            c->gpr[3] = pick, c->gpr[4] = i, c->gpr[5] = parts[k].mask, c->gpr[6] = out + parts[k].off;
            c->lr = lr;
            detail(c);
        }
    wr32(out + 4, mask);
    q3_gfree(tmp);
    c->lr = lr;
    Q3RETURN(mask ? kQ3Success : kQ3Failure);
}

static void Q3Hit_EmptyData(ppc_cpu *c)
{
    gaddr h = Q3R(0);
    if (h) {
        gaddr root = rd32(h + 12), obj = rd32(h + 24), part = rd32(h + 120);
        if (root)
            q3_release(root);
        q3_gfree(rd32(h + 20));
        if (obj)
            q3_release(obj);
        if (part)
            q3_release(part);
        gmemset(h, 0, 124);
    }
    Q3RETURN(h ? kQ3Success : kQ3Failure);
}

/* ---------------------------------------------------------------------- */

static void ok_status(ppc_cpu *c) { Q3RETURN(kQ3Success); }
static void no_error(ppc_cpu *c) { Q3RETURN(0); }

/* The UNIX storage calls: there is no UNIX file system on a Mac. */
static void unix_storage(ppc_cpu *c)
{
    q3_error(kQ3ErrorNotSupported);
    Q3RETURN(0);
}

/* RAVE's large blocks, for engines. */
static void QABigMemoryNew(ppc_cpu *c) { Q3RETURN(tb_NewPtr((int32_t)Q3R(0), false, false)); }
static void QABigMemoryDelete(ppc_cpu *c)
{
    if (Q3R(0))
        tb_DisposePtr(Q3R(0));
    Q3RETURN(0);
}

#define E(n) { #n, n }
const ppc_export q3_extras_exports[] = {
    E(CENameElement_SetData), E(CENameElement_GetData), { "CENameElement_EmptyData", CE_EmptyString },
    E(CEUrlElement_SetData), E(CEUrlElement_GetData), E(CEUrlElement_EmptyData), E(CEWireElement_SetData),
    E(CEWireElement_GetData), E(CEWireElement_EmptyData),
    { "MacCustomElement_Initialization", no_error }, { "MacCustomElement_Termination", no_error },
    E(Q3Pick_GetHitData), E(Q3Hit_EmptyData),
    { "Q3Renderer_Flush", ok_status }, { "Q3Renderer_Sync", ok_status },
    { "Q3UnixStorage_New", unix_storage }, { "Q3UnixStorage_Set", unix_storage }, { "Q3UnixStorage_Get", unix_storage },
    { "Q3UnixStorage_GetType", unix_storage }, { "Q3UnixPathStorage_New", unix_storage },
    { "Q3UnixPathStorage_Set", unix_storage }, { "Q3UnixPathStorage_Get", unix_storage },
    /* The libraries' Code Fragment Manager initialisation and termination. */
    { "EiFragmentInitializationFunction", no_error }, { "EiFragmentTerminationFunction", no_error },
    { "CiLibrary_ConnectionInit", no_error }, { "CiLibrary_ConnectionExit", no_error },
    { "CrADBDriver_Connect", no_error }, { "vcInitialize", no_error },
    { "QAInit", no_error }, { "QAExit", no_error }, E(QABigMemoryNew), E(QABigMemoryDelete),
};
const unsigned q3_extras_export_count = sizeof q3_extras_exports / sizeof q3_extras_exports[0];

/* Names QuickDraw 3D 1.0 and the debugging libraries had for calls since
 * renamed. */
const char *const q3_aliases[][2] = {
    { "Q3AttributeClass_Register", "Q3XAttributeClass_Register" },
    { "Q3ElementType_GetElementSize", "Q3XElementType_GetElementSize" },
    { "Q3ObjectClass_Unregister", "Q3XObjectHierarchy_UnregisterClass" },
    { "Q3Quaternion_SetRotateX", "Q3Quaternion_SetRotate_X" },
    { "Q3Quaternion_SetRotateY", "Q3Quaternion_SetRotate_Y" },
    { "Q3Quaternion_SetRotateZ", "Q3Quaternion_SetRotate_Z" },
    { "Q3Quaternion_SetRotateXYZ", "Q3Quaternion_SetRotate_XYZ" },
    { "Q3View_SubmitWriteData", "Q3XView_SubmitWriteData" },
    { "EiDevelopmentError_Register", "Q3Error_Register" },
    { "EiDevelopmentWarning_Register", "Q3Warning_Register" },
    { "EiDevelopmentNotice_Register", "Q3Notice_Register" },
    { NULL, NULL },
};
