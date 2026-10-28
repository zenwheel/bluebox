/*
 * Tweens (QuickTime 3): values between two others at a point in time, from
 * a 'twen' atom in a QT atom container: its type ('twnt'), the start and
 * end values ('data'), when it starts and lasts ('twst', 'twdu'), whether
 * a change since the last call is wanted ('flag'), limits ('omin',
 * 'omax'), and sequences of other tweens ('seqe').  QTNewTween and
 * QTDoTween compute them, handing each value to the program's data
 * procedure or into a handle; the tweener components ('twen', one per
 * type) do the same for programs and media that call them.
 *
 * Not done: path tweens (they need QuickDraw GX paths), the 3D types,
 * polygons, and non-linear interpolation tweens ('intr'): their tweens
 * answer featureUnsupported.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "qt_atoms.h"
#include "qt_comp.h"
#include "qt_errors.h"
#include "qt_movie_internal.h"
#include "tb_internal.h"
#include "trap.h"

enum {
    kTweenTypeShort = 1, kTweenTypeLong = 2, kTweenTypeFixed = 3, kTweenTypePoint = 4, kTweenTypeQDRect = 5,
    kTweenTypeQDRegion = 6, kTweenTypeMatrix = 7, kTweenTypeRGBColor = 8, kTweenTypeGraphicsModeWithRGBColor = 9,
    kTweenTypeQTFloatSingle = 10, kTweenTypeQTFloatDouble = 11, kTweenTypeFixedPoint = 12,
    kTweenReturnDelta = 1,
};

static uint32_t four(const char *s) { return (uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 | (uint8_t)s[3]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }

/* The element layout of each type: the fields to interpolate ('h' short,
 * 'l' long, 'f' Fixed, 'r' Fract, 's' float, 'd' double, 'c' unsigned
 * short (colour), 'k' kept from the start (a mode)). */
static const char *layout(uint32_t type)
{
    switch (type) {
    case kTweenTypeShort: return "h";
    case kTweenTypeLong: return "l";
    case kTweenTypeFixed: return "f";
    case kTweenTypePoint: return "hh";
    case kTweenTypeQDRect: case kTweenTypeQDRegion: return "hhhh";
    case kTweenTypeMatrix: return "ffrffrffr";
    case kTweenTypeRGBColor: return "ccc";
    case kTweenTypeGraphicsModeWithRGBColor: return "kccc";
    case kTweenTypeQTFloatSingle: return "s";
    case kTweenTypeQTFloatDouble: return "d";
    case kTweenTypeFixedPoint: return "ff";
    }
    return NULL;
}

static size_t field_size(char f) { return f == 'h' || f == 'c' ? 2 : f == 'd' ? 8 : 4; }

static size_t value_size(const char *l)
{
    size_t n = 0;
    for (; *l; l++)
        n += field_size(*l);
    return n;
}

static double get_field(const uint8_t *p, char f)
{
    switch (f) {
    case 'h': return (int16_t)be16(p);
    case 'c': return be16(p);
    case 'l': case 'k': return (int32_t)be32(p);
    case 'f': return (int32_t)be32(p) / 65536.0;
    case 'r': return (int32_t)be32(p) / 1073741824.0;
    case 's': { uint32_t w = be32(p); float v; memcpy(&v, &w, 4); return v; }
    case 'd': {
        uint64_t w = (uint64_t)be32(p) << 32 | be32(p + 4);
        double v;
        memcpy(&v, &w, 8);
        return v;
    }
    }
    return 0;
}

static void put_field(uint8_t *p, char f, double v)
{
    switch (f) {
    case 'h': put16(p, (uint16_t)(int16_t)lround(v)); break;
    case 'c': put16(p, (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : lround(v))); break;
    case 'l': case 'k': put32(p, (uint32_t)(int32_t)llround(v)); break;
    case 'f': put32(p, (uint32_t)(int32_t)llround(v * 65536.0)); break;
    case 'r': put32(p, (uint32_t)(int32_t)llround(v * 1073741824.0)); break;
    case 's': { float x = (float)v; uint32_t w; memcpy(&w, &x, 4); put32(p, w); break; }
    case 'd': {
        uint64_t w;
        memcpy(&w, &v, 8);
        put32(p, (uint32_t)(w >> 32));
        put32(p + 4, (uint32_t)w);
        break;
    }
    }
}

/* A container's bytes. */
typedef struct {
    uint8_t *c;
    size_t n;
} bytes;

static bytes container_bytes(gaddr h)
{
    bytes b = { NULL, 0 };
    if (!h || !rd32(h))
        return b;
    b.n = (size_t)tb_GetHandleSize(h);
    b.c = malloc(b.n ? b.n : 1);
    if (b.c)
        gget(b.c, rd32(h), (uint32_t)b.n);
    return b;
}

static const uint8_t *leaf(const bytes *b, uint32_t parent, uint32_t type, uint32_t id, uint32_t *len)
{
    *len = 0;
    uint32_t a = id ? qta_find(b->c, b->n, parent, type, id) : qta_index(b->c, b->n, parent, type, 1, NULL);
    return a ? qta_data(b->c, b->n, a, len) : NULL;
}

/* A tween's value at percent (0 to 1) into out (its size in *len): from
 * the data atom (the start then the end), within limits. */
static int16_t value_at(const bytes *b, uint32_t tween, uint32_t data_atom, double percent, uint8_t *out, size_t *len,
                        uint32_t *type_out)
{
    uint32_t n;
    const uint8_t *t = leaf(b, tween, four("twnt"), 0, &n);
    uint32_t type = t && n >= 4 ? be32(t) : 0;
    if (type_out)
        *type_out = type;
    /* a sequence: the entry for the percent, as a part of its own */
    int seqs = qta_count(b->c, b->n, tween, four("seqe"));
    if (seqs) {
        double from = 0;
        for (int i = 1; i <= seqs; i++) {
            uint32_t a = qta_index(b->c, b->n, tween, four("seqe"), i, NULL), sl;
            const uint8_t *e = qta_data(b->c, b->n, a, &sl);
            if (!e || sl < 12)
                continue;
            double to = (int32_t)be32(e) / 65536.0;
            if (percent <= to || i == seqs) {
                uint32_t sub = qta_find(b->c, b->n, 0, four("twen"), be32(e + 4));
                uint32_t sd = sub ? qta_find(b->c, b->n, sub, four("data"), be32(e + 8)) : 0;
                double local = to > from ? (percent - from) / (to - from) : 1;
                return sub ? value_at(b, sub, sd, local < 0 ? 0 : local > 1 ? 1 : local, out, len, type_out)
                           : invalidAtomErr;
            }
            from = to;
        }
    }
    const char *l = layout(type);
    if (!l)
        return featureUnsupported;
    size_t size = value_size(l);
    uint32_t dl;
    const uint8_t *d = data_atom ? qta_data(b->c, b->n, data_atom, &dl) : leaf(b, tween, four("data"), 0, &dl);
    if (!d || dl < 2 * size)
        return invalidAtomErr;
    uint32_t ml, xl;
    const uint8_t *mn = leaf(b, tween, four("omin"), 0, &ml), *mx = leaf(b, tween, four("omax"), 0, &xl);
    size_t at = 0;
    for (const char *f = l; *f; f++) {
        double a = get_field(d + at, *f), z = get_field(d + size + at, *f);
        double v = *f == 'k' ? a : a + (z - a) * percent;
        if (mn && ml >= at + field_size(*f) && v < get_field(mn + at, *f))
            v = get_field(mn + at, *f);
        if (mx && xl >= at + field_size(*f) && v > get_field(mx + at, *f))
            v = get_field(mx + at, *f);
        put_field(out + at, *f, v);
        at += field_size(*f);
    }
    *len = size;
    return noErr;
}

/* A rectangular region of a QDRect value (big-endian), its handle. */
static gaddr region_of(const uint8_t *r)
{
    gaddr h = tb_NewRgn();
    gaddr rect = tb_NewPtr(8, false, true);
    gput(rect, r, 8);
    tb_RectRgn(h, rect);
    tb_DisposePtr(rect);
    return h;
}

/* ---- QTNewTween, QTDoTween ---- */

#define MAX_TWEENS 64
typedef struct {
    gaddr token;         /* the QTTweener the program holds; 0 free */
    bytes b;
    uint32_t atom;
    int32_t max_time;
    uint8_t last[64];    /* the last value, for deltas */
    bool have_last;
} tweener;
static tweener tweens[MAX_TWEENS];

static tweener *tween_of(gaddr token)
{
    for (int i = 0; token && i < MAX_TWEENS; i++)
        if (tweens[i].token == token)
            return &tweens[i];
    return NULL;
}

int16_t tb_QTNewTween(gaddr tween, int32_t container, int32_t tweenAtom, int32_t maxTime)
{
    if (tween)
        wr32(tween, 0);
    if (!tween || !container)
        return qt_set_error(paramErr);
    tweener *t = NULL;
    for (int i = 0; i < MAX_TWEENS && !t; i++)
        if (!tweens[i].token)
            t = &tweens[i];
    if (!t)
        return qt_set_error(memFullErr);
    bytes b = container_bytes((gaddr)container);
    uint32_t n;
    if (!b.c || qta_type(b.c, (uint32_t)tweenAtom) != four("twen")) {
        free(b.c);
        return qt_set_error(invalidAtomErr);
    }
    const uint8_t *ty = qta_data(b.c, b.n, qta_index(b.c, b.n, (uint32_t)tweenAtom, four("twnt"), 1, NULL), &n);
    if (!ty || n < 4 || (!layout(be32(ty)) && !qta_count(b.c, b.n, (uint32_t)tweenAtom, four("seqe")))) {
        free(b.c);
        return qt_set_error(ty ? featureUnsupported : invalidAtomErr);
    }
    *t = (tweener){ tb_NewPtr(4, false, true), b, (uint32_t)tweenAtom, maxTime, { 0 }, false };
    wr32(tween, t->token);
    return qt_set_error(noErr);
}

int16_t tb_QTDisposeTween(int32_t tween)
{
    tweener *t = tween_of((gaddr)tween);
    if (!t)
        return qt_set_error(paramErr);
    free(t->b.c);
    tb_DisposePtr(t->token);
    memset(t, 0, sizeof *t);
    return qt_set_error(noErr);
}

/* The value at a time: its percent of the tween's duration from its start. */
static int16_t tween_value(tweener *t, int32_t at, uint8_t *out, size_t *len, uint32_t *type, double *percent)
{
    uint32_t n;
    const uint8_t *st = leaf(&t->b, t->atom, four("twst"), 0, &n);
    int32_t start = st && n >= 4 ? (int32_t)be32(st) : 0;
    const uint8_t *du = leaf(&t->b, t->atom, four("twdu"), 0, &n);
    int32_t dur = du && n >= 4 ? (int32_t)be32(du) : t->max_time;
    double p = dur > 0 ? (double)(at - start) / dur : 1;
    p = p < 0 ? 0 : p > 1 ? 1 : p;
    *percent = p;
    int16_t err = value_at(&t->b, t->atom, 0, p, out, len, type);
    if (err)
        return err;
    const uint8_t *fl = leaf(&t->b, t->atom, four("flag"), 0, &n);
    if (fl && n >= 4 && (be32(fl) & kTweenReturnDelta)) {
        uint8_t now[64];
        memcpy(now, out, *len);
        const char *l = layout(*type);
        size_t o = 0;
        for (; l && *l; l++) {
            double v = get_field(out + o, *l) - (t->have_last ? get_field(t->last + o, *l) : get_field(out + o, *l));
            put_field(out + o, *l, v);
            o += field_size(*l);
        }
        memcpy(t->last, now, *len);
        t->have_last = true;
    }
    return noErr;
}

/* TweenerDataProc(TweenRecord *tr, void *data, long size, long seed, Handle desc,
 * ICMCompletionProcRecordPtr, UniversalProcPtr transferProc, void *refCon). */
static int32_t call_data_proc(gaddr proc, gaddr record, gaddr data, uint32_t size, gaddr refcon)
{
    cpu68k *c = g_cpu;
    cpu_push32(c, 0);
    cpu_push32(c, record);
    cpu_push32(c, data);
    cpu_push32(c, size);
    cpu_push32(c, 0);
    cpu_push32(c, 0);
    cpu_push32(c, 0);
    cpu_push32(c, 0);
    cpu_push32(c, refcon);
    guest_call(proc);
    return (int32_t)cpu_pop32(c);
}

static int16_t deliver(gaddr container, uint32_t atom, uint32_t type, double percent, const uint8_t *v, size_t len,
                       gaddr result, gaddr result_size, gaddr proc, gaddr refcon, bool ptr)
{
    /* a region type delivers a RgnHandle */
    gaddr region = type == kTweenTypeQDRegion ? region_of(v) : 0;
    uint32_t n = region ? 4 : (uint32_t)len;
    gaddr data = tb_NewPtr((int32_t)n, false, true);
    if (region)
        wr32(data, region);
    else
        gput(data, v, n);
    int32_t r = noErr;
    if (proc) {
        gaddr tr = tb_NewPtr(40, false, true); /* TweenV1Record */
        wr32(tr, 1);
        wr32(tr + 4, container);
        wr32(tr + 8, atom);
        wr32(tr + 16, (uint32_t)(int32_t)lround(percent * 65536));
        wr32(tr + 20, proc);
        wr32(tr + 32, (uint32_t)(int32_t)lround(percent * 1073741824.0));
        r = call_data_proc(proc, tr, data, n, refcon);
        tb_DisposePtr(tr);
    } else if (ptr) {
        if (result && (!result_size || n <= (uint32_t)result_size))
            gmemcpy(result, data, n);
        else
            r = paramErr;
    } else if (result) {
        tb_SetHandleSize(result, (int32_t)n);
        if ((uint32_t)tb_GetHandleSize(result) == n)
            gmemcpy(rd32(result), data, n);
        else
            r = memFullErr;
        if (result_size)
            wr32(result_size, n);
    }
    tb_DisposePtr(data);
    if (region && proc) /* the procedure had it for the call */
        tb_DisposeRgn(region);
    return (int16_t)r;
}

int16_t tb_QTDoTween(int32_t tween, int32_t atTime, int32_t result, gaddr resultSize, int32_t tweenDataProc,
                     gaddr tweenDataRefCon)
{
    tweener *t = tween_of((gaddr)tween);
    if (!t)
        return qt_set_error(paramErr);
    uint8_t v[64];
    size_t len;
    uint32_t type;
    double p;
    int16_t err = tween_value(t, atTime, v, &len, &type, &p);
    if (err)
        return qt_set_error(err);
    return qt_set_error(deliver(0, t->atom, type, p, v, len, (gaddr)result, resultSize, (gaddr)tweenDataProc,
                                tweenDataRefCon, false));
}

int16_t tb_QTDoTweenPtr(int32_t tween, int32_t atTime, int32_t result, int32_t resultSize)
{
    tweener *t = tween_of((gaddr)tween);
    if (!t)
        return qt_set_error(paramErr);
    uint8_t v[64];
    size_t len;
    uint32_t type;
    double p;
    int16_t err = tween_value(t, atTime, v, &len, &type, &p);
    if (err)
        return qt_set_error(err);
    return qt_set_error(deliver(0, t->atom, type, p, v, len, (gaddr)result, (gaddr)resultSize, 0, 0, true));
}

/* ---- Tweener components ---- */

typedef struct {
    gaddr container;
    uint32_t tween, data;
} twen_state;

static int32_t twen_call(qtc_call *k)
{
    twen_state *st = k->self;
    if (!k->call)
        return qtcBadSelector;
    switch (k->what) {
    case 1: /* TweenerInitialize(tc, container, tweenAtom, dataAtom) */
        st->container = k->a[0];
        st->tween = k->a[1];
        st->data = k->a[2];
        return noErr;
    case 2: { /* TweenerDoTween(tc, TweenRecord *tr) */
        gaddr tr = k->a[0];
        if (!tr)
            return paramErr;
        gaddr container = rd32(tr + 4) ? rd32(tr + 4) : st->container;
        uint32_t atom = rd32(tr + 8) ? rd32(tr + 8) : st->tween, data = rd32(tr + 12) ? rd32(tr + 12) : st->data;
        double p = (int32_t)rd32(tr + 16) / 65536.0;
        bytes b = container_bytes(container);
        if (!b.c)
            return paramErr;
        uint8_t v[64];
        size_t len;
        uint32_t type;
        int16_t err = value_at(&b, atom, data, p < 0 ? 0 : p > 1 ? 1 : p, v, &len, &type);
        free(b.c);
        if (err)
            return err;
        gaddr proc = rd32(tr + 20);
        return proc ? deliver(container, atom, type, p, v, len, 0, 0, proc, rd32(tr + 24), false) : noErr;
    }
    case 3: /* TweenerReset */
        return noErr;
    }
    return qtcBadSelector;
}

#define TWEN(sub, name) \
    { 0x7477656E, sub, 0x6170706C, 0, name, NULL, 0x00030000, { "Tweener" }, sizeof(twen_state), twen_call, 0 }
static qtc_class tweeners[] = {
    TWEN(kTweenTypeShort, "Short Tweener"), TWEN(kTweenTypeLong, "Long Tweener"),
    TWEN(kTweenTypeFixed, "Fixed Tweener"), TWEN(kTweenTypePoint, "Point Tweener"),
    TWEN(kTweenTypeQDRect, "Rect Tweener"), TWEN(kTweenTypeQDRegion, "Region Tweener"),
    TWEN(kTweenTypeMatrix, "Matrix Tweener"), TWEN(kTweenTypeRGBColor, "RGB Color Tweener"),
    TWEN(kTweenTypeGraphicsModeWithRGBColor, "Graphics Mode Tweener"),
    TWEN(kTweenTypeQTFloatSingle, "Float Tweener"), TWEN(kTweenTypeQTFloatDouble, "Double Tweener"),
    TWEN(kTweenTypeFixedPoint, "Fixed Point Tweener"),
};

void qt_tween_register(void)
{
    for (int i = 0; i < MAX_TWEENS; i++)
        free(tweens[i].b.c);
    memset(tweens, 0, sizeof tweens);
    for (size_t i = 0; i < sizeof tweeners / sizeof tweeners[0]; i++)
        qtc_register(&tweeners[i]);
}
