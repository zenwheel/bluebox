/*
 * Component Manager (ComponentDispatch, A82A).
 *
 * The selector is in D0; selector 0 is a call to a component instance, whose
 * glue pushed a ComponentParameters record (flags, paramSize, what, then the
 * arguments) on top of the instance and the result.  Components are those an
 * application registers itself, from code or from 'thng' resources, and
 * one of our own: Internet Config, with no preferences in it.  Asking for a
 * QuickTime codec or the like finds nothing.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "filemgr_internal.h"
#include "m68k.h"
#include "qt_codec.h"
#include "qt_tune.h"
#include "tb_internal.h"
#include "trap.h"

#define MAX_COMPONENTS 64
#define MAX_INSTANCES 128

/* The IDs we hand out: tags in the high word, so they never look like 0. */
#define COMPONENT_TAG 0x00C10000u
#define INSTANCE_TAG 0x00C20000u

enum {
    badComponentInstance = (int32_t)0x80008001,
    badComponentSelector = (int32_t)0x80008002,
    invalidComponentID = -3000,
    componentNotCaptured = -3002,
};

typedef struct {
    bool used;
    uint32_t type, subtype, manufacturer, flags;
    gaddr entry;      /* ComponentRoutine */
    gaddr name, info, icon;
    uint32_t refcon;
    int16_t resfile;  /* the resource file it came from, or -1 */
    char path[1024];  /* and that file, to open again once its registrant closed it */
    int16_t opened;   /* the refnum we opened it as, until CloseComponentResFile */
    unsigned instances;
    uint32_t captured_by; /* component ID, when captured */
} component;

typedef struct {
    bool used;
    unsigned comp;
    gaddr storage;
    uint32_t a5;
    int16_t error;
} instance;

static component comps[MAX_COMPONENTS];
static instance insts[MAX_INSTANCES];
static uint32_t mod_seed = 1;

static component *comp_of(uint32_t id)
{
    if ((id & 0xFFFF0000u) == INSTANCE_TAG) {
        /* Most calls taking a Component accept an instance too. */
        unsigned i = id & 0xFFFF;
        if (i < MAX_INSTANCES && insts[i].used)
            id = COMPONENT_TAG | insts[i].comp;
    }
    if ((id & 0xFFFF0000u) != COMPONENT_TAG)
        return NULL;
    unsigned i = id & 0xFFFF;
    return i < MAX_COMPONENTS && comps[i].used ? &comps[i] : NULL;
}

static instance *inst_of(uint32_t id)
{
    if ((id & 0xFFFF0000u) != INSTANCE_TAG)
        return NULL;
    unsigned i = id & 0xFFFF;
    return i < MAX_INSTANCES && insts[i].used ? &insts[i] : NULL;
}

/* Does component c match a ComponentDescription (zero fields match anything)? */
static bool matches(const component *c, gaddr cd)
{
    if (!cd)
        return true;
    uint32_t t = rd32(cd), s = rd32(cd + 4), m = rd32(cd + 8), f = rd32(cd + 12), mask = rd32(cd + 16);
    return (!t || t == c->type) && (!s || s == c->subtype) && (!m || m == c->manufacturer) &&
           ((c->flags ^ f) & mask) == 0 && !c->captured_by;
}

static uint32_t register_component(uint32_t type, uint32_t subtype, uint32_t manu, uint32_t flags,
                                   gaddr entry, gaddr name, gaddr info, gaddr icon, int16_t resfile)
{
    for (unsigned i = 0; i < MAX_COMPONENTS; i++) {
        if (!comps[i].used) {
            comps[i] = (component){ true, type, subtype, manu, flags, entry, name, info, icon, 0, resfile, "", 0, 0, 0 };
            if (resfile < 0 || !rm_host_path(resfile, comps[i].path, sizeof comps[i].path))
                comps[i].path[0] = 0;
            mod_seed++;
            return COMPONENT_TAG | i;
        }
    }
    return 0;
}

/* Call a component routine: ComponentResult (params, storage). */
static int32_t call_entry(instance *in, gaddr params)
{
    cpu68k *c = g_cpu;
    component *k = &comps[in->comp];
    uint32_t saved_a5 = c->a[5];
    if (in->a5)
        c->a[5] = in->a5;
    cpu_push32(c, 0);
    cpu_push32(c, params);
    cpu_push32(c, in->storage);
    guest_call(k->entry);
    c->a[5] = saved_a5;
    return (int32_t)cpu_pop32(c);
}

/* Send one of the standard requests whose single argument is a long.  The
 * instance follows the parameters, as it does on the stack of a call
 * through the glue: CallComponentFunction finds the instance there (and
 * so its storage), Internet Config's open function among others. */
static int32_t send_request(instance *in, uint32_t ci, int16_t what, int has_arg, uint32_t arg)
{
    gaddr p = tb_NewPtr(12, false, true);
    if (!p)
        return memFullErr;
    wr8(p + 1, has_arg ? 4 : 0);
    wr16(p + 2, (uint16_t)what);
    wr32(p + 4, arg);
    wr32(p + (has_arg ? 8 : 4), ci);
    int32_t r = call_entry(in, p);
    tb_DisposePtr(p);
    return r;
}

static uint32_t open_component(uint32_t cid, int16_t *errp)
{
    component *k = comp_of(cid);
    *errp = noErr;
    if (!k) {
        *errp = invalidComponentID;
        return 0;
    }
    for (unsigned i = 0; i < MAX_INSTANCES; i++) {
        if (!insts[i].used) {
            insts[i] = (instance){ true, (unsigned)(k - comps), 0, 0, 0 };
            k->instances++;
            uint32_t ci = INSTANCE_TAG | i;
            int32_t r = send_request(&insts[i], ci, -1, 1, ci); /* kComponentOpenSelect */
            if (r) {
                insts[i].used = false;
                k->instances--;
                *errp = (int16_t)r;
                return 0;
            }
            return ci;
        }
    }
    *errp = memFullErr;
    return 0;
}

static int16_t close_component(uint32_t ci)
{
    instance *in = inst_of(ci);
    if (!in)
        return (int16_t)badComponentInstance;
    int32_t r = send_request(in, ci, -2, 1, ci); /* kComponentCloseSelect */
    comps[in->comp].instances--;
    in->used = false;
    return (int16_t)r;
}

static uint32_t find_next(uint32_t after, gaddr cd)
{
    unsigned start = 0;
    if (after) {
        component *k = comp_of(after);
        start = k ? (unsigned)(k - comps) + 1 : MAX_COMPONENTS;
    }
    for (unsigned i = start; i < MAX_COMPONENTS; i++)
        if (comps[i].used && matches(&comps[i], cd))
            return COMPONENT_TAG | i;
    return 0;
}

/* A 'thng' resource: ComponentDescription, then ResourceSpecs (type, id)
 * for the code, name, info string and icon. */
static uint32_t register_thng(gaddr h, int16_t resfile)
{
    if (!h || !rd32(h))
        return 0;
    gaddr p = rd32(h);
    uint32_t code_type = rd32(p + 20);
    int16_t code_id = (int16_t)rd16(p + 24);
    gaddr code = tb_GetResource((int32_t)code_type, code_id);
    if (!code || !rd32(code))
        return 0;
    /* The component outlives its file: a program registering the
     * components of a file it found (Internet Config, its extension)
     * closes the file again, which would take the code with it. */
    tb_DetachResource(code);
    tb_HNoPurge(code);
    tb_HLock(code);
    gaddr name = 0, info = 0, icon = 0;
    p = rd32(h);
    if (rd32(p + 26))
        name = tb_GetResource((int32_t)rd32(p + 26), (int16_t)rd16(p + 30));
    p = rd32(h);
    if (rd32(p + 32))
        info = tb_GetResource((int32_t)rd32(p + 32), (int16_t)rd16(p + 36));
    p = rd32(h);
    if (rd32(p + 38))
        icon = tb_GetResource((int32_t)rd32(p + 38), (int16_t)rd16(p + 42));
    gaddr strs[] = { name, info, icon };
    for (int i = 0; i < 3; i++)
        if (strs[i])
            tb_DetachResource(strs[i]);
    p = rd32(h);
    return register_component(rd32(p), rd32(p + 4), rd32(p + 8), rd32(p + 12), rd32(code),
                              name, info, icon, resfile);
}

/* A public resource of a component (QuickTime 4): asked of the component
 * itself, when it can do kComponentGetPublicResourceSelect (-10); none
 * of ours has a public resource map.  A handle the caller disposes, or 0. */
gaddr components_public_resource(uint32_t cid, uint32_t type, int16_t id)
{
    int16_t err;
    uint32_t ci = open_component(cid, &err);
    instance *in = inst_of(ci);
    if (!in)
        return 0;
    gaddr h = 0;
    gaddr p = tb_NewPtr(22, false, true);
    if (p) {
        wr8(p + 1, 2); /* kComponentCanDoSelect (ftnNumber) */
        wr16(p + 2, (uint16_t)-3);
        wr16(p + 4, (uint16_t)-10);
        wr32(p + 6, ci); /* the instance after the parameters, as on the stack */
        if (call_entry(in, p)) {
            /* (type, id, &h): the last argument first */
            wr8(p + 1, 10);
            wr16(p + 2, (uint16_t)-10);
            wr32(p + 4, p + 18);
            wr16(p + 8, (uint16_t)id);
            wr32(p + 10, type);
            wr32(p + 14, ci);
            wr32(p + 18, 0);
            if (!call_entry(in, p))
                h = rd32(p + 18);
        }
        tb_DisposePtr(p);
    }
    close_component(ci);
    return h;
}

/* Copy a handle's contents into another handle the caller supplied. */
static void copy_handle(gaddr from, gaddr to)
{
    if (!to)
        return;
    if (!from || !rd32(from)) {
        tb_SetHandleSize(to, 0);
        return;
    }
    int32_t n = tb_GetHandleSize(from);
    tb_SetHandleSize(to, n);
    if (rd32(to))
        gmemcpy(rd32(to), rd32(from), (uint32_t)n);
}

/* Is refnum rn open on the component's file? */
static bool is_comp_file(const component *k, int16_t rn)
{
    char now[1024];
    return rn > 0 && rm_host_path(rn, now, sizeof now) && !strcmp(now, k->path);
}

/* OpenComponentResFile: the refnum the component came from while that is
 * still its file, else the file opened again (and current, as opening
 * makes it), until CloseComponentResFile closes it. */
static int16_t comp_resfile(component *k)
{
    if (!k || k->resfile < 0)
        return -1;
    if (!k->path[0] || is_comp_file(k, k->resfile))
        return k->resfile;
    if (!is_comp_file(k, k->opened))
        k->opened = rm_open_path(k->path, 1 /* fsRdPerm */);
    return k->opened > 0 ? k->opened : -1;
}

/* Argument k bytes above the stack pointer (the last argument is at 0). */
#define ARG32(off) rd32(c->a[7] + (off))
#define ARG16(off) rd16(c->a[7] + (off))

/* Pop n bytes of arguments, store the result (rbytes 0, 2 or 4) and return. */
static void finish(cpu68k *c, uint32_t ret, uint32_t n, int rbytes, uint32_t result)
{
    c->a[7] += n;
    if (rbytes == 4)
        wr32(c->a[7], result);
    else if (rbytes == 2)
        wr16(c->a[7], (uint16_t)result);
    cpu_push32(c, ret);
}

/* A call to an instance: ComponentParameters at the stack top. */
static void component_call(cpu68k *c, uint32_t ret)
{
    gaddr params = c->a[7];
    uint32_t size = rd8(params + 1);
    uint32_t ci = rd32(params + 4 + size);
    instance *in = inst_of(ci);
    int32_t r;
    if (!in) {
        r = badComponentInstance;
        LOGF(LOG_TRAPS, "  component call %04X: %08X is no instance\n", rd16(params + 2), ci);
    } else
        r = call_entry(in, params);
    if (in)
        in->error = (int16_t)(r & 0xFFFF) == r ? (int16_t)r : in->error;
    finish(c, ret, 4 + size + 4, 4, (uint32_t)r);
}

/* CallComponentFunction(params, func) and
 * CallComponentFunctionWithStorage(storage, params, func) share a selector;
 * the function is called with the caller's arguments, after the storage
 * for the second. */
static void call_function(cpu68k *c, uint32_t ret)
{
    gaddr func = ARG32(0), params = ARG32(4);
    uint32_t size = rd8(params + 1);
    instance *in = inst_of(rd32(params + 4 + size));
    uint32_t n = 8;
    if (in && in->storage && ARG32(8) == in->storage)
        n = 12; /* the WithStorage form passed it */
    cpu_push32(c, 0);
    if (n == 12)
        cpu_push32(c, in->storage);
    c->a[7] -= size;
    gmemcpy(c->a[7], params + 4, size);
    guest_call(func);
    uint32_t r = cpu_pop32(c);
    finish(c, ret, n, 4, r);
}

void raw_ComponentDispatch(cpu68k *c, uint16_t tw)
{
    uint16_t sel = (uint16_t)c->d[0] & 0xFF;
    if (sel)
        LOGF(LOG_TRAPS, "  ComponentDispatch selector %02X\n", sel);
    uint32_t ret = cpu_pop32(c);
    int16_t err;
    component *k;
    instance *in;

    switch (sel) {
    case 0x00:
        component_call(c, ret);
        return;
    case 0x01: { /* RegisterComponent(cd, entry, global, name, info, icon): Component */
        gaddr cd = ARG32(18);
        uint32_t id = register_component(rd32(cd), rd32(cd + 4), rd32(cd + 8), rd32(cd + 12), ARG32(14),
                                         ARG32(8), ARG32(4), ARG32(0), -1);
        finish(c, ret, 22, 4, id);
        return;
    }
    case 0x02: /* UnregisterComponent(c): OSErr */
        k = comp_of(ARG32(0));
        err = !k ? invalidComponentID : k->instances ? -3001 /* validInstancesExist */ : noErr;
        if (!err) {
            k->used = false;
            mod_seed++;
        }
        finish(c, ret, 4, 2, (uint16_t)err);
        return;
    case 0x03: { /* CountComponents(looking): long */
        uint32_t n = 0;
        for (unsigned i = 0; i < MAX_COMPONENTS; i++)
            n += comps[i].used && matches(&comps[i], ARG32(0));
        finish(c, ret, 4, 4, n);
        return;
    }
    case 0x04: /* FindNextComponent(c, looking): Component */
        finish(c, ret, 8, 4, find_next(ARG32(4), ARG32(0)));
        return;
    case 0x05: { /* GetComponentInfo(c, cd, name, info, icon): OSErr */
        k = comp_of(ARG32(16));
        gaddr cd = ARG32(12);
        if (k && cd) {
            wr32(cd, k->type);
            wr32(cd + 4, k->subtype);
            wr32(cd + 8, k->manufacturer);
            wr32(cd + 12, k->flags);
            wr32(cd + 16, 0);
        }
        if (k) {
            copy_handle(k->name, ARG32(8));
            copy_handle(k->info, ARG32(4));
            copy_handle(k->icon, ARG32(0));
        }
        finish(c, ret, 20, 2, k ? noErr : (uint16_t)invalidComponentID);
        return;
    }
    case 0x06: /* GetComponentListModSeed: long */
        finish(c, ret, 0, 4, mod_seed);
        return;
    case 0x07: /* OpenComponent(c): ComponentInstance */
        finish(c, ret, 4, 4, open_component(ARG32(0), &err));
        return;
    case 0x08: /* CloseComponent(ci): OSErr */
        finish(c, ret, 4, 2, (uint16_t)close_component(ARG32(0)));
        return;
    case 0x0A: /* GetComponentInstanceError(ci): OSErr */
        in = inst_of(ARG32(0));
        err = in ? in->error : (int16_t)badComponentInstance;
        if (in)
            in->error = 0;
        finish(c, ret, 4, 2, (uint16_t)err);
        return;
    case 0x0B: /* SetComponentInstanceError(ci, err) */
        if ((in = inst_of(ARG32(2))) != NULL)
            in->error = (int16_t)ARG16(0);
        finish(c, ret, 6, 0, 0);
        return;
    case 0x0C: /* GetComponentInstanceStorage(ci): Handle */
        in = inst_of(ARG32(0));
        finish(c, ret, 4, 4, in ? in->storage : 0);
        return;
    case 0x0D: /* SetComponentInstanceStorage(ci, h) */
        if ((in = inst_of(ARG32(4))) != NULL)
            in->storage = ARG32(0);
        finish(c, ret, 8, 0, 0);
        return;
    case 0x0E: /* GetComponentInstanceA5(ci): long */
        in = inst_of(ARG32(0));
        finish(c, ret, 4, 4, in ? in->a5 : 0);
        return;
    case 0x0F: /* SetComponentInstanceA5(ci, a5) */
        if ((in = inst_of(ARG32(4))) != NULL)
            in->a5 = ARG32(0);
        finish(c, ret, 8, 0, 0);
        return;
    case 0x10: /* GetComponentRefcon(c): long */
        k = comp_of(ARG32(0));
        finish(c, ret, 4, 4, k ? k->refcon : 0);
        return;
    case 0x11: /* SetComponentRefcon(c, refcon) */
        if ((k = comp_of(ARG32(4))) != NULL)
            k->refcon = ARG32(0);
        finish(c, ret, 8, 0, 0);
        return;
    case 0x12: /* RegisterComponentResource(thng handle, global): Component */
        finish(c, ret, 6, 4, register_thng(ARG32(2), tb_CurResFile()));
        return;
    case 0x13: /* CountComponentInstances(c): long */
        k = comp_of(ARG32(0));
        finish(c, ret, 4, 4, k ? k->instances : 0);
        return;
    case 0x14: { /* RegisterComponentResourceFile(refnum, global): long */
        int16_t file = (int16_t)ARG16(2), saved = tb_CurResFile();
        int32_t n = 0;
        tb_UseResFile(file);
        int16_t count = tb_Count1Resources((int32_t)fourcc("thng"));
        for (int16_t i = 1; i <= count; i++)
            n += register_thng(tb_Get1IndResource((int32_t)fourcc("thng"), i), file) != 0;
        tb_UseResFile(saved);
        finish(c, ret, 4, 4, (uint32_t)n);
        return;
    }
    case 0x15: /* OpenComponentResFile(c): short */
        finish(c, ret, 4, 2, (uint16_t)comp_resfile(comp_of(ARG32(0))));
        return;
    case 0x18: { /* CloseComponentResFile(refnum): OSErr -- a registrant's file stays open */
        int16_t rn = (int16_t)ARG16(0);
        for (unsigned i = 0; i < MAX_COMPONENTS; i++) {
            if (comps[i].used && comps[i].opened == rn && rn > 0) {
                tb_CloseResFile(rn);
                comps[i].opened = 0;
                break;
            }
        }
        finish(c, ret, 2, 2, noErr);
        return;
    }
    case 0x1C: { /* CaptureComponent(captured, capturing): Component */
        k = comp_of(ARG32(4));
        uint32_t r = 0;
        if (k && !k->captured_by) {
            k->captured_by = ARG32(0);
            r = ARG32(4);
        }
        finish(c, ret, 8, 4, r);
        return;
    }
    case 0x1D: /* UncaptureComponent(c): OSErr */
        k = comp_of(ARG32(0));
        err = k && k->captured_by ? noErr : componentNotCaptured;
        if (k)
            k->captured_by = 0;
        finish(c, ret, 4, 2, (uint16_t)err);
        return;
    case 0x1E: /* SetDefaultComponent(c, flags): OSErr */
        finish(c, ret, 6, 2, noErr);
        return;
    case 0x20: /* ResolveComponentAlias(c): Component */
        finish(c, ret, 4, 4, ARG32(0));
        return;
    case 0x21: { /* OpenDefaultComponent(type, subtype): ComponentInstance */
        uint32_t found = 0;
        for (unsigned i = 0; i < MAX_COMPONENTS && !found; i++)
            if (comps[i].used && !comps[i].captured_by && comps[i].type == ARG32(4) &&
                (!ARG32(0) || comps[i].subtype == ARG32(0)))
                found = COMPONENT_TAG | i;
        finish(c, ret, 8, 4, found ? open_component(found, &err) : 0);
        return;
    }
    case 0x24: { /* DelegateComponentCall(originalParams, ci): ComponentResult */
        in = inst_of(ARG32(0));
        int32_t r = in ? call_entry(in, ARG32(4)) : badComponentInstance;
        finish(c, ret, 8, 4, (uint32_t)r);
        return;
    }
    case 0x29: /* GetComponentIconSuite(c, *suite): OSErr */
        wr32(ARG32(0), 0);
        finish(c, ret, 8, 2, (uint16_t)resFNotFound);
        return;
    case 0x2C: { /* GetComponentTypeModSeed(type): long */
        finish(c, ret, 4, 4, mod_seed);
        return;
    }
    case 0x2D: { /* OpenAComponent(c, *ci): OSErr */
        uint32_t ci = open_component(ARG32(4), &err);
        wr32(ARG32(0), ci);
        finish(c, ret, 8, 2, (uint16_t)err);
        return;
    }
    case 0x2E: { /* OpenADefaultComponent(type, subtype, *ci): OSErr */
        uint32_t found = 0, ci = 0;
        for (unsigned i = 0; i < MAX_COMPONENTS && !found; i++)
            if (comps[i].used && !comps[i].captured_by && comps[i].type == ARG32(8) &&
                (!ARG32(4) || comps[i].subtype == ARG32(4)))
                found = COMPONENT_TAG | i;
        err = found ? noErr : -3000 /* invalidComponentID: none found */;
        if (found)
            ci = open_component(found, &err);
        wr32(ARG32(0), ci);
        finish(c, ret, 12, 2, (uint16_t)err);
        return;
    }
    case 0x2F: { /* OpenAComponentResFile(c, *refnum): OSErr */
        int16_t rn = comp_resfile(comp_of(ARG32(4)));
        wr16(ARG32(0), (uint16_t)rn);
        finish(c, ret, 8, 2, rn >= 0 ? noErr : (uint16_t)resFNotFound);
        return;
    }
    case 0x35: { /* GetComponentResource(c, type, id, *h): OSErr */
        int16_t rn = comp_resfile(comp_of(ARG32(10)));
        gaddr h = 0;
        if (rn >= 0) {
            int16_t saved = tb_CurResFile();
            tb_UseResFile(rn);
            h = tb_GetResource((int32_t)ARG32(6), (int16_t)ARG16(4));
            if (h)
                tb_DetachResource(h);
            tb_UseResFile(saved);
        }
        wr32(ARG32(0), h);
        finish(c, ret, 14, 2, h ? noErr : (uint16_t)resFNotFound);
        return;
    }
    case 0x36: /* GetComponentIndString(c, str, id, index): OSErr */
        if (ARG32(4))
            wr8(ARG32(4), 0);
        finish(c, ret, 12, 2, (uint16_t)resFNotFound);
        return;
    case 0x38: { /* GetComponentPublicResource(c, type, id, *h): OSErr */
        gaddr h = comp_of(ARG32(10)) ? components_public_resource(ARG32(10), ARG32(6), (int16_t)ARG16(4)) : 0;
        if (ARG32(0))
            wr32(ARG32(0), h);
        finish(c, ret, 14, 2, h ? noErr : (uint16_t)resNotFound);
        return;
    }
    case 0x39: { /* GetComponentPublicResourceList(type, id, flags, cd, missingProc, refCon, atomContainerPtr) */
        /* An atom container with nothing in it: no component has public
         * resources to list (its root 'sean' atom after a 12-byte header). */
        gaddr out = ARG32(0), h = tb_NewHandle(32, false, true);
        if (h && out) {
            gaddr p = rd32(h);
            wr32(p + 12, 20);
            wr32(p + 16, fourcc("sean"));
            wr32(p + 20, 1);
            wr32(out, h);
        }
        finish(c, ret, 26, 2, h ? noErr : (uint16_t)memFullErr);
        return;
    }
    case 0x3A: { /* GetComponentPublicIndString(c, str, id, index): OSErr */
        gaddr str = ARG32(4);
        int16_t index = (int16_t)ARG16(0);
        gaddr h = comp_of(ARG32(8)) ? components_public_resource(ARG32(8), fourcc("STR#"), (int16_t)ARG16(2)) : 0;
        bool found = false;
        if (str)
            wr8(str, 0);
        if (h && rd32(h) && index >= 1 && index <= (int16_t)rd16(rd32(h))) {
            gaddr q = rd32(h) + 2;
            for (int i = 1; i < index; i++)
                q += 1u + rd8(q);
            if (str)
                gmemcpy(str, q, 1u + rd8(q));
            found = true;
        }
        if (h)
            tb_DisposeHandle(h);
        finish(c, ret, 12, 2, found ? noErr : (uint16_t)resNotFound);
        return;
    }
    case 0xFF:
        call_function(c, ret);
        return;
    }
    cpu_push32(c, ret);
    log_printf("ComponentDispatch: unknown selector %02X\n", sel);
    cpu_halt(c, "unimplemented ComponentDispatch selector %02X", sel);
}


/* ---- Components of our own (the QuickTime movie controller) ---- */

uint32_t components_register_native(uint32_t type, uint32_t subtype, uint32_t manufacturer, gaddr entry)
{
    return register_component(type, subtype, manufacturer, 0, entry, 0, 0, 0, -1);
}

uint32_t components_open(uint32_t component)
{
    int16_t err;
    return open_component(component, &err);
}

void components_set_storage(uint32_t ci, gaddr storage)
{
    instance *in = inst_of(ci);
    if (in)
        in->storage = storage;
}

void components_close(uint32_t ci) { close_component(ci); }

/* ---- Internet Config ('PREF' 'ICAp') ----
 * Programs that require Internet Config open it themselves (Internet
 * Explorer).  Its preferences are the ones the host gives (the Internet
 * settings: name, mail, servers, proxies), and those programs set while
 * they run; any other lookup finds nothing, so programs use their own
 * defaults.  Keys match without regard to case, as Internet Config's do. */

enum {
    kICCStart = 0, kICCStop = 1, kICCFindConfigFile = 2, kICCSpecifyConfigFile = 3, kICCGetSeed = 4,
    kICCBegin = 5, kICCGetPref = 6, kICCSetPref = 7, kICCCountPref = 8, kICCGetIndPref = 9, kICCEnd = 10,
    kICCDefaultFileName = 11,
    kICCDeletePref = 12, kICCGetPerm = 13, kICCFindUserConfigFile = 14, kICCGetPrefHandle = 26,
    kICCFindPrefHandle = 36, kICCLaunchURL = 17,
    kICCSetPrefHandle = 27,
    icTruncatedErr = -670, icPrefNotFoundErr = -666,
};

#define IC_MAX_PREFS 96
static struct ic_pref {
    uint8_t key[256]; /* a Pascal string */
    uint8_t *data;
    int32_t len;
} ic_prefs[IC_MAX_PREFS];
static int ic_npref;
static uint32_t ic_seed = 1;

static int ic_find(const uint8_t *key)
{
    for (int i = 0; i < ic_npref; i++) {
        if (ic_prefs[i].key[0] != key[0])
            continue;
        int k = 1;
        while (k <= key[0] && tolower(ic_prefs[i].key[k]) == tolower(key[k]))
            k++;
        if (k > key[0])
            return i;
    }
    return -1;
}

static void ic_store(const uint8_t *key, const uint8_t *data, int32_t len)
{
    int i = ic_find(key);
    if (i < 0) {
        if (ic_npref == IC_MAX_PREFS)
            return;
        i = ic_npref++;
        memcpy(ic_prefs[i].key, key, (size_t)key[0] + 1);
        ic_prefs[i].data = NULL;
    }
    free(ic_prefs[i].data);
    ic_prefs[i].data = malloc(len > 0 ? (size_t)len : 1);
    if (len > 0)
        memcpy(ic_prefs[i].data, data, (size_t)len);
    ic_prefs[i].len = len;
    ic_seed++;
}

/* The preferences the host gave at the start: what ICGetDefaultPref gives. */
static struct ic_default {
    uint8_t key[256];
    uint8_t *data;
    int32_t len;
} ic_defaults[IC_MAX_PREFS];
static int ic_ndefault;

static void ic_key(gaddr key, uint8_t out[256])
{
    out[0] = key ? rd8(key) : 0;
    for (int k = 1; k <= out[0]; k++)
        out[k] = rd8(key + (uint32_t)k);
}

void machine_set_internet_pref(const char *key, const uint8_t *data, int32_t len)
{
    uint8_t k[256];
    size_t n = strlen(key) < 255 ? strlen(key) : 255;
    k[0] = (uint8_t)n;
    memcpy(k + 1, key, n);
    ic_store(k, data, len);
    int i = 0;
    while (i < ic_ndefault && !(ic_defaults[i].key[0] == k[0] && !memcmp(ic_defaults[i].key + 1, k + 1, n)))
        i++;
    if (i == ic_ndefault && ic_ndefault < IC_MAX_PREFS)
        ic_ndefault++;
    if (i < ic_ndefault) {
        free(ic_defaults[i].data);
        memcpy(ic_defaults[i].key, k, n + 1);
        ic_defaults[i].data = malloc(len > 0 ? (size_t)len : 1);
        if (len > 0)
            memcpy(ic_defaults[i].data, data, (size_t)len);
        ic_defaults[i].len = len;
    }
}

/* ICGetDefaultPref(key, prefH): the default's data into the handle. */
static int16_t ic_get_default_pref(gaddr key, gaddr h)
{
    uint8_t k[256];
    ic_key(key, k);
    if (!h)
        return paramErr;
    for (int i = 0; i < ic_ndefault; i++) {
        if (ic_defaults[i].key[0] != k[0])
            continue;
        int j = 1;
        while (j <= k[0] && tolower(ic_defaults[i].key[j]) == tolower(k[j]))
            j++;
        if (j <= k[0])
            continue;
        tb_SetHandleSize(h, ic_defaults[i].len);
        if (!rd32(h) || tb_GetHandleSize(h) != ic_defaults[i].len)
            return memFullErr;
        gput(rd32(h), ic_defaults[i].data, (uint32_t)ic_defaults[i].len);
        return noErr;
    }
    return icPrefNotFoundErr;
}

/* Profiles (Internet Config 2.0): each its own set of preferences; the
 * one in use is the working set above, the others are kept here. */
enum { IC_MAX_PROFILES = 16, icProfileNotFoundErr = -676, icTooManyProfilesErr = -677 };
static struct ic_profile {
    int32_t id;
    uint8_t name[256];
    struct ic_pref *saved; /* its preferences while it is not the current profile */
    int nsaved;
} ic_profiles[IC_MAX_PROFILES];
static int ic_nprofile, ic_current_profile;

static void ic_profiles_init(void)
{
    if (ic_nprofile)
        return;
    ic_nprofile = 1;
    ic_profiles[0].id = 1;
    ic_profiles[0].name[0] = 7;
    memcpy(ic_profiles[0].name + 1, "Default", 7);
    ic_current_profile = 0;
}

static int ic_profile_find(int32_t id)
{
    for (int i = 0; i < ic_nprofile; i++)
        if (ic_profiles[i].id == id)
            return i;
    return -1;
}

static struct ic_pref *ic_copy_prefs(const struct ic_pref *p, int n)
{
    struct ic_pref *c = calloc(n ? (size_t)n : 1, sizeof *c);
    for (int i = 0; i < n; i++) {
        c[i] = p[i];
        c[i].data = malloc(p[i].len > 0 ? (size_t)p[i].len : 1);
        if (p[i].len > 0)
            memcpy(c[i].data, p[i].data, (size_t)p[i].len);
    }
    return c;
}

static void ic_free_prefs(struct ic_pref *p, int n)
{
    for (int i = 0; i < n; i++)
        free(p[i].data);
    free(p);
}

int16_t ic_get_current_profile(gaddr out)
{
    ic_profiles_init();
    if (out)
        wr32(out, (uint32_t)ic_profiles[ic_current_profile].id);
    return noErr;
}

int16_t ic_set_current_profile(int32_t id)
{
    ic_profiles_init();
    int to = ic_profile_find(id);
    if (to < 0)
        return icProfileNotFoundErr;
    if (to == ic_current_profile)
        return noErr;
    struct ic_profile *from = &ic_profiles[ic_current_profile];
    ic_free_prefs(from->saved, from->nsaved);
    from->saved = ic_copy_prefs(ic_prefs, ic_npref);
    from->nsaved = ic_npref;
    for (int i = 0; i < ic_npref; i++)
        free(ic_prefs[i].data);
    struct ic_profile *dst = &ic_profiles[to];
    ic_npref = dst->nsaved;
    for (int i = 0; i < ic_npref; i++) {
        ic_prefs[i] = dst->saved[i];
        dst->saved[i].data = NULL; /* moved into the working set */
    }
    free(dst->saved);
    dst->saved = NULL;
    dst->nsaved = 0;
    ic_current_profile = to;
    ic_seed++;
    return noErr;
}

int16_t ic_count_profiles(gaddr out)
{
    ic_profiles_init();
    if (out)
        wr32(out, (uint32_t)ic_nprofile);
    return noErr;
}

int16_t ic_get_ind_profile(int32_t index, gaddr out)
{
    ic_profiles_init();
    if (index < 1 || index > ic_nprofile)
        return icProfileNotFoundErr;
    if (out)
        wr32(out, (uint32_t)ic_profiles[index - 1].id);
    return noErr;
}

int16_t ic_get_profile_name(int32_t id, gaddr name)
{
    ic_profiles_init();
    int i = ic_profile_find(id);
    if (i < 0)
        return icProfileNotFoundErr;
    if (name)
        gput(name, ic_profiles[i].name, (uint32_t)ic_profiles[i].name[0] + 1);
    return noErr;
}

int16_t ic_set_profile_name(int32_t id, gaddr name)
{
    ic_profiles_init();
    int i = ic_profile_find(id);
    if (i < 0)
        return icProfileNotFoundErr;
    if (!name)
        return paramErr;
    ic_profiles[i].name[0] = rd8(name);
    for (uint32_t k = 0; k < rd8(name); k++)
        ic_profiles[i].name[k + 1] = rd8(name + 1 + k);
    ic_seed++;
    return noErr;
}

/* ICAddProfile(prototypeID, &newID): a profile starting as a copy of the
 * prototype's preferences (none for kICNilProfileID). */
int16_t ic_add_profile(int32_t proto, gaddr out)
{
    ic_profiles_init();
    if (ic_nprofile == IC_MAX_PROFILES)
        return icTooManyProfilesErr;
    int p = proto ? ic_profile_find(proto) : -1;
    if (proto && p < 0)
        return icProfileNotFoundErr;
    struct ic_profile *n = &ic_profiles[ic_nprofile];
    memset(n, 0, sizeof *n);
    int32_t id = 1;
    for (int i = 0; i < ic_nprofile; i++)
        if (ic_profiles[i].id >= id)
            id = ic_profiles[i].id + 1;
    n->id = id;
    n->name[0] = 12;
    memcpy(n->name + 1, "New Profile ", 12);
    if (p >= 0) {
        bool cur = p == ic_current_profile;
        n->saved = ic_copy_prefs(cur ? ic_prefs : ic_profiles[p].saved, cur ? ic_npref : ic_profiles[p].nsaved);
        n->nsaved = cur ? ic_npref : ic_profiles[p].nsaved;
    }
    ic_nprofile++;
    if (out)
        wr32(out, (uint32_t)id);
    ic_seed++;
    return noErr;
}

int16_t ic_delete_profile(int32_t id)
{
    ic_profiles_init();
    int i = ic_profile_find(id);
    if (i < 0)
        return icProfileNotFoundErr;
    if (i == ic_current_profile || ic_nprofile == 1)
        return paramErr; /* the profile in use stays */
    ic_free_prefs(ic_profiles[i].saved, ic_profiles[i].nsaved);
    memmove(&ic_profiles[i], &ic_profiles[i + 1], (size_t)(ic_nprofile - i - 1) * sizeof ic_profiles[0]);
    if (ic_current_profile > i)
        ic_current_profile--;
    ic_nprofile--;
    ic_seed++;
    return noErr;
}

/* ICCreateGURLEvent(helperCreator, urlH, &event): a GURL event for the
 * helper application, the URL its direct parameter. */
int16_t ic_create_gurl_event(int32_t helper, gaddr urlh, gaddr event)
{
    if (!urlh || !rd32(urlh) || !event)
        return paramErr;
    gaddr target = tb_NewPtr(8, false, true), sig = tb_NewPtr(4, false, true);
    if (!target || !sig)
        return memFullErr;
    wr32(sig, (uint32_t)helper);
    int16_t err = tb_AECreateDesc((int32_t)fourcc("sign"), sig, 4, target);
    if (!err)
        err = tb_AECreateAppleEvent((int32_t)fourcc("GURL"), (int32_t)fourcc("GURL"), target, -1 /* auto */, 0, event);
    tb_AEDisposeDesc(target);
    tb_DisposePtr(target);
    tb_DisposePtr(sig);
    if (!err)
        err = tb_AEPutParamPtr(event, (int32_t)fourcc("----"), (int32_t)fourcc("TEXT"), rd32(urlh), tb_GetHandleSize(urlh));
    return err;
}

/* ICSendGURLEvent(event): sent without waiting for an answer. */
int16_t ic_send_gurl_event(gaddr event)
{
    if (!event)
        return paramErr;
    gaddr reply = tb_NewPtr(8, false, true);
    if (!reply)
        return memFullErr;
    wr32(reply, fourcc("null"));
    wr32(reply + 4, 0);
    int16_t err = tb_AESend(event, reply, 1 /* kAENoReply */ | 0x10 /* kAENeverInteract */, 0, -1, 0, 0);
    tb_AEDisposeDesc(reply);
    tb_DisposePtr(reply);
    return err;
}

/* ICGetPref: the data into buf (size in: its room, out: the data's
 * length; no buf asks only the length), and the attributes. */
int16_t ic_get_pref(gaddr key, gaddr attr, gaddr buf, gaddr size)
{
    uint8_t k[256];
    ic_key(key, k);
    int i = ic_find(k);
    if (attr)
        wr32(attr, 0);
    if (i < 0) {
        if (size)
            wr32(size, 0);
        return icPrefNotFoundErr;
    }
    int32_t room = size ? (int32_t)rd32(size) : 0, len = ic_prefs[i].len;
    if (size)
        wr32(size, (uint32_t)len);
    if (!buf)
        return noErr;
    gput(buf, ic_prefs[i].data, (uint32_t)(len < room ? len : room));
    return len > room ? icTruncatedErr : noErr;
}

/* ICGetPrefHandle (a new handle into *out) and ICFindPrefHandle (the
 * data into the handle given). */
int16_t ic_get_pref_handle(gaddr key, gaddr attr, gaddr out, gaddr h)
{
    uint8_t k[256];
    ic_key(key, k);
    int i = ic_find(k);
    if (attr)
        wr32(attr, 0);
    if (out)
        wr32(out, 0);
    if (i < 0)
        return icPrefNotFoundErr;
    if (!h)
        h = tb_NewHandle(ic_prefs[i].len, false, false);
    else
        tb_SetHandleSize(h, ic_prefs[i].len);
    if (!h || !rd32(h) || tb_GetHandleSize(h) != ic_prefs[i].len)
        return memFullErr;
    gput(rd32(h), ic_prefs[i].data, (uint32_t)ic_prefs[i].len);
    if (out)
        wr32(out, h);
    return noErr;
}

int16_t ic_set_pref(gaddr key, gaddr buf, int32_t len)
{
    uint8_t k[256];
    ic_key(key, k);
    if (!k[0] || len < 0)
        return paramErr;
    uint8_t *d = malloc(len > 0 ? (size_t)len : 1);
    if (len > 0)
        gget(d, buf, (uint32_t)len);
    ic_store(k, d, len);
    free(d);
    return noErr;
}

int16_t ic_delete_pref(gaddr key)
{
    uint8_t k[256];
    ic_key(key, k);
    int i = ic_find(k);
    if (i < 0)
        return icPrefNotFoundErr;
    free(ic_prefs[i].data);
    ic_prefs[i] = ic_prefs[--ic_npref];
    ic_seed++;
    return noErr;
}

int32_t ic_count_prefs(void) { return ic_npref; }
uint32_t ic_get_seed(void) { return ic_seed; }

/* ICGetIndPref: the key of the index'th preference (from 1). */
int16_t ic_get_ind_pref(int32_t index, gaddr key)
{
    if (index < 1 || index > ic_npref)
        return icPrefNotFoundErr;
    if (key)
        gput(key, ic_prefs[index - 1].key, (uint32_t)ic_prefs[index - 1].key[0] + 1);
    return noErr;
}

static void ic_file_name(gaddr str)
{
    static const char name[] = "Internet Preferences";
    wr8(str, sizeof name - 1);
    gput(str + 1, name, sizeof name - 1);
}

/* An empty Internet Preferences file (a resource file) in the Preferences
 * folder, or a resource fork for one made without: programs open it
 * themselves and call it damaged when it has none (Internet Explorer). */
void ic_make_file(void)
{
    gaddr tmp = tb_NewPtr(2 + 4 + 64 + FSSpec_SIZEOF, false, true);
    if (!tmp)
        return;
    gaddr vref = tmp, dir = tmp + 2, name = tmp + 6, spec = tmp + 70;
    ic_file_name(name);
    int16_t e = tb_FindFolder(-32768, (int32_t)fourcc("pref"), 1, vref, dir);
    if (!e)
        e = tb_FSMakeFSSpec((int16_t)rd16(vref), (int32_t)rd32(dir), name, spec);
    if (!e || e == fnfErr) /* adds a resource fork to a file with none; leaves one with a fork alone */
        tb_FSpCreateResFile(spec, (int32_t)fourcc("ICAp"), (int32_t)fourcc("ICAp"), 0);
    tb_DisposePtr(tmp);
}

int16_t ic_launch_url(gaddr data, int32_t len, int32_t a, int32_t b)
{
    if (a < 0 || b > len || a >= b)
        a = 0, b = len;
    char url[1024];
    int n = 0;
    for (int32_t i = a; i < b && n < (int)sizeof url - 1; i++) {
        uint8_t ch = rd8(data + (uint32_t)i);
        if (ch > ' ')
            url[n++] = (char)ch;
    }
    url[n] = 0;
    char *u = url; /* less any "<URL:...>" around it */
    if (!strncmp(u, "<URL:", 5))
        u += 5;
    else if (*u == '<')
        u++;
    size_t ul = strlen(u);
    if (ul && u[ul - 1] == '>')
        u[--ul] = 0;
    if (!ul || !strchr(u, ':'))
        return -673; /* icNoURLErr */
    return process_open_url(u) ? noErr : -668; /* icConfigNotFoundErr */
}

/* pascal ComponentResult (ComponentParameters *params, Handle storage) */
static void ic_component(cpu68k *c, uint16_t tw)
{
    (void)tw;
    uint32_t ret = cpu_pop32(c);
    cpu_pop32(c); /* storage */
    gaddr params = cpu_pop32(c);
    int16_t what = (int16_t)rd16(params + 2);
    gaddr last = rd32(params + 4); /* the last argument (a pointer, for the calls answering in one) */
    int32_t r = noErr;
    switch (what) {
    case -3: /* kComponentCanDoSelect */
        r = (int16_t)rd16(params + 4) <= 0x34;
        break;
    case -4: /* kComponentVersionSelect */
        r = 0x00020000;
        break;
    case kICCGetSeed:
        if (last)
            wr32(last, ic_get_seed());
        break;
    case kICCCountPref:
        if (last)
            wr32(last, (uint32_t)ic_count_prefs());
        break;
    case kICCDefaultFileName: /* the preferences file's name (Str63) */
        if (last)
            ic_file_name(last);
        break;
    case kICCFindConfigFile: /* as Internet Config does, make the file when there is none */
        ic_make_file();
        break;
    case kICCGetPerm:
        if (last)
            wr8(last, 0); /* icNoPerm */
        break;
    /* The arguments, last first: */
    case kICCGetPref: /* (key, &attr, buf, &size) */
        r = ic_get_pref(rd32(params + 16), rd32(params + 12), rd32(params + 8), last);
        break;
    case kICCSetPref: /* (key, attr, buf, size) */
        r = ic_set_pref(rd32(params + 16), rd32(params + 8), (int32_t)last);
        break;
    case kICCGetIndPref: /* (index, key) */
        r = ic_get_ind_pref((int32_t)rd32(params + 8), last);
        break;
    case kICCGetPrefHandle: /* (key, &attr, &prefh) */
        r = ic_get_pref_handle(rd32(params + 12), rd32(params + 8), last, 0);
        break;
    case kICCFindPrefHandle: /* (key, &attr, prefh) */
        r = ic_get_pref_handle(rd32(params + 12), rd32(params + 8), 0, last);
        break;
    case kICCSetPrefHandle: /* (key, attr, prefh) */
        r = last && rd32(last) ? ic_set_pref(rd32(params + 12), rd32(last), tb_GetHandleSize(last)) : paramErr;
        break;
    case kICCDeletePref: /* (key) */
        r = ic_delete_pref(last);
        break;
    /* Profiles, defaults and GURL events (Internet Config 2.0 and 2.5) */
    case 0x25: /* ICGetCurrentProfile(&id) */
        r = ic_get_current_profile(last);
        break;
    case 0x26: /* ICSetCurrentProfile(id) */
        r = ic_set_current_profile((int32_t)last);
        break;
    case 0x27: /* ICCountProfiles(&count) */
        r = ic_count_profiles(last);
        break;
    case 0x28: /* ICGetIndProfile(index, &id) */
        r = ic_get_ind_profile((int32_t)rd32(params + 8), last);
        break;
    case 0x29: /* ICGetProfileName(id, name) */
        r = ic_get_profile_name((int32_t)rd32(params + 8), last);
        break;
    case 0x2A: /* ICSetProfileName(id, name) */
        r = ic_set_profile_name((int32_t)rd32(params + 8), last);
        break;
    case 0x2B: /* ICAddProfile(prototypeID, &newID) */
        r = ic_add_profile((int32_t)rd32(params + 8), last);
        break;
    case 0x2C: /* ICDeleteProfile(id) */
        r = ic_delete_profile((int32_t)last);
        break;
    case 0x31: /* ICGetDefaultPref(key, prefH) */
        r = ic_get_default_pref(rd32(params + 8), last);
        break;
    case 0x33: /* ICCreateGURLEvent(helperCreator, urlH, &event) */
        r = ic_create_gurl_event((int32_t)rd32(params + 12), rd32(params + 8), last);
        break;
    case 0x34: /* ICSendGURLEvent(event) */
        r = ic_send_gurl_event(last);
        break;
    /* The mappings database, URLs and the configuration (vam.c): */
    case 0x0F: /* ICEditPreferences(key) */
        r = ic_edit_preferences();
        break;
    case 0x10: /* ICParseURL(hint, data, len, &selStart, &selEnd, url) */
        r = ic_parse_url(rd32(params + 24), rd32(params + 20), (int32_t)rd32(params + 16), rd32(params + 12),
                         rd32(params + 8), last);
        break;
    case 0x12: /* ICCountMapEntries(entries, &count) */
        r = ic_map_count(rd32(params + 8), last);
        break;
    case 0x13: /* ICGetIndMapEntry(entries, index, &pos, entry) */
        r = ic_map_get_ind(rd32(params + 16), (int32_t)rd32(params + 12), rd32(params + 8), last);
        break;
    case 0x14: /* ICGetMapEntry(entries, pos, entry) */
        r = ic_map_get(rd32(params + 12), (int32_t)rd32(params + 8), last);
        break;
    case 0x15: /* ICSetMapEntry(entries, pos, entry) */
        r = ic_map_set(rd32(params + 12), (int32_t)rd32(params + 8), last);
        break;
    case 0x16: /* ICDeleteMapEntry(entries, pos) */
        r = ic_map_delete(rd32(params + 8), (int32_t)last);
        break;
    case 0x17: /* ICAddMapEntry(entries, entry) */
        r = ic_map_add(rd32(params + 8), last);
        break;
    case 0x18: /* ICMapFilename(filename, entry) */
        r = ic_map_filename(rd32(params + 8), last);
        break;
    case 0x19: /* ICMapTypeCreator(type, creator, filename, entry) */
        r = ic_map_type_creator(rd32(params + 16), rd32(params + 12), rd32(params + 8), last);
        break;
    case 0x1C: /* ICMapEntriesFilename(entries, filename, entry) */
        r = ic_map_entries_filename(rd32(params + 12), rd32(params + 8), last);
        break;
    case 0x1D: /* ICMapEntriesTypeCreator(entries, type, creator, filename, entry) */
        r = ic_map_entries_type_creator(rd32(params + 20), rd32(params + 16), rd32(params + 12), rd32(params + 8),
                                        last);
        break;
    case 0x1F: /* ICGetConfigReference(ref) */
        r = ic_get_config_ref(last);
        break;
    case 0x20: /* ICSetConfigReference(ref, flags) */
        r = ic_set_config_ref(rd32(params + 8));
        break;
    case kICCLaunchURL: { /* (hint, data, len, &selStart, &selEnd), last first */
        gaddr ps = rd32(params + 8);
        int32_t len = (int32_t)rd32(params + 12);
        r = ic_launch_url(rd32(params + 16), len, ps ? (int32_t)rd32(ps) : 0, last ? (int32_t)rd32(last) : len);
        break;
    }
    default: /* open, close, start, stop, begin, end, finding the configuration ... */
        break;
    }
    wr32(c->a[7], (uint32_t)r);
    cpu_push32(c, ret);
}

/* OpenDefaultComponent for native callers: an instance of the first
 * component of the type (and subtype, if not 0), or 0. */
gaddr components_open_default(uint32_t type, uint32_t subtype)
{
    int16_t err;
    for (unsigned i = 0; i < MAX_COMPONENTS; i++)
        if (comps[i].used && !comps[i].captured_by && comps[i].type == type && (!subtype || comps[i].subtype == subtype))
            return open_component(COMPONENT_TAG | i, &err);
    return 0;
}

/* Forget everything registered: a new run. */
void components_reset(void)
{
    for (int i = 0; i < ic_nprofile; i++)
        ic_free_prefs(ic_profiles[i].saved, ic_profiles[i].nsaved);
    memset(ic_profiles, 0, sizeof ic_profiles);
    ic_nprofile = ic_current_profile = 0;
    for (int i = 0; i < ic_ndefault; i++)
        free(ic_defaults[i].data);
    memset(ic_defaults, 0, sizeof ic_defaults);
    ic_ndefault = 0;
    memset(comps, 0, sizeof comps);
    memset(insts, 0, sizeof insts);
    register_component(fourcc("PREF"), fourcc("ICAp"), fourcc("JPQE"), 0,
                       native_register(ic_component, "InternetConfig", 0), 0, 0, 0, -1);
    qt_tune_register(); /* QuickTime's tune player and note allocator */
    qt_gimport_register(); /* and its graphics importer */
    qt_clock_register();   /* and microsecond clock */
}
