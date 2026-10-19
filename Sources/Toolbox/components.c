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
#include <string.h>

#include "m68k.h"
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
            comps[i] = (component){ true, type, subtype, manu, flags, entry, name, info, icon, 0, resfile, 0, 0 };
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

/* Send one of the standard requests whose single argument is a long. */
static int32_t send_request(instance *in, uint32_t ci, int16_t what, int has_arg, uint32_t arg)
{
    gaddr p = tb_NewPtr(8, false, true);
    if (!p)
        return memFullErr;
    wr8(p + 1, has_arg ? 4 : 0);
    wr16(p + 2, (uint16_t)what);
    wr32(p + 4, arg);
    int32_t r = call_entry(in, p);
    tb_DisposePtr(p);
    (void)ci;
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
    p = rd32(h);
    return register_component(rd32(p), rd32(p + 4), rd32(p + 8), rd32(p + 12), rd32(code),
                              name, info, icon, resfile);
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
    if (!in)
        r = badComponentInstance;
    else
        r = call_entry(in, params);
    if (in)
        in->error = (int16_t)(r & 0xFFFF) == r ? (int16_t)r : in->error;
    finish(c, ret, 4 + size + 4, 4, (uint32_t)r);
}

/* CallComponentFunction(params, func) and
 * CallComponentFunctionWithStorage(storage, params, func) share a selector;
 * the function is called with the storage, then the caller's arguments. */
static void call_function(cpu68k *c, uint32_t ret)
{
    gaddr func = ARG32(0), params = ARG32(4);
    uint32_t size = rd8(params + 1);
    instance *in = inst_of(rd32(params + 4 + size));
    gaddr storage = in ? in->storage : 0;
    uint32_t n = 8;
    if (in && in->storage && ARG32(8) == in->storage)
        n = 12; /* the WithStorage form passed it */
    cpu_push32(c, 0);
    cpu_push32(c, storage);
    c->a[7] -= size;
    gmemcpy(c->a[7], params + 4, size);
    guest_call(func);
    uint32_t r = cpu_pop32(c);
    finish(c, ret, n, 4, r);
}

void raw_ComponentDispatch(cpu68k *c, uint16_t tw)
{
    uint16_t sel = (uint16_t)c->d[0] & 0xFF;
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
        k = comp_of(ARG32(0));
        finish(c, ret, 4, 2, (uint16_t)(k && k->resfile >= 0 ? k->resfile : -1));
        return;
    case 0x18: /* CloseComponentResFile(refnum): OSErr -- the file stays open */
        finish(c, ret, 2, 2, noErr);
        return;
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
    case 0x2F: /* OpenAComponentResFile(c, *refnum): OSErr */
        k = comp_of(ARG32(4));
        wr16(ARG32(0), (uint16_t)(k && k->resfile >= 0 ? k->resfile : -1));
        finish(c, ret, 8, 2, k && k->resfile >= 0 ? noErr : (uint16_t)resFNotFound);
        return;
    case 0x35: { /* GetComponentResource(c, type, id, *h): OSErr */
        k = comp_of(ARG32(10));
        gaddr h = 0;
        if (k && k->resfile >= 0) {
            int16_t saved = tb_CurResFile();
            tb_UseResFile(k->resfile);
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
 * Explorer).  It holds no preferences: every lookup finds nothing, so
 * programs use their own defaults. */

enum {
    kICCStart = 0, kICCStop = 1, kICCFindConfigFile = 2, kICCSpecifyConfigFile = 3, kICCGetSeed = 4,
    kICCBegin = 5, kICCGetPref = 6, kICCSetPref = 7, kICCCountPref = 8, kICCGetIndPref = 9, kICCEnd = 10,
    kICCDefaultFileName = 11,
    kICCDeletePref = 12, kICCGetPerm = 13, kICCFindUserConfigFile = 14, kICCGetPrefHandle = 26,
    kICCFindPrefHandle = 36,
    icPrefNotFoundErr = -666,
};

static void ic_file_name(gaddr str)
{
    static const char name[] = "Internet Preferences";
    wr8(str, sizeof name - 1);
    gput(str + 1, name, sizeof name - 1);
}

/* An empty Internet Preferences file (a resource file) in the Preferences
 * folder, or a resource fork for one made without: programs open it
 * themselves and call it damaged when it has none (Internet Explorer). */
static void ic_make_file(void)
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
        r = rd16(params + 4) <= kICCFindPrefHandle || (int16_t)rd16(params + 4) < 0;
        break;
    case -4: /* kComponentVersionSelect */
        r = 0x00020000;
        break;
    case kICCGetSeed:
        if (last)
            wr32(last, 1);
        break;
    case kICCCountPref:
        if (last)
            wr32(last, 0);
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
    case kICCGetPref: case kICCGetIndPref: case kICCGetPrefHandle: case kICCFindPrefHandle: case kICCDeletePref:
        r = icPrefNotFoundErr;
        break;
    default: /* open, close, start, stop, begin, end, finding the configuration ... */
        break;
    }
    wr32(c->a[7], (uint32_t)r);
    cpu_push32(c, ret);
}

/* Forget everything registered: a new run. */
void components_reset(void)
{
    memset(comps, 0, sizeof comps);
    memset(insts, 0, sizeof insts);
    register_component(fourcc("PREF"), fourcc("ICAp"), fourcc("JPQE"), 0,
                       native_register(ic_component, "InternetConfig", 0), 0, 0, 0, -1);
    qt_tune_register(); /* QuickTime's tune player and note allocator */
}
