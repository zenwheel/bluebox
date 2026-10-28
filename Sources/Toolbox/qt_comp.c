/*
 * QuickTime's components of our own, written in C: the media handlers, data
 * handlers, clocks, codecs, exporters and the rest are registered with the
 * Component Manager like any other, so programs find, open and call them
 * as they did Apple's (and can capture or delegate to them), but their
 * routine is C, reached without 68k code between.
 *
 * Each class names the call families it implements (qt_calls.tsv); a call
 * comes to its routine with the arguments read out of the
 * ComponentParameters record in the order the interfaces declare them.
 * The standard selectors are answered here unless the routine does.
 */
#include <stdlib.h>
#include <string.h>

#include "qt_atoms.h"
#include "qt_comp.h"
#include "tb_internal.h"

void qt_dhlr_register(void); /* qt_dhlr.c */
void qt_mhlr_register(void); /* qt_mhlr.c */

#define MAX_SLOTS 1024 /* the Component Manager's instances (their IDs' low word) */

typedef struct {
    uint32_t ci; /* 0: free */
    const qtc_class *cls;
    void *self;
    uint32_t target;
} slot;

static slot slots[MAX_SLOTS];

static slot *slot_of(uint32_t ci)
{
    slot *s = &slots[ci & (MAX_SLOTS - 1)];
    return ci && s->ci == ci ? s : NULL;
}

void *qtc_self(uint32_t ci)
{
    slot *s = slot_of(ci);
    return s ? s->self : NULL;
}

const qtc_class *qtc_class_of(uint32_t ci)
{
    slot *s = slot_of(ci);
    return s ? s->cls : NULL;
}

/* The class's call with this selector. */
static const qt_call_info *class_call(const qtc_class *cls, int16_t what)
{
    for (int i = 0; i < 8 && cls->families[i]; i++) {
        const qt_call_info *q = qt_call_find(cls->families[i], what);
        if (q)
            return q;
    }
    return NULL;
}

/* Arguments from the parameter block: the first declared is pushed first,
 * so it is the highest. */
static void read_args(qtc_call *k, uint32_t size)
{
    uint32_t off = size;
    k->n = 0;
    for (int i = 0; k->call->sizes[i] && i < 32; i++) {
        char t = k->call->sizes[i];
        off -= t == '4' || t == 'f' ? 4 : 2;
        gaddr p = k->params + 4 + off;
        k->a[k->n++] = t == '4' || t == 'f' ? rd32(p) : t == '2' ? rd16(p) : rd8(p);
    }
}

static int32_t dispatch(uint32_t ci, gaddr params, void *cookie)
{
    const qtc_class *cls = cookie;
    qtc_call k = { .ci = ci, .what = (int16_t)rd16(params + 2), .params = params, .cls = cls };
    uint32_t size = rd8(params + 1);
    slot *s = slot_of(ci);
    if (k.what == kQTCOpen) {
        if (s)
            return noErr;
        s = &slots[ci & (MAX_SLOTS - 1)];
        *s = (slot){ ci, cls, calloc(1, cls->size ? cls->size : 1), ci };
        if (!s->self) {
            s->ci = 0;
            return memFullErr;
        }
        k.self = s->self;
        k.target = ci;
        int32_t r = cls->fn(&k);
        if (r == qtcBadSelector)
            r = noErr;
        if (r) {
            free(s->self);
            *s = (slot){ 0 };
        }
        return r;
    }
    if (!s)
        return qtcBadInstance;
    k.self = s->self;
    k.target = s->target;
    if (k.what == kQTCClose) {
        cls->fn(&k);
        free(s->self);
        *s = (slot){ 0 };
        return noErr;
    }
    if (k.what < 0) {
        if (size >= 2)
            k.a[0] = rd16(params + 4);
        if (size >= 4)
            k.a[0] = rd32(params + 4);
        k.n = size ? 1 : 0;
        int32_t r = cls->fn(&k);
        if (r != qtcBadSelector)
            return r;
        switch (k.what) {
        case kQTCCanDo: {
            int16_t sel = (int16_t)rd16(params + 4);
            return (sel <= -1 && sel >= -7) || (sel >= 0 && class_call(cls, sel));
        }
        case kQTCVersion:
            return cls->version ? cls->version : 0x00010000;
        case kQTCRegister:
        case kQTCUnregister:
            return noErr;
        case kQTCTarget:
            s->target = k.a[0] ? k.a[0] : ci;
            return noErr;
        }
        return qtcBadSelector;
    }
    k.call = class_call(cls, k.what);
    if (!k.call)
        return qtcBadSelector;
    read_args(&k, size);
    return cls->fn(&k);
}

uint32_t qtc_register(qtc_class *cls)
{
    cls->component = components_register_c(cls->type, cls->subtype, cls->manufacturer, cls->flags, cls->name,
                                           cls->info, dispatch, cls);
    return cls->component;
}

void qtc_each(const qtc_class *cls, void (*fn)(void *self, uint32_t ci))
{
    for (int i = 0; i < MAX_SLOTS; i++)
        if (slots[i].ci && slots[i].cls == cls)
            fn(slots[i].self, slots[i].ci);
}

uint32_t qtc_open(const qtc_class *cls)
{
    int16_t err;
    return cls->component ? components_open_component(cls->component, &err) : 0;
}

static int32_t call_with(uint32_t ci, const qt_call_info *q, const uint32_t *args)
{
    uint32_t size = qt_call_param_size(q->sizes);
    gaddr p = tb_NewPtr((int32_t)(4 + size + 4), false, true);
    if (!p)
        return memFullErr;
    wr8(p + 1, (uint8_t)size);
    wr16(p + 2, q->what);
    uint32_t off = size;
    for (int i = 0; q->sizes[i]; i++) {
        char t = q->sizes[i];
        off -= t == '4' || t == 'f' ? 4 : 2;
        if (t == '4' || t == 'f')
            wr32(p + 4 + off, args[i]);
        else if (t == '2')
            wr16(p + 4 + off, (uint16_t)args[i]);
        else
            wr16(p + 4 + off, (uint16_t)((args[i] & 0xFF) << 8));
    }
    wr32(p + 4 + size, ci);
    int32_t r = components_call(ci, p);
    tb_DisposePtr(p);
    return r;
}

int32_t qtc_call_instance(uint32_t ci, const char *family, int16_t what, const uint32_t *args)
{
    const qt_call_info *q = qt_call_find(family, what);
    return q ? call_with(ci, q, args) : qtcBadSelector;
}

int32_t qtc_call_named(uint32_t ci, const char *name, const uint32_t *args)
{
    unsigned lo = 0, hi = qt_call_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        int d = strcmp(qt_calls[mid].name, name);
        if (!d)
            return call_with(ci, &qt_calls[mid], args);
        if (d < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return qtcBadSelector;
}

/* A new run: our instances' states go with the old one, and the
 * components are registered again (components_reset). */
void qt_components_reset(void)
{
    for (int i = 0; i < MAX_SLOTS; i++)
        free(slots[i].self);
    memset(slots, 0, sizeof slots);
    qt_udata_reset();
    qt_dhlr_register();
    qt_mhlr_register();
}
