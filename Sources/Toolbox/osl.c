/*
 * Object Support Library: object accessors, AEResolve, and the object
 * model's descriptors (CreateObjSpecifier and the rest).
 *
 * AEResolve walks an object specifier (an 'obj ' record: want, from, form,
 * seld) from the outside in.  The container resolves first (a null
 * container is the application, whose token is null), then the accessor
 * installed for the desired class and the container token's type gives
 * the token.  A container that resolved to a list of tokens has the
 * accessor applied to each.  Ranges resolve their bounds within the
 * container and pass the accessor a range of tokens; tests ('cmpd' and
 * 'logi') are evaluated here, element by element, with the application's
 * count and compare callbacks, unless it does whose clauses itself
 * (kAEIDoWhose, which gets the test as it came).  The callbacks are
 * special handlers, as AESetObjectCallbacks installs them.
 */
#include "machine.h"
#include "tb_internal.h"

#define T(s) fourcc(s)

enum {
    errAEEventNotHandled = -1708, errAEHandlerNotFound = -1717, errAEAccessorNotFound = -1723,
    errAENoSuchLogical = -1726, errAENotAnObjSpec = -1727, errAENoSuchObject = -1728,
    errAEBadTestKey = -1730,
};

enum { kAEIDoWhose = 1 };

static void set_desc(gaddr desc, uint32_t type, gaddr h)
{
    AEDesc_set_descriptorType(desc, (int32_t)type);
    AEDesc_set_dataHandle(desc, h);
}

static uint32_t desc_type(gaddr desc) { return (uint32_t)AEDesc_descriptorType(desc); }

static gaddr new_desc(void) { return tb_NewPtr(AEDesc_SIZEOF, false, true); }

static void free_desc(gaddr d)
{
    tb_AEDisposeDesc(d);
    tb_DisposePtr(d);
}

/* A four-character parameter of a record, or 0. */
static uint32_t param_code(gaddr rec, uint32_t key, uint32_t type)
{
    gaddr v = tb_NewPtr(4, false, true);
    uint32_t r = tb_AEGetParamPtr(rec, (int32_t)key, (int32_t)type, 0, v, 4, 0) == noErr ? rd32(v) : 0;
    tb_DisposePtr(v);
    return r;
}

/* ---------------------------------------------------------------------- */
/* Accessors, the application's [0] and the system's [1] */

typedef struct {
    uint32_t desired, container;
    gaddr accessor;
    int32_t refcon;
} accessor_entry;

static accessor_entry accessors[2][64];
static int naccessors[2];

int16_t tb_AEInstallObjectAccessor(int32_t desired, int32_t container, gaddr accessor, int32_t refcon, int8_t sys)
{
    mm_note_proc(accessor);
    int s = sys != 0;
    for (int i = 0; i < naccessors[s]; i++)
        if (accessors[s][i].desired == (uint32_t)desired && accessors[s][i].container == (uint32_t)container) {
            accessors[s][i].accessor = accessor;
            accessors[s][i].refcon = refcon;
            return noErr;
        }
    if (naccessors[s] >= 64)
        return memFullErr;
    accessors[s][naccessors[s]++] = (accessor_entry){ (uint32_t)desired, (uint32_t)container, accessor, refcon };
    return noErr;
}

int16_t tb_AERemoveObjectAccessor(int32_t desired, int32_t container, gaddr accessor, int8_t sys)
{
    int s = sys != 0;
    for (int i = 0; i < naccessors[s]; i++)
        if (accessors[s][i].desired == (uint32_t)desired && accessors[s][i].container == (uint32_t)container) {
            accessors[s][i] = accessors[s][--naccessors[s]];
            return noErr;
        }
    return errAEHandlerNotFound;
}

int16_t tb_AEGetObjectAccessor(int32_t desired, int32_t container, gaddr accessor_out, gaddr refcon_out, int8_t sys)
{
    int s = sys != 0;
    for (int i = 0; i < naccessors[s]; i++)
        if (accessors[s][i].desired == (uint32_t)desired && accessors[s][i].container == (uint32_t)container) {
            wr32(accessor_out, accessors[s][i].accessor);
            wr32(refcon_out, (uint32_t)accessors[s][i].refcon);
            return noErr;
        }
    return errAEAccessorNotFound;
}

/* The accessor for a class in a container of a token type: exact, then
 * wildcards; the application's, then the system's. */
static const accessor_entry *find_accessor(uint32_t desired, uint32_t container)
{
    const uint32_t any = T("****");
    const uint32_t tries[4][2] = { { desired, container }, { desired, any }, { any, container }, { any, any } };
    for (int s = 0; s < 2; s++)
        for (int t = 0; t < 4; t++)
            for (int i = 0; i < naccessors[s]; i++)
                if (accessors[s][i].desired == tries[t][0] && accessors[s][i].container == tries[t][1])
                    return &accessors[s][i];
    return NULL;
}

int16_t tb_AECallObjectAccessor(int32_t desired, gaddr container_token, int32_t container_class, int32_t key_form,
                                gaddr key_data, gaddr token)
{
    const accessor_entry *a = find_accessor((uint32_t)desired, desc_type(container_token));
    set_desc(token, T("null"), 0);
    if (!a)
        return errAEAccessorNotFound;
    /* pascal OSErr (DescType, const AEDesc *, DescType, DescType, const AEDesc *, AEDesc *, long) */
    uint32_t args[7] = { (uint32_t)desired, container_token, (uint32_t)container_class, (uint32_t)key_form,
                         key_data, token, (uint32_t)a->refcon };
    return (int16_t)aem_call_proc(a->accessor, 0x000FFFE0, args, 7);
}

/* ---------------------------------------------------------------------- */
/* Callbacks */

int16_t tb_AESetObjectCallbacks(gaddr compare, gaddr count, gaddr dispose, gaddr getmark, gaddr mark, gaddr adjust,
                                gaddr errdesc)
{
    const gaddr procs[7] = { compare, count, dispose, getmark, mark, adjust, errdesc };
    for (int i = 0; i < 7; i++)
        mm_note_proc(procs[i]);
    static const char keys[7][5] = { "cmpr", "cont", "xtok", "mkid", "mark", "adjm", "indc" };
    for (int i = 0; i < 7; i++)
        if (procs[i])
            tb_AEInstallSpecialHandler((int32_t)T(keys[i]), procs[i], 0);
    return noErr;
}

int16_t tb_AEObjectInit(void) { return noErr; }

/* A token's disposal: the application's callback, else AEDisposeDesc; a
 * list of tokens item by item. */
int16_t tb_AEDisposeToken(gaddr token)
{
    if (!token)
        return noErr;
    if (desc_type(token) == T("list")) {
        gaddr count = tb_NewPtr(4, false, true);
        if (tb_AECountItems(token, count) == noErr) {
            gaddr item = new_desc();
            for (uint32_t i = 1; i <= rd32(count); i++)
                if (tb_AEGetNthDesc(token, (int32_t)i, (int32_t)T("****"), 0, item) == noErr)
                    tb_AEDisposeToken(item);
            tb_DisposePtr(item);
        }
        tb_DisposePtr(count);
        return tb_AEDisposeDesc(token);
    }
    gaddr proc = aem_special_handler(T("xtok"));
    if (proc && !(desc_type(token) == T("null") && !AEDesc_dataHandle(token))) { /* not the application's */
        uint32_t args[1] = { token };
        int16_t err = (int16_t)aem_call_proc(proc, 0x000000E0 /* pascal OSErr (AEDesc *) */, args, 1);
        if (err != errAEEventNotHandled) {
            set_desc(token, T("null"), 0);
            return err;
        }
    }
    return tb_AEDisposeDesc(token);
}

static int16_t compare(uint32_t op, gaddr a, gaddr b, bool *result)
{
    gaddr proc = aem_special_handler(T("cmpr"));
    if (!proc)
        return errAEEventNotHandled;
    gaddr r = tb_NewPtr(2, false, true);
    /* pascal OSErr (DescType, const AEDesc *, const AEDesc *, Boolean *) */
    uint32_t args[4] = { op, a, b, r };
    int16_t err = (int16_t)aem_call_proc(proc, 0x00003FE0, args, 4);
    *result = rd8(r) != 0;
    tb_DisposePtr(r);
    return err;
}

static int16_t count_elements(uint32_t desired, uint32_t container_class, gaddr container, uint32_t *n)
{
    gaddr proc = aem_special_handler(T("cont"));
    if (!proc)
        return errAEEventNotHandled;
    gaddr r = tb_NewPtr(4, false, true);
    /* pascal OSErr (DescType, DescType, const AEDesc *, long *) */
    uint32_t args[4] = { desired, container_class, container, r };
    int16_t err = (int16_t)aem_call_proc(proc, 0x00003FE0, args, 4);
    *n = rd32(r);
    tb_DisposePtr(r);
    return err;
}

/* ---------------------------------------------------------------------- */
/* Resolving */

/* What the resolution of a specifier works within: the element a test is
 * examining ('exmn') and the container a range's bounds lie in ('ccnt'),
 * with their classes. */
typedef struct {
    gaddr examined, current;
    uint32_t examined_class, current_class;
    int16_t flags;
} context;

/* *borrowed: the token is the examined element's or the range's
 * container's (the same handle), for the caller to forget, not dispose. */
static int16_t resolve(gaddr spec, const context *cx, gaddr token, uint32_t *cls, bool *borrowed);

/* The accessor on one container token, or on each of a list of them. */
static int16_t access(uint32_t want, gaddr ctok, uint32_t cclass, uint32_t form, gaddr seld, gaddr token)
{
    if (desc_type(ctok) != T("list") || find_accessor(want, T("list")))
        return tb_AECallObjectAccessor((int32_t)want, ctok, (int32_t)cclass, (int32_t)form, seld, token);
    gaddr count = tb_NewPtr(4, false, true);
    int16_t err = tb_AECountItems(ctok, count);
    uint32_t n = err ? 0 : rd32(count);
    tb_DisposePtr(count);
    if (err)
        return err;
    tb_AECreateList(0, 0, 0, token);
    gaddr item = new_desc(), result = new_desc();
    for (uint32_t i = 1; i <= n && !err; i++) {
        err = tb_AEGetNthDesc(ctok, (int32_t)i, (int32_t)T("****"), 0, item);
        if (!err)
            err = tb_AECallObjectAccessor((int32_t)want, item, (int32_t)cclass, (int32_t)form, seld, result);
        if (!err)
            err = tb_AEPutDesc(token, 0, result);
        tb_AEDisposeDesc(item);
        tb_AEDisposeDesc(result);
    }
    tb_DisposePtr(item);
    tb_DisposePtr(result);
    if (err)
        tb_AEDisposeToken(token);
    return err;
}

/* An operand of a comparison: an object resolved (within the element
 * examined), or a value as it is.  *resolved says which. */
static int16_t operand(gaddr d, const context *cx, gaddr out, bool *resolved)
{
    uint32_t t = desc_type(d);
    *resolved = false;
    if (t != T("obj ") && t != T("exmn"))
        return tb_AEDuplicateDesc(d, out);
    uint32_t cls;
    bool borrowed;
    int16_t err = resolve(d, cx, out, &cls, &borrowed);
    *resolved = !borrowed;
    return err;
}

/* Whether the element examined passes a test: a comparison ('cmpd':
 * relo, obj1, obj2) or a logical term ('logi': logc, term). */
static int16_t evaluate(gaddr test, const context *cx, bool *pass)
{
    uint32_t t = desc_type(test);
    if (t == T("logi")) {
        uint32_t op = param_code(test, T("logc"), T("enum"));
        gaddr terms = new_desc(), term = new_desc(), count = tb_NewPtr(4, false, true);
        int16_t err = tb_AEGetParamDesc(test, (int32_t)T("term"), (int32_t)T("list"), terms);
        if (!err)
            err = tb_AECountItems(terms, count);
        uint32_t n = err ? 0 : rd32(count);
        bool all = true, any = false;
        for (uint32_t i = 1; i <= n && !err; i++) {
            bool p = false;
            err = tb_AEGetNthDesc(terms, (int32_t)i, (int32_t)T("****"), 0, term);
            if (!err)
                err = evaluate(term, cx, &p);
            tb_AEDisposeDesc(term);
            all = all && p;
            any = any || p;
            if (op == T("NOT "))
                break;
        }
        free_desc(terms);
        tb_DisposePtr(term);
        tb_DisposePtr(count);
        if (op == T("AND "))
            *pass = all;
        else if (op == T("OR  "))
            *pass = any;
        else if (op == T("NOT "))
            *pass = !any;
        else if (!err)
            err = errAENoSuchLogical;
        return err;
    }
    if (t != T("cmpd"))
        return errAEBadTestKey;
    uint32_t op = param_code(test, T("relo"), T("enum"));
    gaddr d1 = new_desc(), d2 = new_desc(), v1 = new_desc(), v2 = new_desc();
    bool r1 = false, r2 = false;
    int16_t err = tb_AEGetParamDesc(test, (int32_t)T("obj1"), (int32_t)T("****"), d1);
    if (!err)
        err = tb_AEGetParamDesc(test, (int32_t)T("obj2"), (int32_t)T("****"), d2);
    if (!err)
        err = operand(d1, cx, v1, &r1);
    if (!err)
        err = operand(d2, cx, v2, &r2);
    if (!err)
        err = compare(op, v1, v2, pass);
    free_desc(d1);
    free_desc(d2);
    if (r1)
        tb_AEDisposeToken(v1);
    if (r2)
        tb_AEDisposeToken(v2);
    tb_DisposePtr(v1); /* each disposed of, or borrowed, or a value: */
    tb_DisposePtr(v2);
    return err;
}

/* formTest: count the elements, get each by index and keep those that
 * pass, as a list of tokens. */
static int16_t select_elements(uint32_t want, gaddr ctok, uint32_t cclass, gaddr test, const context *cx,
                               gaddr token)
{
    uint32_t n;
    int16_t err = count_elements(want, cclass, ctok, &n);
    if (err)
        return err;
    tb_AECreateList(0, 0, 0, token);
    gaddr index = new_desc(), elem = new_desc();
    for (uint32_t i = 1; i <= n && !err; i++) {
        gaddr v = tb_NewPtr(4, false, true);
        wr32(v, i);
        tb_AECreateDesc((int32_t)T("long"), v, 4, index);
        tb_DisposePtr(v);
        err = access(want, ctok, cclass, T("indx"), index, elem);
        tb_AEDisposeDesc(index);
        if (err)
            break;
        context inner = *cx;
        inner.examined = elem;
        inner.examined_class = want;
        bool pass = false;
        err = evaluate(test, &inner, &pass);
        if (!err && pass)
            err = tb_AEPutDesc(token, 0, elem); /* the list keeps the token */
        else if (!err)
            tb_AEDisposeToken(elem);
        tb_AEDisposeDesc(elem);
    }
    tb_DisposePtr(index);
    tb_DisposePtr(elem);
    if (err)
        tb_AEDisposeToken(token);
    return err;
}

/* formRange: the bounds resolve within the container (their 'ccnt'), and
 * the accessor gets a range record of their tokens. */
static int16_t access_range(uint32_t want, gaddr ctok, uint32_t cclass, gaddr seld, const context *cx, gaddr token)
{
    context inner = *cx;
    inner.current = ctok;
    inner.current_class = cclass;
    gaddr bound = new_desc(), btok = new_desc(), range = new_desc();
    tb_AECreateList(0, 0, 1, range);
    static const char keys[2][5] = { "star", "stop" };
    int16_t err = noErr;
    bool borrowed[2] = { false, false };
    for (int i = 0; i < 2 && !err; i++) {
        uint32_t bcls;
        err = tb_AEGetParamDesc(seld, (int32_t)T(keys[i]), (int32_t)T("****"), bound);
        if (!err)
            err = resolve(bound, &inner, btok, &bcls, &borrowed[i]);
        if (!err)
            err = tb_AEPutParamDesc(range, (int32_t)T(keys[i]), btok);
        tb_AEDisposeDesc(bound);
        if (!borrowed[i])
            tb_AEDisposeDesc(btok);
    }
    if (!err) {
        gaddr r = new_desc();
        tb_AECoerceDesc(range, (int32_t)T("rang"), r);
        err = access(want, ctok, cclass, T("rang"), r, token);
        free_desc(r);
    }
    /* The bounds' tokens were the accessor's to read, not to keep. */
    for (int i = 0; i < 2; i++)
        if (tb_AEGetParamDesc(range, (int32_t)T(keys[i]), (int32_t)T("****"), btok) == noErr) {
            if (!borrowed[i])
                tb_AEDisposeToken(btok);
            tb_AEDisposeDesc(btok);
        }
    free_desc(range);
    tb_DisposePtr(bound);
    tb_DisposePtr(btok);
    return err;
}

static int16_t resolve(gaddr spec, const context *cx, gaddr token, uint32_t *cls, bool *borrowed)
{
    uint32_t t = desc_type(spec);
    set_desc(token, T("null"), 0);
    *borrowed = false;
    if (t == T("null")) { /* the application */
        *cls = T("null");
        return noErr;
    }
    if (t == T("exmn") || t == T("ccnt")) { /* the element examined, the range's container */
        gaddr from = t == T("exmn") ? cx->examined : cx->current;
        if (!from)
            return errAENoSuchObject;
        *cls = t == T("exmn") ? cx->examined_class : cx->current_class;
        gmemcpy(token, from, AEDesc_SIZEOF);
        *borrowed = true;
        return noErr;
    }
    if (t != T("obj "))
        return errAENotAnObjSpec;
    uint32_t want = param_code(spec, T("want"), T("type"));
    uint32_t form = param_code(spec, T("form"), T("enum"));
    gaddr from = new_desc(), seld = new_desc(), ctok = new_desc();
    uint32_t cclass = T("null");
    int16_t err = tb_AEGetParamDesc(spec, (int32_t)T("from"), (int32_t)T("****"), from);
    if (err == -1701) { /* errAEDescNotFound: the application */
        set_desc(from, T("null"), 0);
        err = noErr;
    }
    if (!err)
        err = tb_AEGetParamDesc(spec, (int32_t)T("seld"), (int32_t)T("****"), seld);
    bool shared = false;
    if (!err)
        err = resolve(from, cx, ctok, &cclass, &shared);
    if (!err) {
        if (form == T("rang"))
            err = access_range(want, ctok, cclass, seld, cx, token);
        else if (form == T("test") && !(cx->flags & kAEIDoWhose))
            err = select_elements(want, ctok, cclass, seld, cx, token);
        else
            err = access(want, ctok, cclass, form, seld, token);
        if (!shared)
            tb_AEDisposeToken(ctok);
    }
    free_desc(from);
    free_desc(seld);
    tb_DisposePtr(ctok);
    *cls = want;
    return err;
}

int16_t tb_AEResolve(gaddr spec, int16_t flags, gaddr token)
{
    context cx = { 0, 0, 0, 0, flags };
    uint32_t cls;
    bool borrowed;
    if (!spec) {
        set_desc(token, T("null"), 0);
        return noErr;
    }
    int16_t err = resolve(spec, &cx, token, &cls, &borrowed);
    if (err) {
        /* The specifier that failed, for the application's error reply. */
        gaddr proc = aem_special_handler(T("indc"));
        if (proc) {
            gaddr where = tb_NewPtr(4, false, true);
            uint32_t args[1] = { where };
            if ((int16_t)aem_call_proc(proc, 0x000000E0 /* pascal OSErr (AEDesc **) */, args, 1) == noErr &&
                rd32(where))
                tb_AEDuplicateDesc(spec, rd32(where));
            tb_DisposePtr(where);
        }
    }
    return err;
}

/* ---------------------------------------------------------------------- */
/* The object model's descriptors */

static void dispose_inputs(int8_t dispose, gaddr a, gaddr b)
{
    if (!dispose)
        return;
    if (a)
        tb_AEDisposeDesc(a);
    if (b)
        tb_AEDisposeDesc(b);
}

/* A record with these parameters (a 4-character code first), as `type`. */
static int16_t make_record(uint32_t type, uint32_t code_key, uint32_t code_type, uint32_t code, uint32_t k1,
                           gaddr d1, uint32_t k2, gaddr d2, gaddr out)
{
    gaddr rec = new_desc(), v = tb_NewPtr(4, false, true);
    wr32(v, code);
    int16_t err = tb_AECreateList(0, 0, 1, rec);
    if (!err)
        err = tb_AEPutParamPtr(rec, (int32_t)code_key, (int32_t)code_type, v, 4);
    if (!err && k1)
        err = d1 ? tb_AEPutParamDesc(rec, (int32_t)k1, d1) : tb_AEPutParamPtr(rec, (int32_t)k1, (int32_t)T("null"), 0, 0);
    if (!err && k2)
        err = d2 ? tb_AEPutParamDesc(rec, (int32_t)k2, d2) : tb_AEPutParamPtr(rec, (int32_t)k2, (int32_t)T("null"), 0, 0);
    if (!err)
        err = tb_AECoerceDesc(rec, (int32_t)type, out);
    free_desc(rec);
    tb_DisposePtr(v);
    return err;
}

int16_t tb_CreateObjSpecifier(int32_t desired_class, gaddr container, int32_t key_form, gaddr key_data,
                              int8_t dispose, gaddr spec)
{
    gaddr rec = new_desc(), v = tb_NewPtr(4, false, true);
    int16_t err = tb_AECreateList(0, 0, 1, rec);
    wr32(v, (uint32_t)desired_class);
    if (!err)
        err = tb_AEPutParamPtr(rec, (int32_t)T("want"), (int32_t)T("type"), v, 4);
    if (!err)
        err = container ? tb_AEPutParamDesc(rec, (int32_t)T("from"), container)
                        : tb_AEPutParamPtr(rec, (int32_t)T("from"), (int32_t)T("null"), 0, 0);
    wr32(v, (uint32_t)key_form);
    if (!err)
        err = tb_AEPutParamPtr(rec, (int32_t)T("form"), (int32_t)T("enum"), v, 4);
    if (!err)
        err = tb_AEPutParamDesc(rec, (int32_t)T("seld"), key_data);
    if (!err)
        err = tb_AECoerceDesc(rec, (int32_t)T("obj "), spec);
    free_desc(rec);
    tb_DisposePtr(v);
    dispose_inputs(dispose, container, key_data);
    return err;
}

int16_t tb_CreateOffsetDescriptor(int32_t offset, gaddr desc)
{
    gaddr v = tb_NewPtr(4, false, true);
    wr32(v, (uint32_t)offset);
    int16_t err = tb_AECreateDesc((int32_t)T("long"), v, 4, desc);
    tb_DisposePtr(v);
    return err;
}

int16_t tb_CreateCompDescriptor(int32_t op, gaddr operand1, gaddr operand2, int8_t dispose, gaddr desc)
{
    int16_t err = make_record(T("cmpd"), T("relo"), T("enum"), (uint32_t)op, T("obj1"), operand1, T("obj2"),
                              operand2, desc);
    dispose_inputs(dispose, operand1, operand2);
    return err;
}

int16_t tb_CreateLogicalDescriptor(gaddr terms, int32_t op, int8_t dispose, gaddr desc)
{
    int16_t err = make_record(T("logi"), T("logc"), T("enum"), (uint32_t)op, T("term"), terms, 0, 0, desc);
    dispose_inputs(dispose, terms, 0);
    return err;
}

int16_t tb_CreateRangeDescriptor(gaddr start, gaddr stop, int8_t dispose, gaddr desc)
{
    gaddr rec = new_desc();
    int16_t err = tb_AECreateList(0, 0, 1, rec);
    if (!err)
        err = tb_AEPutParamDesc(rec, (int32_t)T("star"), start);
    if (!err)
        err = tb_AEPutParamDesc(rec, (int32_t)T("stop"), stop);
    if (!err)
        err = tb_AECoerceDesc(rec, (int32_t)T("rang"), desc);
    free_desc(rec);
    dispose_inputs(dispose, start, stop);
    return err;
}

void osl_reset(void) { naccessors[0] = naccessors[1] = 0; }
