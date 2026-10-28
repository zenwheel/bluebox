/*
 * AppleScript, our own runtime (M14): the loader for compiled scripts and
 * the bytecode interpreter that runs them.
 *
 * Compiled scripts ('scpt' 128 in applets, "FasdUAS 1.101.10" from
 * AppleScript 1.1) are AppleScript's FasLoad serialization: a header, then
 * objects (an index byte, a reference number, a size) that refer to each
 * other through a reference table; the root is a script object whose
 * property vector holds the properties' values and the handlers, each with
 * its literals and its bytecode for a stack machine.  The format and the
 * instruction set follow n0kovo's applescript-decompiler (MIT, built on
 * Jinmo's applescript-disassembler), read as a reference; the code here is
 * our own, and what the decompiler only guessed (how loops, try blocks and
 * commands keep the stack) was worked out from the corpus's applets.
 *
 * Nothing a script does reaches the host: its commands are the scripting
 * additions in as_osax.c and Apple Events to other Blue Box runs.
 */
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "as_internal.h"
#include "tb_internal.h"

#define T(s) fourcc(s)

/* ---------------------------------------------------------------------- */
/* Values and the collector */

static as_obj *heap;
static uint64_t heap_count, allocated_since;
static as_obj *s_undef, *s_null, *s_missing, *s_true, *s_false;

as_obj *as_new(as_kind k)
{
    as_obj *o = calloc(1, sizeof *o);
    o->kind = (uint8_t)k;
    o->gc_next = heap;
    heap = o;
    heap_count++;
    allocated_since++;
    return o;
}

static as_obj *singleton(as_obj **slot, as_kind k, int32_t i)
{
    if (!*slot) {
        *slot = as_new(k);
        (*slot)->i = i;
    }
    return *slot;
}

as_obj *as_undef(void) { return singleton(&s_undef, V_UNDEF, 0); }
as_obj *as_null(void) { return singleton(&s_null, V_NULL, 0); }
as_obj *as_missing(void) { return singleton(&s_missing, V_MISSING, 0); }
as_obj *as_bool(bool b) { return b ? singleton(&s_true, V_BOOL, 1) : singleton(&s_false, V_BOOL, 0); }

as_obj *as_int(int32_t v)
{
    as_obj *o = as_new(V_INT);
    o->i = v;
    return o;
}

as_obj *as_real(double v)
{
    as_obj *o = as_new(V_REAL);
    o->r = v;
    return o;
}

/* AppleScript's integers are 30 bits wide; past that, arithmetic gives reals. */
#define AS_INT_MAX 536870911

as_obj *as_number(double v)
{
    if (v == floor(v) && fabs(v) <= AS_INT_MAX)
        return as_int((int32_t)v);
    return as_real(v);
}

static void set_bytes(as_obj *o, const void *s, size_t n)
{
    o->bytes = malloc(n + 1);
    if (n)
        memcpy(o->bytes, s, n);
    o->bytes[n] = 0;
    o->n = (uint32_t)n;
    o->cap = (uint32_t)n + 1;
}

as_obj *as_text(const void *s, size_t n)
{
    as_obj *o = as_new(V_TEXT);
    set_bytes(o, s, n);
    return o;
}

as_obj *as_cstr(const char *s) { return as_text(s, strlen(s)); }

as_obj *as_ident(const char *s)
{
    as_obj *o = as_new(V_IDENT);
    set_bytes(o, s, strlen(s));
    return o;
}

as_obj *as_const(uint64_t code)
{
    as_obj *o = as_new(V_CONST);
    o->code = code;
    return o;
}

as_obj *as_desc(uint32_t type, const void *data, size_t n)
{
    as_obj *o = as_new(V_DESC);
    o->code = type;
    set_bytes(o, data, n);
    return o;
}

static void reserve(as_obj *l, uint32_t need, bool keys)
{
    if (need <= l->cap)
        return;
    uint32_t cap = l->cap ? l->cap : 4;
    while (cap < need)
        cap *= 2;
    l->items = realloc(l->items, cap * sizeof *l->items);
    if (keys)
        l->keys = realloc(l->keys, cap * sizeof *l->keys);
    l->cap = cap;
}

as_obj *as_list(uint32_t n)
{
    as_obj *o = as_new(V_LIST);
    reserve(o, n, false);
    return o;
}

as_obj *as_record(void) { return as_new(V_RECORD); }

void as_list_push(as_obj *l, as_obj *v)
{
    reserve(l, l->n + 1, false);
    l->items[l->n++] = v;
}

void as_list_insert(as_obj *l, uint32_t at, as_obj *v)
{
    if (at > l->n)
        at = l->n;
    reserve(l, l->n + 1, false);
    memmove(l->items + at + 1, l->items + at, (l->n - at) * sizeof *l->items);
    l->items[at] = v;
    l->n++;
}

void as_text_append(as_obj *t, const void *s, size_t n)
{
    if (t->n + n + 1 > t->cap) {
        uint32_t cap = t->cap ? t->cap : 16;
        while (cap < t->n + n + 1)
            cap *= 2;
        t->bytes = realloc(t->bytes, cap);
        t->cap = cap;
    }
    memcpy(t->bytes + t->n, s, n);
    t->n += (uint32_t)n;
    t->bytes[t->n] = 0;
}

static bool ident_eq(const as_obj *a, const as_obj *b)
{
    if (a->n != b->n)
        return false;
    for (uint32_t i = 0; i < a->n; i++)
        if (tolower(a->bytes[i]) != tolower(b->bytes[i]))
            return false;
    return true;
}

static bool key_eq(const as_obj *a, const as_obj *b)
{
    if (a->kind != b->kind)
        return false;
    if (a->kind == V_CONST)
        return a->code == b->code;
    if (a->kind == V_IDENT || a->kind == V_TEXT)
        return ident_eq(a, b);
    return a == b;
}

static int record_find(as_obj *r, as_obj *key)
{
    for (uint32_t i = 0; i < r->n; i++)
        if (key_eq(r->keys[i], key))
            return (int)i;
    return -1;
}

as_obj *as_record_get(as_obj *r, as_obj *key)
{
    int i = record_find(r, key);
    return i < 0 ? NULL : r->items[i];
}

as_obj *as_record_get_code(as_obj *r, uint32_t code)
{
    for (uint32_t i = 0; i < r->n; i++)
        if (r->keys[i]->kind == V_CONST && r->keys[i]->code == code)
            return r->items[i];
    return NULL;
}

void as_record_set(as_obj *r, as_obj *key, as_obj *v)
{
    int i = record_find(r, key);
    if (i >= 0) {
        r->items[i] = v;
        return;
    }
    reserve(r, r->n + 1, true);
    r->keys[r->n] = key;
    r->items[r->n++] = v;
}

/* A copy as `copy` makes: lists and records copied all the way down. */
as_obj *as_clone(as_obj *v)
{
    if (!v)
        return v;
    if (v->kind == V_LIST) {
        as_obj *l = as_list(v->n);
        for (uint32_t i = 0; i < v->n; i++)
            as_list_push(l, as_clone(v->items[i]));
        return l;
    }
    if (v->kind == V_RECORD) {
        as_obj *r = as_record();
        for (uint32_t i = 0; i < v->n; i++)
            as_record_set(r, v->keys[i], as_clone(v->items[i]));
        return r;
    }
    return v;
}

/* ---- Interpreter state (the collector's roots) ---- */

typedef struct {
    as_obj *handler, *script;
    as_obj **locals;
    uint32_t nlocals;
    const uint8_t *code;
    uint32_t len, pc;
    uint32_t sp, blk;   /* the value and block stacks' depths at entry */
    as_obj *result;
    as_obj *lits;
} frame;

enum { BLK_LOOP, BLK_TRY, BLK_TELL, BLK_CONSIDER, BLK_TIMEOUT, BLK_TRANSACTION };

typedef struct {
    int kind;
    uint32_t sp;        /* the value stack's depth at the block's start */
    uint32_t target;    /* a loop's exit, a try's handler */
    int state;          /* a loop: 0 until its repeat instruction has run */
    int var;            /* a loop's variable */
    double cur, end, step;
    as_obj *v;          /* a tell's target, a loop's collection, a consider's attributes */
    bool case_sensitive;
} block;

static as_obj **stack;
static uint32_t sp, stack_cap;
static frame *frames;
static uint32_t nframes, frames_cap;
static block *blocks;
static uint32_t nblocks, blocks_cap;
static as_obj **pinned;
static uint32_t npinned, pinned_cap;
static as_obj *delimiters; /* AppleScript's text item delimiters (a list) */
static uint64_t instructions;

/* The error being raised, and the one a try block caught. */
static bool error_pending, error_abort;
static as_obj *caught_offending, *caught_partial;
as_error_info as_last_error;
bool as_quit_requested;

void as_pin(as_obj *v)
{
    if (!v)
        return;
    if (npinned == pinned_cap) {
        pinned_cap = pinned_cap ? pinned_cap * 2 : 32;
        pinned = realloc(pinned, pinned_cap * sizeof *pinned);
    }
    pinned[npinned++] = v;
}

void as_unpin(as_obj *v)
{
    for (uint32_t i = npinned; i-- > 0;)
        if (pinned[i] == v) {
            pinned[i] = pinned[--npinned];
            return;
        }
}

static void mark(as_obj *o)
{
    while (o && !o->mark) {
        o->mark = 1;
        if (o->items)
            for (uint32_t i = 0; i < o->n; i++)
                mark(o->items[i]);
        if (o->keys)
            for (uint32_t i = 0; i < o->n; i++)
                mark(o->keys[i]);
        mark(o->a);
        mark(o->b);
        mark(o->c);
        o = o->d;
    }
}

static void free_obj(as_obj *o)
{
    free(o->bytes);
    free(o->items);
    free(o->keys);
    free(o);
}

void as_collect(void)
{
    as_obj *roots[] = { s_undef, s_null, s_missing, s_true, s_false, delimiters, caught_offending,
                        caught_partial, as_last_error.offending, as_last_error.partial };
    for (size_t i = 0; i < sizeof roots / sizeof roots[0]; i++)
        mark(roots[i]);
    for (uint32_t i = 0; i < sp; i++)
        mark(stack[i]);
    for (uint32_t i = 0; i < nframes; i++) {
        frame *f = &frames[i];
        mark(f->handler);
        mark(f->script);
        mark(f->result);
        mark(f->lits);
        for (uint32_t k = 0; k < f->nlocals; k++)
            mark(f->locals[k]);
    }
    for (uint32_t i = 0; i < nblocks; i++)
        mark(blocks[i].v);
    for (uint32_t i = 0; i < npinned; i++)
        mark(pinned[i]);
    as_obj **link = &heap;
    while (*link) {
        as_obj *o = *link;
        if (o->mark) {
            o->mark = 0;
            link = &o->gc_next;
        } else {
            *link = o->gc_next;
            free_obj(o);
            heap_count--;
        }
    }
    allocated_since = 0;
}

/* ---------------------------------------------------------------------- */
/* Errors */

static const char *error_text(int32_t n)
{
    switch (n) {
    case -128: return "User canceled.";
    case -600: return "Application isn't running.";
    case -1700: return "Can't make some data into the expected type.";
    case -1708: return "Event wasn't handled.";
    case -1728: return "Can't get the object.";
    case -2700: return "Unknown error.";
    case -2701: return "Can't divide by zero.";
    case -2753: return "The variable is not defined.";
    }
    return "An error has occurred.";
}

as_obj *as_raise(int32_t num, as_obj *offending, const char *fmt, ...)
{
    error_pending = true;
    as_last_error.number = num;
    as_last_error.offending = offending;
    as_last_error.partial = NULL;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(as_last_error.message, sizeof as_last_error.message, fmt, ap);
        va_end(ap);
    } else {
        snprintf(as_last_error.message, sizeof as_last_error.message, "%s", error_text(num));
    }
    return NULL;
}

bool as_error_pending(void) { return error_pending; }

void as_clear_error(void)
{
    error_pending = error_abort = false;
    memset(&as_last_error, 0, sizeof as_last_error);
}

/* ---------------------------------------------------------------------- */
/* Names of codes, for display and messages */

static const struct {
    uint32_t code;
    const char *name;
} code_names[] = {
    { 0x6C6F6E67, "integer" },  { 0x646F7562, "real" },          { 0x6E6D6272, "number" },
    { 0x54455854, "string" },   { 0x63747874, "text" },          { 0x6C697374, "list" },
    { 0x7265636F, "record" },   { 0x626F6F6C, "boolean" },       { 0x74797065, "class" },
    { 0x636F626A, "item" },     { 0x63686120, "character" },     { 0x63776F72, "word" },
    { 0x63706172, "paragraph" }, { 0x636C696E, "line" },         { 0x6369746D, "text item" },
    { 0x6C656E67, "length" },   { 0x70636C73, "class" },         { 0x70636E74, "contents" },
    { 0x706E616D, "name" },     { 0x62686974, "button returned" }, { 0x74747874, "text returned" },
    { 0x67617675, "gave up" },  { 0x7478646C, "text item delimiters" }, { 0x61736372, "AppleScript" },
    { 0x72736C74, "result" },   { 0x616C6973, "alias" },         { 0x66696C65, "file" },
    { 0x6C647420, "date" },     { 0x73637074, "script" },        { 0x68616E64, "handler" },
    { 0x63617070, "application" }, { 0x72657374, "rest" },       { 0x72767365, "reverse" },
    { 0x65727273, "error" },    { 0x6D736E67, "missing value" }, { 0x6E756C6C, "null" },
    { 0x70692020, "pi" },       { 0x72657420, "return" },        { 0x73706163, "space" },
    { 0x74616220, "tab" },      { 0x76657273, "version" },       { 0x74696D65, "time" },
    { 0x776B6479, "weekday" }, { 0x6D6E7468, "month" },        { 0x64617920, "day" },
    { 0x79656172, "year" },
};

const char *as_code_name(uint32_t code)
{
    for (size_t i = 0; i < sizeof code_names / sizeof code_names[0]; i++)
        if (code_names[i].code == code)
            return code_names[i].name;
    return NULL;
}

static void code_chars(uint32_t code, char out[5])
{
    for (int i = 0; i < 4; i++) {
        uint8_t ch = (uint8_t)(code >> (24 - 8 * i));
        out[i] = (char)(ch >= ' ' ? ch : '?');
    }
    out[4] = 0;
}

/* ---------------------------------------------------------------------- */
/* The loader (FasLoad) */

typedef struct {
    const uint8_t *p;
    size_t len, pos;
    bool bad;
    char why[128];
    as_obj **table;
    uint32_t ntable;
    bool reuse;
    int index, ref, size;
    int mismatches, depth;
} fas;

static void fas_fail(fas *f, const char *why)
{
    if (!f->bad)
        snprintf(f->why, sizeof f->why, "%s at offset %zu", why, f->pos);
    f->bad = true;
}

static bool fas_need(fas *f, size_t n)
{
    if (f->bad || f->pos + n > f->len) {
        fas_fail(f, "truncated data");
        return false;
    }
    return true;
}

static uint32_t fas_u8(fas *f)
{
    if (!fas_need(f, 1))
        return 0;
    return f->p[f->pos++];
}

static uint32_t fas_u16(fas *f)
{
    if (!fas_need(f, 2))
        return 0;
    uint32_t v = (uint32_t)f->p[f->pos] << 8 | f->p[f->pos + 1];
    f->pos += 2;
    return v;
}

static int32_t fas_s16(fas *f) { return (int16_t)fas_u16(f); }

static uint32_t fas_u32(fas *f)
{
    uint32_t hi = fas_u16(f);
    return hi << 16 | fas_u16(f);
}

static const uint8_t *fas_bytes(fas *f, size_t n)
{
    if (!fas_need(f, n))
        return NULL;
    const uint8_t *b = f->p + f->pos;
    f->pos += n;
    return b;
}

static void fas_register(fas *f, int ref, as_obj *v)
{
    if (ref < 0)
        return;
    if ((uint32_t)ref >= f->ntable) {
        uint32_t n = (uint32_t)ref + 32;
        f->table = realloc(f->table, n * sizeof *f->table);
        memset(f->table + f->ntable, 0, (n - f->ntable) * sizeof *f->table);
        f->ntable = n;
    }
    f->table[ref] = v;
}

static as_obj *fas_lookup(fas *f, int ref)
{
    return ref >= 0 && (uint32_t)ref < f->ntable ? f->table[ref] : NULL;
}

static void fas_header(fas *f, int *index, int *ref, int *size)
{
    *index = (int)fas_u8(f);
    *ref = fas_s16(f);
    *size = (int)fas_u16(f);
}

static as_obj *fas_body(fas *f, int ref, int index, int size);

static as_obj *fas_load(fas *f, int num)
{
    if (f->bad)
        return as_null();
    if (f->reuse)
        f->reuse = false;
    else
        fas_header(f, &f->index, &f->ref, &f->size);
    if (f->ref != num) {
        /* As AppleScript does: the object is missing; this header is the next one's. */
        f->reuse = true;
        if (++f->mismatches > 200)
            fas_fail(f, "too many reference mismatches");
        return as_null();
    }
    if (++f->depth > 200) {
        fas_fail(f, "objects nested too deeply");
        return as_null();
    }
    int index = f->index;
    as_obj *v = fas_body(f, num, index, f->size);
    f->depth--;
    /* Objects are known by their reference from then on (a string the
     * source's statements and a handler's literals share), except the
     * kinds whose reference AppleScript doesn't use. */
    if (v && num > 0 && index != 2 && index != 7 && index != 10 && index != 11 && !fas_lookup(f, num))
        fas_register(f, num, v);
    return v ? v : as_null();
}

static as_obj *fas_find(fas *f, int num)
{
    if (num == 0)
        return as_null();
    as_obj *v = fas_lookup(f, num);
    return v ? v : fas_load(f, num);
}

/* Resolve n references read first, then loaded in order. */
static as_obj *fas_vector(fas *f, int btype, int n)
{
    int16_t *refs = malloc((size_t)(n ? n : 1) * sizeof *refs);
    for (int i = 0; i < n; i++)
        refs[i] = (int16_t)fas_s16(f);
    as_obj *v = as_new(V_VECTOR);
    v->btype = (int16_t)btype;
    reserve(v, (uint32_t)n, false);
    for (int i = 0; i < n && !f->bad; i++)
        v->items[v->n++] = fas_find(f, refs[i]);
    free(refs);
    return v;
}

/* An application named in a script (tell application "X"): its alias as
 * AppleScript keeps it, after a header of its own. */
static as_obj *fas_application(const uint8_t *d, size_t n)
{
    as_obj *app = as_new(V_APP);
    if (n >= 94) {
        const uint8_t *alias = d + 94;
        size_t alen = n - 94;
        set_bytes(app, alias, alen);
        if (alen >= 130) {
            uint8_t nl = alias[50] > 63 ? 63 : alias[50];
            app->a = as_text(alias + 51, nl);
            app->code = (uint32_t)alias[126] << 24 | (uint32_t)alias[127] << 16 | (uint32_t)alias[128] << 8 | alias[129];
            app->i = (int32_t)((uint32_t)alias[122] << 24 | (uint32_t)alias[123] << 16 | (uint32_t)alias[124] << 8 | alias[125]);
        }
    }
    if (!app->a)
        app->a = as_cstr("");
    return app;
}

static as_obj *fas_body(fas *f, int ref, int index, int size)
{
    switch (index) {
    case 1: /* a symbol */
        if (size) {
            uint64_t hi = fas_u32(f);
            return as_const(hi << 32 | fas_u32(f));
        }
        return as_null();
    case 2: { /* a list, as a chain of pairs */
        as_obj *l = as_list(0);
        if (size != 2)
            return l;
        for (;;) {
            int a = fas_s16(f), b = fas_s16(f);
            as_list_push(l, fas_find(f, a));
            if (f->bad || fas_lookup(f, b))
                break;
            int idx, r, sz;
            fas_header(f, &idx, &r, &sz);
            if (idx != 2)
                fas_body(f, r, idx, sz);
            if (sz != 2) {
                if (sz)
                    fas_fail(f, "bad list");
                break;
            }
        }
        return l;
    }
    case 3: /* a small integer, in the header */
        return as_int((int16_t)size);
    case 4:
    case 14: { /* a value block */
        int btype = (int)fas_u8(f);
        if (size == 0 && btype == 15) {
            as_obj *v = as_new(V_VECTOR);
            v->btype = 15;
            fas_register(f, ref, v);
            return v;
        }
        as_obj *v = fas_vector(f, btype, size);
        fas_register(f, ref, v);
        return v;
    }
    case 6: { /* a record, as a chain of bindings */
        as_obj *r = as_record();
        fas_register(f, ref, r);
        if (size == 1)
            return r;
        if (size != 3) {
            fas_fail(f, "bad record");
            return r;
        }
        for (;;) {
            int a = fas_s16(f), b = fas_s16(f), c = fas_s16(f);
            as_obj *key = fas_find(f, a), *val = fas_find(f, b);
            reserve(r, r->n + 1, true);
            r->keys[r->n] = key;
            r->items[r->n++] = val;
            if (f->bad || fas_lookup(f, c))
                break;
            int idx, rr, sz;
            fas_header(f, &idx, &rr, &sz);
            if (idx != 6) {
                fas_body(f, rr, idx, sz);
                break;
            }
            if (sz != 3)
                break;
        }
        return r;
    }
    case 7: /* a long integer */
        if (size != 4) {
            fas_fail(f, "bad long integer");
            return NULL;
        }
        return as_number((int32_t)fas_u32(f));
    case 8: { /* a real */
        if (size != 8) {
            fas_fail(f, "bad real");
            return NULL;
        }
        uint64_t bits = (uint64_t)fas_u32(f) << 32;
        bits |= fas_u32(f);
        double d;
        memcpy(&d, &bits, sizeof d);
        return as_real(d);
    }
    case 9:
        return as_bool(size != 0);
    case 10: { /* a class, property, constant or event identifier */
        int kind = (int)fas_u8(f);
        if (kind == 11 && size == 8) {
            uint64_t hi = fas_u32(f);
            return as_const(hi << 32 | fas_u32(f));
        }
        if ((kind == 10 || kind == 47) && size == 4)
            return as_const(fas_u32(f));
        if (kind == 46 && size == 24) {
            as_obj *e = as_new(V_EVENT);
            for (int i = 0; i < 6; i++)
                e->ev[i] = fas_u32(f);
            return e;
        }
        fas_fail(f, "bad identifier");
        return NULL;
    }
    case 11: { /* a user identifier: the name folded, then as written */
        fas_u8(f);
        uint32_t al = fas_u16(f);
        const uint8_t *a = fas_bytes(f, al);
        uint32_t bl = fas_u16(f);
        const uint8_t *b = fas_bytes(f, bl);
        as_obj *id = as_new(V_IDENT);
        if (bl && b)
            set_bytes(id, b, bl);
        else if (a)
            set_bytes(id, a, al);
        else
            set_bytes(id, "", 0);
        return id;
    }
    case 12: { /* a string, with its style */
        uint32_t n = fas_u16(f);
        const uint8_t *s = fas_bytes(f, n);
        uint32_t sn = fas_u16(f);
        fas_bytes(f, sn);
        return s ? as_text(s, n) : NULL;
    }
    case 13: { /* a statement (the source's structure): read and dropped */
        fas_u8(f);
        fas_u16(f);
        fas_u16(f);
        fas_u16(f);
        fas_register(f, ref, as_null());
        fas_vector(f, 0, size);
        return as_null();
    }
    case 15: { /* a typed data block */
        int type = (int)fas_u8(f);
        const uint8_t *d = fas_bytes(f, (size_t)size);
        if (!d)
            return NULL;
        as_obj *v;
        if (type == 8)
            v = fas_application(d, (size_t)size);
        else if (type == 13 && size >= 8)
            v = as_desc((uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3], d + 8,
                        (size_t)size - 8);
        else {
            v = as_new(V_BYTES);
            set_bytes(v, d, (size_t)size);
        }
        fas_register(f, ref, v);
        return v;
    }
    case 16: { /* a pointer block */
        as_obj *v = fas_vector(f, -1, size);
        fas_register(f, ref, v);
        return v;
    }
    case 17: { /* untyped data (bytecode) */
        const uint8_t *d = fas_bytes(f, (size_t)size);
        as_obj *v = as_new(V_BYTES);
        set_bytes(v, d ? d : (const uint8_t *)"", d ? (size_t)size : 0);
        return v;
    }
    case 18:
    case 19: { /* long data blocks */
        if (index == 18)
            fas_u8(f);
        uint32_t n = fas_u32(f);
        const uint8_t *d = fas_bytes(f, n);
        as_obj *v = as_new(V_BYTES);
        set_bytes(v, d ? d : (const uint8_t *)"", d ? n : 0);
        return v;
    }
    }
    fas_fail(f, "unknown object type");
    return NULL;
}

static as_obj *item_at(as_obj *v, uint32_t i)
{
    return v && (v->kind == V_VECTOR || v->kind == V_LIST) && i < v->n ? v->items[i] : NULL;
}

/* A value as a script keeps it: lists (a property's {}) are value blocks
 * of type 4 holding the count and a vector of the items. */
static as_obj *script_value(as_obj *v)
{
    if (v && v->kind == V_VECTOR && v->btype == 4 && v->n == 2 && v->items[0]->kind == V_INT) {
        as_obj *items = v->items[1];
        int32_t n = v->items[0]->i;
        as_obj *l = as_list(0);
        for (int32_t i = 0; items && (items->kind == V_VECTOR || items->kind == V_LIST) && i < n && (uint32_t)i < items->n; i++)
            as_list_push(l, script_value(items->items[i]));
        return l;
    }
    if (v && v->kind == V_LIST)
        for (uint32_t i = 0; i < v->n; i++)
            v->items[i] = script_value(v->items[i]);
    return v;
}

/* A handler's value block: name, statement, parameters, labelled
 * parameters, local names, literals, bytecode. */
static as_obj *make_handler(as_obj *blk)
{
    as_obj *name = item_at(blk, 0), *args = item_at(blk, 2), *locals = item_at(blk, 4);
    as_obj *lits = item_at(blk, 5), *code = item_at(blk, 6);
    if (!name || !lits || !code || code->kind != V_BYTES || (lits->kind != V_VECTOR && lits->kind != V_LIST))
        return NULL;
    for (uint32_t k = 0; k < lits->n; k++)
        lits->items[k] = script_value(lits->items[k]);
    as_obj *h = as_new(V_HANDLER);
    h->a = name;
    h->c = lits;
    h->d = code;
    if (args && args->kind == V_IDENT) {
        h->i = 2; /* an event handler's direct parameter */
        h->b = as_list(1);
        as_list_push(h->b, args);
    } else if (args && args->kind == V_VECTOR && args->n >= 2) {
        h->i = 1;
        as_obj *names = args->items[1];
        h->b = as_list(0);
        for (uint32_t k = 0; names && k < names->n; k++)
            as_list_push(h->b, names->items[k]);
    } else if (args && args->kind == V_LIST) {
        h->i = 1;
        h->b = as_list(0);
    }
    uint32_t nl = locals && (locals->kind == V_VECTOR || locals->kind == V_LIST) ? locals->n : 0;
    h->items = calloc(nl ? nl : 1, sizeof *h->items);
    for (uint32_t k = 0; k < nl; k++)
        h->items[k] = locals->items[k];
    h->n = nl;
    return h;
}

/* The root is a script object (block type 15) whose last slot is its
 * property vector: something, the names, then each one's value. */
static as_obj *make_script(as_obj *root)
{
    if (!root || root->kind != V_VECTOR)
        return NULL;
    as_obj *props = NULL;
    for (uint32_t i = root->n; i-- > 0 && !props;) {
        as_obj *p = root->items[i];
        if (p && p->kind == V_VECTOR && p->n >= 2 && p->items[1] && p->items[1]->kind == V_VECTOR)
            props = p;
    }
    if (!props)
        return NULL;
    as_obj *s = as_new(V_SCRIPT);
    as_obj *names = props->items[1];
    s->a = names;
    s->b = as_list(names->n);
    for (uint32_t i = 0; i < names->n; i++) {
        as_obj *v = i + 2 < props->n ? props->items[i + 2] : as_undef();
        if (v && v->kind == V_VECTOR && v->btype == 16) {
            as_obj *h = make_handler(v);
            if (h)
                v = h;
        } else {
            v = script_value(v);
        }
        as_list_push(s->b, v ? v : as_undef());
    }
    s->c = as_record();
    return s;
}

as_obj *as_fasload(const uint8_t *data, size_t len, char *err, size_t errlen)
{
    fas f = { .p = data, .len = len };
    if (len >= 2 && data[0] == '#' && data[1] == '!')
        while (f.pos < len && data[f.pos++] != '\n')
            ;
    const uint8_t *magic = fas_bytes(&f, 8);
    if (!magic || memcmp(magic, "FasdUAS ", 8) != 0) {
        snprintf(err, errlen, "not a compiled script");
        return NULL;
    }
    const uint8_t *version = fas_bytes(&f, 4);
    if (version && memcmp(version, "1.10", 4) >= 0)
        version = fas_bytes(&f, 4);
    if (!version || memcmp(version, "0.97", 4) <= 0 || memcmp(version, "1.11", 4) >= 0) {
        snprintf(err, errlen, "a compiled script of a version we don't read");
        return NULL;
    }
    as_obj *root = fas_load(&f, 0);
    free(f.table);
    if (f.bad) {
        snprintf(err, errlen, "%s", f.why);
        return NULL;
    }
    as_obj *s = make_script(root);
    if (!s)
        snprintf(err, errlen, "no script object");
    return s;
}

/* ---------------------------------------------------------------------- */
/* Text and numbers */

static bool is_number(const as_obj *v) { return v->kind == V_INT || v->kind == V_REAL; }
static double num_of(const as_obj *v) { return v->kind == V_INT ? v->i : v->r; }

/* Reals as AppleScript writes them: "2.5", and E notation from 10000 up
 * and below 0.0001 ("1.0E+4"). */
static void format_real(double r, char *out, size_t cap)
{
    double a = fabs(r);
    if (r == 0) {
        snprintf(out, cap, "0.0");
        return;
    }
    if (a >= 1e-4 && a < 1e4) {
        snprintf(out, cap, "%.12g", r);
        if (!strpbrk(out, ".en"))
            strncat(out, ".0", cap - strlen(out) - 1);
        return;
    }
    int e = (int)floor(log10(a));
    double m = r / pow(10, e);
    if (fabs(m) >= 10 - 5e-12) {
        m /= 10;
        e++;
    }
    char ms[32];
    snprintf(ms, sizeof ms, "%.12g", m);
    if (!strchr(ms, '.'))
        strcat(ms, ".0");
    snprintf(out, cap, "%sE%s%d", ms, e >= 0 ? "+" : "-", abs(e));
}

static bool parse_number(const uint8_t *s, uint32_t n, double *out, bool *is_int)
{
    char buf[64];
    uint32_t a = 0, b = n;
    while (a < b && isspace(s[a]))
        a++;
    while (b > a && isspace(s[b - 1]))
        b--;
    if (a == b || b - a >= sizeof buf)
        return false;
    memcpy(buf, s + a, b - a);
    buf[b - a] = 0;
    char *end;
    double d = strtod(buf, &end);
    if (*end || isnan(d) || isinf(d) || !strpbrk(buf, "0123456789"))
        return false;
    *out = d;
    *is_int = !strpbrk(buf, ".eE") && d == floor(d) && fabs(d) <= AS_INT_MAX;
    return true;
}

bool as_to_number(as_obj *v, double *out, bool *is_int)
{
    if (!v)
        return false;
    switch (v->kind) {
    case V_INT:
        *out = v->i;
        *is_int = true;
        return true;
    case V_REAL:
        *out = v->r;
        *is_int = false;
        return true;
    case V_TEXT:
        return parse_number(v->bytes, v->n, out, is_int);
    case V_LIST:
        return v->n == 1 && as_to_number(v->items[0], out, is_int);
    default:
        return false;
    }
}

bool as_truth(as_obj *v, bool *out)
{
    if (!v)
        return false;
    if (v->kind == V_BOOL) {
        *out = v->i != 0;
        return true;
    }
    if (v->kind == V_TEXT) {
        if (v->n == 4 && !strncasecmp((const char *)v->bytes, "true", 4)) {
            *out = true;
            return true;
        }
        if (v->n == 5 && !strncasecmp((const char *)v->bytes, "false", 5)) {
            *out = false;
            return true;
        }
    }
    if (v->kind == V_LIST && v->n == 1)
        return as_truth(v->items[0], out);
    return false;
}

as_obj *as_delimiters(void)
{
    if (!delimiters) {
        delimiters = as_list(1);
        as_list_push(delimiters, as_cstr(""));
    }
    return delimiters;
}

static bool text_into(as_obj *t, as_obj *v);

static bool text_into(as_obj *t, as_obj *v)
{
    char buf[64];
    switch (v->kind) {
    case V_TEXT:
        as_text_append(t, v->bytes, v->n);
        return true;
    case V_INT:
        snprintf(buf, sizeof buf, "%d", v->i);
        as_text_append(t, buf, strlen(buf));
        return true;
    case V_REAL:
        format_real(v->r, buf, sizeof buf);
        as_text_append(t, buf, strlen(buf));
        return true;
    case V_BOOL:
        as_text_append(t, v->i ? "true" : "false", v->i ? 4 : 5);
        return true;
    case V_LIST: {
        as_obj *d = as_delimiters();
        as_obj *delim = d->n && d->items[0]->kind == V_TEXT ? d->items[0] : NULL;
        for (uint32_t i = 0; i < v->n; i++) {
            if (i && delim)
                as_text_append(t, delim->bytes, delim->n);
            if (!text_into(t, v->items[i]))
                return false;
        }
        return true;
    }
    case V_CONST: {
        const char *n = v->code >> 32 ? NULL : as_code_name((uint32_t)v->code);
        if (!n)
            return false;
        as_text_append(t, n, strlen(n));
        return true;
    }
    case V_DATE: {
        char s[128];
        as_show(v, s, sizeof s);
        /* without the word "date" and its quotes */
        size_t l = strlen(s);
        if (l > 7)
            as_text_append(t, s + 6, l - 7);
        return true;
    }
    case V_DESC:
        if (v->code == T("TEXT") || v->code == T("ctxt") || v->code == T("itxt")) {
            as_text_append(t, v->bytes, v->n);
            return true;
        }
        if (v->code == T("alis") || v->code == T("fss ")) {
            as_obj *p = as_file_path(v);
            if (!p)
                return false;
            as_text_append(t, p->bytes, p->n);
            return true;
        }
        return false;
    default:
        return false;
    }
}

static as_obj *cant_make(as_obj *v, uint32_t type)
{
    char shown[256], tn[8];
    as_show(v, shown, sizeof shown);
    const char *name = as_code_name(type);
    if (!name) {
        code_chars(type, tn);
        name = tn;
    }
    return as_raise(asErrCantCoerce, v, "Can't make %s into a %s.", shown, name);
}

as_obj *as_to_text(as_obj *v)
{
    if (v && v->kind == V_TEXT)
        return v;
    as_obj *t = as_text("", 0);
    if (!v || !text_into(t, v))
        return cant_make(v ? v : as_undef(), T("TEXT"));
    return t;
}

/* ---------------------------------------------------------------------- */
/* Display (OSADisplay and messages) */

typedef struct {
    char *p;
    size_t n, cap;
} sbuf;

static void sb_put(sbuf *b, const char *s, size_t n)
{
    for (size_t i = 0; i < n && b->n + 1 < b->cap; i++)
        b->p[b->n++] = s[i];
    b->p[b->n] = 0;
}

static void sb_str(sbuf *b, const char *s) { sb_put(b, s, strlen(s)); }

static void show_code(sbuf *b, uint64_t code, bool constant)
{
    const char *n = code >> 32 ? NULL : as_code_name((uint32_t)code);
    if (n && !constant) {
        sb_str(b, n);
        return;
    }
    char c[16];
    if (code >> 32) {
        code_chars((uint32_t)(code >> 32), c);
        code_chars((uint32_t)code, c + 4);
        sb_str(b, "\xC7" "constant ");
    } else {
        code_chars((uint32_t)code, c);
        sb_str(b, "\xC7" "class ");
    }
    sb_str(b, c);
    sb_str(b, "\xC8");
}

static void show_into(sbuf *b, as_obj *v, int depth)
{
    char buf[64];
    if (!v) {
        sb_str(b, "missing value");
        return;
    }
    if (depth > 8) {
        sb_str(b, "...");
        return;
    }
    switch (v->kind) {
    case V_UNDEF:
    case V_NULL:
        sb_str(b, v->kind == V_NULL ? "null" : "");
        return;
    case V_MISSING:
        sb_str(b, "missing value");
        return;
    case V_BOOL:
        sb_str(b, v->i ? "true" : "false");
        return;
    case V_INT:
        snprintf(buf, sizeof buf, "%d", v->i);
        sb_str(b, buf);
        return;
    case V_REAL:
        format_real(v->r, buf, sizeof buf);
        sb_str(b, buf);
        return;
    case V_TEXT:
        sb_str(b, "\"");
        for (uint32_t i = 0; i < v->n; i++) {
            if (v->bytes[i] == '"' || v->bytes[i] == '\\')
                sb_str(b, "\\");
            sb_put(b, (const char *)v->bytes + i, 1);
        }
        sb_str(b, "\"");
        return;
    case V_IDENT:
        sb_put(b, (const char *)v->bytes, v->n);
        return;
    case V_LIST:
        sb_str(b, "{");
        for (uint32_t i = 0; i < v->n; i++) {
            if (i)
                sb_str(b, ", ");
            show_into(b, v->items[i], depth + 1);
        }
        sb_str(b, "}");
        return;
    case V_RECORD:
        sb_str(b, "{");
        for (uint32_t i = 0; i < v->n; i++) {
            if (i)
                sb_str(b, ", ");
            if (v->keys[i]->kind == V_CONST)
                show_code(b, v->keys[i]->code, false);
            else
                show_into(b, v->keys[i], depth + 1);
            sb_str(b, ":");
            show_into(b, v->items[i], depth + 1);
        }
        sb_str(b, "}");
        return;
    case V_CONST:
        show_code(b, v->code, (v->code >> 32) != 0);
        return;
    case V_APP:
        sb_str(b, "application \"");
        if (v->a)
            sb_put(b, (const char *)v->a->bytes, v->a->n);
        sb_str(b, "\"");
        return;
    case V_SCRIPT:
        sb_str(b, "\xC7script\xC8");
        return;
    case V_HANDLER:
        sb_str(b, "\xC7handler ");
        if (v->a && v->a->kind == V_IDENT)
            sb_put(b, (const char *)v->a->bytes, v->a->n);
        sb_str(b, "\xC8");
        return;
    case V_DATE: {
        time_t t = (time_t)(v->r - 2082844800.0);
        struct tm tm;
        gmtime_r(&t, &tm);
        static const char *days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
        static const char *months[] = { "January", "February", "March", "April", "May", "June", "July",
                                        "August", "September", "October", "November", "December" };
        char s[96];
        snprintf(s, sizeof s, "date \"%s, %s %d, %d %d:%02d:%02d %s\"", days[tm.tm_wday], months[tm.tm_mon], tm.tm_mday,
                 tm.tm_year + 1900, tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, tm.tm_min, tm.tm_sec,
                 tm.tm_hour < 12 ? "AM" : "PM");
        sb_str(b, s);
        return;
    }
    case V_REF: {
        static const char *forms[] = { "", "every ", "some ", "", "", "", "", "", "", "beginning", "end", "middle " };
        if (v->btype == REF_PROPERTY) {
            if (v->b && v->b->kind == V_CONST)
                show_code(b, v->b->code, false);
            else
                show_into(b, v->b, depth + 1);
        } else if (v->btype == REF_BEGINNING || v->btype == REF_END) {
            sb_str(b, forms[v->btype]);
        } else {
            if (v->btype < (int)(sizeof forms / sizeof forms[0]))
                sb_str(b, forms[v->btype]);
            if (v->b && v->b->kind == V_CONST)
                show_code(b, v->b->code, false);
            if (v->btype == REF_INDEX || v->btype == REF_NAME || v->btype == REF_KEY) {
                sb_str(b, " ");
                show_into(b, v->c, depth + 1);
            } else if (v->btype == REF_RANGE) {
                sb_str(b, " ");
                show_into(b, v->c, depth + 1);
                sb_str(b, " thru ");
                show_into(b, v->d, depth + 1);
            }
        }
        if (v->a && v->a->kind != V_SCRIPT) {
            sb_str(b, " of ");
            show_into(b, v->a, depth + 1);
        }
        return;
    }
    case V_DESC: {
        char c[8];
        if (v->code == T("alis")) {
            as_obj *p = as_file_path(v);
            if (p) {
                sb_str(b, "alias \"");
                sb_put(b, (const char *)p->bytes, p->n);
                sb_str(b, "\"");
                return;
            }
        }
        code_chars((uint32_t)v->code, c);
        sb_str(b, "\xC7" "data ");
        sb_str(b, c);
        sb_str(b, "\xC8");
        return;
    }
    case V_EVENT: {
        char c[16];
        code_chars(v->ev[0], c);
        code_chars(v->ev[1], c + 4);
        sb_str(b, "\xC7" "event ");
        sb_str(b, c);
        sb_str(b, "\xC8");
        return;
    }
    default:
        sb_str(b, "\xC7" "data\xC8");
        return;
    }
}

void as_show(as_obj *v, char *out, size_t cap)
{
    sbuf b = { out, 0, cap };
    if (cap)
        out[0] = 0;
    show_into(&b, v, 0);
    /* Long values are cut short in messages, as AppleScript's are. */
    if (b.n + 1 >= cap && cap > 4)
        memcpy(out + cap - 4, "...", 4);
}

/* ---------------------------------------------------------------------- */
/* Coercions */

static uint32_t class_of(as_obj *v)
{
    switch (v->kind) {
    case V_INT: return T("long");
    case V_REAL: return T("doub");
    case V_TEXT: return T("TEXT");
    case V_LIST: return T("list");
    case V_RECORD: return T("reco");
    case V_BOOL: return T("bool");
    case V_CONST: return T("type");
    case V_SCRIPT: return T("scpt");
    case V_HANDLER: return T("hand");
    case V_APP: return T("capp");
    case V_DATE: return T("ldt ");
    case V_DESC: return (uint32_t)v->code;
    case V_MISSING: return T("msng");
    default: return T("null");
    }
}

as_obj *as_coerce(as_obj *v, uint32_t type)
{
    v = as_deref(v);
    if (!v)
        return NULL;
    double d;
    bool is_int, b;
    switch (type) {
    case 0x54455854: /* 'TEXT' */
    case 0x63747874: /* 'ctxt' */
    case 0x69747874: /* 'itxt' */
    case 0x75747874: /* 'utxt' */
        return as_to_text(v);
    case 0x6C6F6E67: /* 'long' */
    case 0x73686F72: /* 'shor' */
        if (!as_to_number(v, &d, &is_int))
            return cant_make(v, T("long"));
        /* Reals round to the nearest integer, halves away from zero. */
        d = d < 0 ? -floor(-d + 0.5) : floor(d + 0.5);
        if (fabs(d) > 2147483647.0)
            return cant_make(v, T("long"));
        return as_int((int32_t)d);
    case 0x6E6D6272: /* 'nmbr' */
        if (!as_to_number(v, &d, &is_int))
            return cant_make(v, type);
        return is_int ? as_int((int32_t)d) : as_real(d);
    case 0x646F7562: /* 'doub' */
    case 0x73696E67: /* 'sing' */
    case 0x6C64626C: /* 'ldbl' */
        if (!as_to_number(v, &d, &is_int))
            return cant_make(v, T("doub"));
        return as_real(d);
    case 0x626F6F6C: /* 'bool' */
        if (!as_truth(v, &b))
            return cant_make(v, type);
        return as_bool(b);
    case 0x6C697374: /* 'list' */
        if (v->kind == V_LIST)
            return v;
        if (v->kind == V_RECORD) {
            as_obj *l = as_list(v->n);
            for (uint32_t i = 0; i < v->n; i++)
                as_list_push(l, v->items[i]);
            return l;
        }
        {
            as_obj *l = as_list(1);
            as_list_push(l, v);
            return l;
        }
    case 0x7265636F: /* 'reco' */
        if (v->kind == V_RECORD)
            return v;
        return cant_make(v, type);
    case 0x2A2A2A2A: /* '****' */
    case 0x636F626A: /* 'cobj' */
        return v;
    case 0x74797065: /* 'type' */
        if (v->kind == V_CONST)
            return v;
        return cant_make(v, type);
    }
    if (v->kind == V_LIST && v->n == 1)
        return as_coerce(v->items[0], type);
    if (class_of(v) == type)
        return v;
    return cant_make(v, type);
}

/* ---------------------------------------------------------------------- */
/* Comparison and arithmetic */

static bool considering_case(void);

static int text_cmp(const as_obj *a, const as_obj *b, bool cs)
{
    uint32_t n = a->n < b->n ? a->n : b->n;
    for (uint32_t i = 0; i < n; i++) {
        int x = a->bytes[i], y = b->bytes[i];
        if (!cs && x < 128 && y < 128) {
            x = tolower(x);
            y = tolower(y);
        }
        if (x != y)
            return x - y;
    }
    return (int)a->n - (int)b->n;
}

static bool equal_cs(as_obj *a, as_obj *b, bool cs)
{
    if (a == b)
        return true;
    if (is_number(a) && is_number(b))
        return num_of(a) == num_of(b);
    if (a->kind != b->kind) {
        /* A text and a one-item list of it, as AppleScript compares them. */
        if (a->kind == V_LIST && a->n == 1)
            return equal_cs(a->items[0], b, cs);
        if (b->kind == V_LIST && b->n == 1)
            return equal_cs(a, b->items[0], cs);
        return false;
    }
    switch (a->kind) {
    case V_TEXT:
        return text_cmp(a, b, cs) == 0;
    case V_IDENT:
        return ident_eq(a, b);
    case V_BOOL:
        return a->i == b->i;
    case V_CONST:
        return a->code == b->code;
    case V_DATE:
        return a->r == b->r;
    case V_LIST:
        if (a->n != b->n)
            return false;
        for (uint32_t i = 0; i < a->n; i++)
            if (!equal_cs(a->items[i], b->items[i], cs))
                return false;
        return true;
    case V_RECORD:
        if (a->n != b->n)
            return false;
        for (uint32_t i = 0; i < a->n; i++) {
            as_obj *o = as_record_get(b, a->keys[i]);
            if (!o || !equal_cs(a->items[i], o, cs))
                return false;
        }
        return true;
    case V_APP:
        return a->code == b->code;
    case V_UNDEF:
    case V_NULL:
    case V_MISSING:
        return true;
    case V_DESC:
        return a->code == b->code && a->n == b->n && !memcmp(a->bytes, b->bytes, a->n);
    default:
        return false;
    }
}

bool as_equal(as_obj *a, as_obj *b) { return equal_cs(a, b, considering_case()); }

/* a < b and the like: the right operand made into the left's class. */
static bool compare(as_obj *a, as_obj *b, int *out)
{
    double x, y;
    bool ix;
    if (is_number(a)) {
        if (!as_to_number(b, &y, &ix)) {
            cant_make(b, T("nmbr"));
            return false;
        }
        x = num_of(a);
        *out = x < y ? -1 : x > y;
        return true;
    }
    if (a->kind == V_TEXT) {
        as_obj *t = as_to_text(b);
        if (!t)
            return false;
        int c = text_cmp(a, t, considering_case());
        *out = c < 0 ? -1 : c > 0;
        return true;
    }
    if (a->kind == V_DATE && b->kind == V_DATE) {
        *out = a->r < b->r ? -1 : a->r > b->r;
        return true;
    }
    if (a->kind == V_LIST && a->n == 1)
        return compare(a->items[0], b, out);
    char sa[128], sb[128];
    as_show(a, sa, sizeof sa);
    as_show(b, sb, sizeof sb);
    as_raise(asErrCantCoerce, a, "Can't compare %s to %s.", sa, sb);
    return false;
}

static bool text_find(const as_obj *hay, const as_obj *needle, bool cs, uint32_t *at)
{
    if (needle->n == 0) {
        *at = 0;
        return true;
    }
    for (uint32_t i = 0; i + needle->n <= hay->n; i++) {
        uint32_t k = 0;
        for (; k < needle->n; k++) {
            int x = hay->bytes[i + k], y = needle->bytes[k];
            if (!cs && x < 128 && y < 128) {
                x = tolower(x);
                y = tolower(y);
            }
            if (x != y)
                break;
        }
        if (k == needle->n) {
            *at = i;
            return true;
        }
    }
    return false;
}

/* contains, starts with, ends with (op 0, 1, 2) */
static as_obj *containment(as_obj *a, as_obj *b, int op)
{
    bool cs = considering_case();
    if (a->kind == V_TEXT) {
        as_obj *t = as_to_text(b);
        if (!t)
            return NULL;
        if (t->n > a->n)
            return as_bool(false);
        if (op == 1)
            return as_bool(text_cmp(&(as_obj){ .bytes = a->bytes, .n = t->n }, t, cs) == 0);
        if (op == 2)
            return as_bool(text_cmp(&(as_obj){ .bytes = a->bytes + a->n - t->n, .n = t->n }, t, cs) == 0);
        uint32_t at;
        return as_bool(text_find(a, t, cs, &at));
    }
    if (a->kind == V_LIST) {
        as_obj *l = b->kind == V_LIST ? b : NULL;
        uint32_t m = l ? l->n : 1;
        if (m > a->n)
            return as_bool(false);
        if (m == 0)
            return as_bool(true);
        uint32_t from = op == 2 ? a->n - m : 0, to = op == 0 ? a->n - m : from;
        for (uint32_t i = from; i <= to; i++) {
            uint32_t k = 0;
            for (; k < m; k++)
                if (!equal_cs(a->items[i + k], l ? l->items[k] : b, cs))
                    break;
            if (k == m)
                return as_bool(true);
        }
        return as_bool(false);
    }
    if (a->kind == V_RECORD && b->kind == V_RECORD && op == 0) {
        for (uint32_t i = 0; i < b->n; i++) {
            as_obj *v = as_record_get(a, b->keys[i]);
            if (!v || !equal_cs(v, b->items[i], cs))
                return as_bool(false);
        }
        return as_bool(true);
    }
    if (op == 0) {
        as_obj *l = as_list(1);
        as_list_push(l, a);
        return containment(l, b, op);
    }
    char sa[128];
    as_show(a, sa, sizeof sa);
    return as_raise(asErrCantCoerce, a, "Can't make %s into a list.", sa);
}

static as_obj *arith(int op, as_obj *a, as_obj *b)
{
    double x, y;
    bool ix, iy;
    if (a->kind == V_DATE || b->kind == V_DATE) {
        /* date + seconds, date - date */
        if (a->kind == V_DATE && b->kind == V_DATE && op == 1)
            return as_number(a->r - b->r);
        as_obj *d = a->kind == V_DATE ? a : b, *n = a->kind == V_DATE ? b : a;
        if (!as_to_number(n, &y, &iy) || (op != 0 && op != 1) || (op == 1 && d != a))
            return as_raise(asErrCantCoerce, n, "Can't add a date and that.");
        as_obj *r = as_new(V_DATE);
        r->r = op == 0 ? d->r + y : d->r - y;
        return r;
    }
    if (!as_to_number(a, &x, &ix))
        return cant_make(a, T("nmbr"));
    if (!as_to_number(b, &y, &iy))
        return cant_make(b, T("nmbr"));
    bool ints = ix && iy;
    double r;
    switch (op) {
    case 0: r = x + y; break;
    case 1: r = x - y; break;
    case 2: r = x * y; break;
    case 3:
        if (y == 0)
            return as_raise(asErrDivideByZero, a, "Can't divide %g by zero.", x);
        return as_real(x / y);
    case 4: /* div */
        if (y == 0)
            return as_raise(asErrDivideByZero, a, "Can't divide %g by zero.", x);
        r = trunc(x / y);
        return ints ? as_number(r) : as_number(r);
    case 5: /* mod */
        if (y == 0)
            return as_raise(asErrDivideByZero, a, "Can't divide %g by zero.", x);
        r = fmod(x, y);
        return ints ? as_number(r) : as_real(r);
    case 6:
        return as_real(pow(x, y));
    default:
        return as_raise(asErrUnsupported, NULL, NULL);
    }
    return ints && fabs(r) <= AS_INT_MAX ? as_int((int32_t)r) : as_real(r);
}

static as_obj *concat(as_obj *a, as_obj *b)
{
    if (a->kind == V_TEXT) {
        as_obj *t = as_to_text(b);
        if (!t)
            return NULL;
        as_obj *r = as_text(a->bytes, a->n);
        as_text_append(r, t->bytes, t->n);
        return r;
    }
    if (a->kind == V_RECORD && b->kind == V_RECORD) {
        as_obj *r = as_record();
        for (uint32_t i = 0; i < a->n; i++)
            as_record_set(r, a->keys[i], a->items[i]);
        for (uint32_t i = 0; i < b->n; i++)
            if (!as_record_get(r, b->keys[i]))
                as_record_set(r, b->keys[i], b->items[i]);
        return r;
    }
    as_obj *l = as_list(0);
    if (a->kind == V_LIST)
        for (uint32_t i = 0; i < a->n; i++)
            as_list_push(l, a->items[i]);
    else
        as_list_push(l, a);
    if (b->kind == V_LIST)
        for (uint32_t i = 0; i < b->n; i++)
            as_list_push(l, b->items[i]);
    else if (b->kind == V_RECORD)
        for (uint32_t i = 0; i < b->n; i++)
            as_list_push(l, b->items[i]);
    else
        as_list_push(l, b);
    return l;
}

/* ---------------------------------------------------------------------- */
/* Object specifiers on values of the script's own */

static bool word_char(uint8_t c) { return isalnum(c) || c >= 0x80; }

/* The elements of a text of a class: characters, words, paragraphs, text
 * items; each one's start and length. */
static uint32_t text_elements(as_obj *t, uint32_t cls, uint32_t **starts, uint32_t **lens)
{
    uint32_t n = 0, cap = 16;
    *starts = malloc(cap * sizeof **starts);
    *lens = malloc(cap * sizeof **lens);
#define ADD(s, l)                                                     \
    do {                                                              \
        if (n == cap) {                                               \
            cap *= 2;                                                 \
            *starts = realloc(*starts, cap * sizeof **starts);        \
            *lens = realloc(*lens, cap * sizeof **lens);              \
        }                                                             \
        (*starts)[n] = (s);                                           \
        (*lens)[n++] = (l);                                           \
    } while (0)
    const uint8_t *s = t->bytes;
    if (cls == T("cwor")) {
        for (uint32_t i = 0; i < t->n;) {
            while (i < t->n && !word_char(s[i]))
                i++;
            uint32_t a = i;
            while (i < t->n && (word_char(s[i]) || (s[i] == '\'' && i + 1 < t->n && word_char(s[i + 1]) && i > a)))
                i++;
            if (i > a)
                ADD(a, i - a);
        }
    } else if (cls == T("cpar") || cls == T("clin")) {
        uint32_t a = 0;
        for (uint32_t i = 0; i <= t->n; i++) {
            if (i == t->n || s[i] == '\r' || s[i] == '\n') {
                if (i < t->n || i > a)
                    ADD(a, i - a);
                if (i + 1 < t->n && s[i] == '\r' && s[i + 1] == '\n')
                    i++;
                a = i + 1;
            }
        }
    } else if (cls == T("citm")) {
        as_obj *d = as_delimiters();
        as_obj *delim = d->n && d->items[0]->kind == V_TEXT ? d->items[0] : NULL;
        if (!delim || !delim->n) {
            for (uint32_t i = 0; i < t->n; i++)
                ADD(i, 1);
            if (!t->n)
                ADD(0, 0);
        } else {
            uint32_t a = 0;
            bool cs = considering_case();
            for (uint32_t i = 0; i + delim->n <= t->n;) {
                if (text_cmp(&(as_obj){ .bytes = t->bytes + i, .n = delim->n }, delim, cs) == 0) {
                    ADD(a, i - a);
                    i += delim->n;
                    a = i;
                } else
                    i++;
            }
            ADD(a, t->n - a);
        }
    } else {
        for (uint32_t i = 0; i < t->n; i++)
            ADD(i, 1);
    }
#undef ADD
    return n;
}

static bool is_text_class(uint32_t c) { return c == T("ctxt") || c == T("TEXT") || c == T("itxt") || c == T("utxt"); }

/* The items of a list that are of a class (item: all of them). */
static as_obj *list_of_class(as_obj *l, uint32_t cls)
{
    if (cls == T("cobj") || cls == 0)
        return l;
    as_obj *r = as_list(0);
    for (uint32_t i = 0; i < l->n; i++) {
        uint32_t c = class_of(l->items[i]);
        if (c == cls || (is_text_class(cls) && c == T("TEXT")) || (cls == T("nmbr") && is_number(l->items[i])) ||
            (cls == T("doub") && c == T("doub")))
            as_list_push(r, l->items[i]);
    }
    return r;
}

static as_obj *cant_get(as_obj *ref)
{
    char s[256];
    as_show(ref, s, sizeof s);
    return as_raise(asErrCantGet, ref, "Can't get %s.", s);
}

static bool index_of(as_obj *k, uint32_t n, uint32_t *out)
{
    double d;
    bool is_int;
    if (!as_to_number(k, &d, &is_int))
        return false;
    int64_t i = (int64_t)d;
    if (i < 0)
        i += (int64_t)n + 1;
    if (i < 1 || i > (int64_t)n)
        return false;
    *out = (uint32_t)i - 1;
    return true;
}

/* The elements of a container value (a list or a text) of a class, as a
 * list; texts give texts. */
static as_obj *elements(as_obj *container, uint32_t cls)
{
    if (container->kind == V_LIST)
        return list_of_class(container, cls);
    if (container->kind == V_RECORD) {
        as_obj *l = as_list(container->n);
        for (uint32_t i = 0; i < container->n; i++)
            as_list_push(l, container->items[i]);
        return list_of_class(l, cls);
    }
    if (container->kind == V_TEXT) {
        uint32_t *st, *ln;
        uint32_t n = text_elements(container, cls, &st, &ln);
        as_obj *l = as_list(n);
        for (uint32_t i = 0; i < n; i++)
            as_list_push(l, as_text(container->bytes + st[i], ln[i]));
        free(st);
        free(ln);
        return l;
    }
    return NULL;
}

static as_obj *script_property(as_obj *script, as_obj *key);

static as_obj *get_property(as_obj *ref, as_obj *c, as_obj *key)
{
    uint32_t code = key->kind == V_CONST && !(key->code >> 32) ? (uint32_t)key->code : 0;
    if (c->kind == V_CONST && c->code == T("ascr")) {
        if (code == T("txdl")) {
            as_obj *d = as_delimiters();
            return d->n == 1 ? d->items[0] : d;
        }
        if (code == T("vers"))
            return as_cstr("1.1");
        if (code == T("pnam"))
            return as_cstr("AppleScript");
        return cant_get(ref);
    }
    if (c->kind == V_RECORD) {
        as_obj *v = as_record_get(c, key);
        if (v)
            return v;
        if (code == T("leng"))
            return as_int((int32_t)c->n);
        if (code == T("pcls"))
            return as_const(T("reco"));
        return cant_get(ref);
    }
    if (c->kind == V_SCRIPT && key->kind == V_IDENT) {
        as_obj *v = script_property(c, key);
        return v ? v : cant_get(ref);
    }
    switch (code) {
    case 0x6C656E67: /* 'leng' */
        if (c->kind == V_TEXT || c->kind == V_LIST)
            return as_int((int32_t)c->n);
        break;
    case 0x70636C73: /* 'pcls' */
        return as_const(class_of(c));
    case 0x70636E74: /* 'pcnt' */
        return c;
    case 0x72657374: /* 'rest' */
        if (c->kind == V_LIST) {
            as_obj *l = as_list(c->n);
            for (uint32_t i = 1; i < c->n; i++)
                as_list_push(l, c->items[i]);
            return l;
        }
        break;
    case 0x72767365: /* 'rvse' */
        if (c->kind == V_LIST) {
            as_obj *l = as_list(c->n);
            for (uint32_t i = c->n; i-- > 0;)
                as_list_push(l, c->items[i]);
            return l;
        }
        break;
    }
    if (c->kind == V_DATE) {
        time_t t = (time_t)(c->r - 2082844800.0);
        struct tm tm;
        gmtime_r(&t, &tm);
        if (code == T("year"))
            return as_int(tm.tm_year + 1900);
        if (code == T("day "))
            return as_int(tm.tm_mday);
        if (code == T("time"))
            return as_int(tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);
        if (code == T("mnth"))
            return as_int(tm.tm_mon + 1);
        if (code == T("wkdy"))
            return as_int(tm.tm_wday + 1);
    }
    return cant_get(ref);
}

/* The value an object specifier on the script's own values names. */
static as_obj *get_ref(as_obj *ref)
{
    as_obj *c = as_deref(ref->a);
    if (!c)
        return NULL;
    if (c->kind == V_APP)
        return as_app_get(ref);
    if (ref->btype == REF_PROPERTY)
        return get_property(ref, c, ref->b);
    uint32_t cls = ref->b && ref->b->kind == V_CONST ? (uint32_t)ref->b->code : T("cobj");
    if (c->kind == V_TEXT && is_text_class(cls) && ref->btype == REF_RANGE) {
        /* text a thru b: characters a to b as one text */
        uint32_t a, b;
        as_obj *ka = as_deref(ref->c), *kb = as_deref(ref->d);
        if (!ka || !kb)
            return NULL;
        if (!index_of(ka, c->n, &a) || !index_of(kb, c->n, &b))
            return cant_get(ref);
        if (a > b) {
            uint32_t t = a;
            a = b;
            b = t;
        }
        return as_text(c->bytes + a, b - a + 1);
    }
    if (c->kind == V_TEXT && is_text_class(cls) && ref->btype == REF_EVERY)
        return c;
    as_obj *els = elements(c, cls == T("TEXT") || cls == T("ctxt") ? T("cha ") : cls);
    if (!els)
        return cant_get(ref);
    switch (ref->btype) {
    case REF_EVERY:
        return els;
    case REF_INDEX: {
        as_obj *k = as_deref(ref->c);
        if (!k)
            return NULL;
        if (k->kind == V_CONST) { /* first, last, middle, some */
            if (!els->n)
                return cant_get(ref);
            if (k->code == T("firs"))
                return els->items[0];
            if (k->code == T("last"))
                return els->items[els->n - 1];
            if (k->code == T("midd"))
                return els->items[(els->n - 1) / 2];
            if (k->code == T("any "))
                return els->items[as_random() % els->n];
        }
        uint32_t i;
        if (!index_of(k, els->n, &i))
            return cant_get(ref);
        return els->items[i];
    }
    case REF_SOME:
        if (!els->n)
            return cant_get(ref);
        return els->items[as_random() % els->n];
    case REF_MIDDLE:
        if (!els->n)
            return cant_get(ref);
        return els->items[(els->n - 1) / 2];
    case REF_RANGE: {
        as_obj *ka = as_deref(ref->c), *kb = as_deref(ref->d);
        if (!ka || !kb)
            return NULL;
        uint32_t a, b;
        if (!index_of(ka, els->n, &a) || !index_of(kb, els->n, &b))
            return cant_get(ref);
        if (a > b) {
            uint32_t t = a;
            a = b;
            b = t;
        }
        as_obj *l = as_list(b - a + 1);
        for (uint32_t i = a; i <= b; i++)
            as_list_push(l, els->items[i]);
        return l;
    }
    case REF_NAME: {
        /* a record's item by its user label */
        if (c->kind == V_RECORD) {
            as_obj *v = as_record_get(c, ref->c);
            if (v)
                return v;
        }
        return cant_get(ref);
    }
    }
    return cant_get(ref);
}

as_obj *as_deref(as_obj *v)
{
    for (int depth = 0; v && v->kind == V_REF; depth++) {
        if (depth > 64)
            return as_raise(asErrCantGet, v, "A reference refers to itself.");
        if (as_app_rooted(v))
            return as_app_get(v);
        v = get_ref(v);
    }
    return v;
}

bool as_app_rooted(as_obj *v)
{
    while (v && v->kind == V_REF)
        v = v->a;
    return v && v->kind == V_APP;
}

/* set (or copy) value into what a specifier names */
static as_obj *set_ref(as_obj *ref, as_obj *value)
{
    if (ref->kind != V_REF) {
        char s[128];
        as_show(ref, s, sizeof s);
        return as_raise(-10006, ref, "Can't set %s.", s);
    }
    if (as_app_rooted(ref))
        return as_app_set(ref, value);
    as_obj *c = as_deref(ref->a);
    if (!c)
        return NULL;
    uint32_t code = ref->b && ref->b->kind == V_CONST ? (uint32_t)ref->b->code : 0;
    if (ref->btype == REF_PROPERTY) {
        if (c->kind == V_CONST && c->code == T("ascr") && code == T("txdl")) {
            as_obj *l = value->kind == V_LIST ? value : NULL;
            delimiters = as_list(1);
            if (l) {
                for (uint32_t i = 0; i < l->n; i++) {
                    as_obj *t = as_to_text(l->items[i]);
                    if (!t)
                        return NULL;
                    as_list_push(delimiters, t);
                }
            } else {
                as_obj *t = as_to_text(value);
                if (!t)
                    return NULL;
                as_list_push(delimiters, t);
            }
            return value;
        }
        if (c->kind == V_RECORD) {
            as_record_set(c, ref->b, value);
            return value;
        }
    }
    if (c->kind == V_LIST) {
        if (ref->btype == REF_END) {
            as_list_push(c, value);
            return value;
        }
        if (ref->btype == REF_BEGINNING) {
            as_list_insert(c, 0, value);
            return value;
        }
        if (ref->btype == REF_INDEX && (code == T("cobj") || code == 0)) {
            as_obj *k = as_deref(ref->c);
            uint32_t i;
            if (!k)
                return NULL;
            if (!index_of(k, c->n, &i))
                return cant_get(ref);
            c->items[i] = value;
            return value;
        }
    }
    char s[128], vs[128];
    as_show(ref, s, sizeof s);
    as_show(value, vs, sizeof vs);
    return as_raise(-10006, ref, "Can't set %s to %s.", s, vs);
}

/* ---------------------------------------------------------------------- */
/* The interpreter */

static void push(as_obj *v)
{
    if (sp == stack_cap) {
        stack_cap = stack_cap ? stack_cap * 2 : 256;
        stack = realloc(stack, stack_cap * sizeof *stack);
    }
    stack[sp++] = v ? v : as_undef();
}

static frame *top(void) { return nframes ? &frames[nframes - 1] : NULL; }

static as_obj *pop(void)
{
    frame *f = top();
    if (sp == 0 || (f && sp <= f->sp))
        return as_undef(); /* an underflow: the bytecode wasn't as we understood it */
    return stack[--sp];
}

static block *push_block(int kind)
{
    if (nblocks == blocks_cap) {
        blocks_cap = blocks_cap ? blocks_cap * 2 : 32;
        blocks = realloc(blocks, blocks_cap * sizeof *blocks);
    }
    block *b = &blocks[nblocks++];
    memset(b, 0, sizeof *b);
    b->kind = kind;
    b->sp = sp;
    return b;
}

static bool considering_case(void)
{
    frame *f = top();
    for (uint32_t i = nblocks; f && i-- > f->blk;)
        if (blocks[i].kind == BLK_CONSIDER)
            return blocks[i].case_sensitive;
    return false;
}

/* The target of commands: the innermost tell, else the script itself. */
static as_obj *current_target(void)
{
    frame *f = top();
    for (uint32_t i = nblocks; f && i-- > f->blk;)
        if (blocks[i].kind == BLK_TELL)
            return blocks[i].v;
    return f ? f->script : as_undef();
}

static as_obj *lit(frame *f, uint32_t i)
{
    if (!f->lits || i >= f->lits->n)
        return as_raise(asErrCorruptData, NULL, "The script refers to literal %u, which it doesn't have.", i);
    return f->lits->items[i];
}

static as_obj *script_property(as_obj *script, as_obj *key)
{
    as_obj *names = script->a;
    for (uint32_t i = 0; names && i < names->n; i++)
        if (names->items[i]->kind == V_IDENT && key_eq(names->items[i], key))
            return script->b->items[i];
    return NULL;
}

static as_obj *find_handler(as_obj *script, as_obj *name)
{
    as_obj *v = script && script->kind == V_SCRIPT ? script_property(script, name) : NULL;
    return v && v->kind == V_HANDLER ? v : NULL;
}

static as_obj *find_event_handler(as_obj *script, uint32_t cls, uint32_t id)
{
    if (!script || script->kind != V_SCRIPT)
        return NULL;
    for (uint32_t i = 0; i < script->a->n; i++) {
        as_obj *n = script->a->items[i];
        if (n && n->kind == V_EVENT && n->ev[0] == cls && n->ev[1] == id && script->b->items[i]->kind == V_HANDLER)
            return script->b->items[i];
    }
    return NULL;
}

/* A global (by name): the script's globals, then its properties and
 * handlers by name. */
static as_obj *get_global(frame *f, as_obj *name)
{
    if (name->kind == V_CONST) {
        switch ((uint32_t)name->code) {
        case 0x72736C74: /* 'rslt' */
            return f->result ? f->result : as_undef();
        case 0x61736372: /* 'ascr': AppleScript, the object */
            return name;
        case 0x70692020: /* 'pi  ' */
            return as_real(M_PI);
        case 0x72657420: /* 'ret ' */
            return as_cstr("\r");
        case 0x73706163: /* 'spac' */
            return as_cstr(" ");
        case 0x74616220: /* 'tab ' */
            return as_cstr("\t");
        case 0x71756F74: /* 'quot' */
            return as_cstr("\"");
        case 0x7478646C: /* 'txdl' */
        {
            as_obj *d = as_delimiters();
            return d->n == 1 ? d->items[0] : d;
        }
        }
        return name; /* a class or constant used as a value */
    }
    as_obj *script = f->script;
    as_obj *v = script->c ? as_record_get(script->c, name) : NULL;
    if (v)
        return v;
    v = script_property(script, name);
    if (v)
        return v;
    char n[128];
    as_show(name, n, sizeof n);
    return as_raise(asErrNotDefined, name, "The variable %s is not defined.", n);
}

static void set_global(frame *f, as_obj *name, as_obj *v)
{
    if (name->kind == V_CONST) {
        if (name->code == T("rslt"))
            f->result = v;
        else if (name->code == T("txdl"))
            set_ref(&(as_obj){ .kind = V_REF, .btype = REF_PROPERTY, .a = as_const(T("ascr")), .b = name }, v);
        return;
    }
    as_obj *script = f->script;
    as_obj *names = script->a;
    if (!as_record_get(script->c, name))
        for (uint32_t i = 0; names && i < names->n; i++)
            if (names->items[i]->kind == V_IDENT && key_eq(names->items[i], name)) {
                script->b->items[i] = v;
                return;
            }
    as_record_set(script->c, name, v);
}

/* Enter a handler: a new frame with its arguments in the first locals. */
static bool enter(as_obj *script, as_obj *h, as_obj **args, uint32_t nargs)
{
    uint32_t nparams = h->b ? h->b->n : 0;
    if (h->i == 1 && nargs != nparams) {
        char n[64];
        as_show(h->a, n, sizeof n);
        as_raise(-1721, NULL, "%s takes %u parameters, not %u.", n, nparams, nargs);
        return false;
    }
    if (nframes == frames_cap) {
        frames_cap = frames_cap ? frames_cap * 2 : 16;
        frames = realloc(frames, frames_cap * sizeof *frames);
    }
    if (nframes > 400) {
        as_raise(-2706, NULL, "Stack overflow.");
        return false;
    }
    frame *f = &frames[nframes];
    memset(f, 0, sizeof *f);
    f->handler = h;
    f->script = script;
    f->nlocals = h->n > nparams ? h->n : nparams;
    f->locals = calloc(f->nlocals ? f->nlocals : 1, sizeof *f->locals);
    for (uint32_t i = 0; i < f->nlocals; i++)
        f->locals[i] = NULL;
    for (uint32_t i = 0; i < nargs && i < f->nlocals; i++)
        f->locals[i] = args[i];
    f->code = h->d->bytes;
    f->len = h->d->n;
    f->sp = sp;
    f->blk = nblocks;
    f->lits = h->c;
    nframes++;
    return true;
}

static void leave(void)
{
    frame *f = top();
    sp = f->sp;
    nblocks = f->blk;
    free(f->locals);
    nframes--;
}

static uint32_t operand(frame *f)
{
    if (f->pc + 2 > f->len) {
        f->pc = f->len;
        return 0;
    }
    uint32_t v = (uint32_t)f->code[f->pc] << 8 | f->code[f->pc + 1];
    f->pc += 2;
    return v;
}

/* A jump's target: relative to its operand. */
static uint32_t jump_target(frame *f)
{
    uint32_t at = f->pc;
    int16_t off = (int16_t)operand(f);
    return (uint32_t)((int32_t)at + off);
}

static as_obj *make_ref(int form)
{
    as_obj *r = as_new(V_REF);
    r->btype = (int16_t)form;
    switch (form) {
    case REF_PROPERTY:
        r->b = pop();
        r->a = pop();
        break;
    case REF_EVERY:
    case REF_SOME:
    case REF_MIDDLE:
        r->b = pop();
        r->a = pop();
        break;
    case REF_INDEX:
    case REF_KEY:
    case REF_NAME:
    case REF_RELATIVE:
        r->c = pop();
        r->b = pop();
        r->a = pop();
        break;
    case REF_RANGE:
        r->d = pop();
        r->c = pop();
        r->b = pop();
        r->a = pop();
        break;
    case REF_BEGINNING:
    case REF_END:
        r->a = pop();
        break;
    default:
        return as_raise(asErrUnsupported, NULL, "Filtered references (whose) aren't supported yet.");
    }
    /* A key that is a specifier itself (item (count of x) of y) is
     * evaluated now, as AppleScript does. */
    if (r->c && r->c->kind == V_REF && !as_app_rooted(r->c))
        r->c = as_deref(r->c);
    if (r->d && r->d->kind == V_REF && !as_app_rooted(r->d))
        r->d = as_deref(r->d);
    return r;
}

/* An event a script sends: to its own handler for it when the target is
 * a script that has one, otherwise a command (as_osax.c). */
static bool send_event(as_obj *event, as_obj *direct, as_obj *params, as_obj *target)
{
    if (target && target->kind == V_SCRIPT) {
        as_obj *h = find_event_handler(target, event->ev[0], event->ev[1]);
        if (h) {
            as_obj *args[1] = { direct ? direct : as_undef() };
            return enter(target, h, args, h->i == 2 ? 1 : 0);
        }
    }
    as_obj *r = as_command(event, direct, params, target);
    if (!r)
        return false;
    push(r);
    return true;
}

/* MessageSend: the direct parameter (`it` when there is none), the other
 * parameters as keyword and value pairs, and their count. */
static bool message_send(frame *f, uint32_t li)
{
    as_obj *name = lit(f, li);
    if (!name)
        return false;
    as_obj *cnt = pop();
    int32_t n = cnt->kind == V_INT ? cnt->i : 0;
    if (n < 0 || (uint32_t)n > sp - f->sp)
        n = 0;
    as_obj **items = &stack[sp - (uint32_t)n];
    as_obj *params = as_record();
    for (int32_t i = 0; i + 1 < n; i += 2) {
        as_obj *v = items[i + 1];
        if (v->kind == V_REF && !as_app_rooted(v)) {
            v = as_deref(v);
            if (!v)
                return false;
        }
        as_record_set(params, items[i], v);
    }
    sp -= (uint32_t)n;
    as_obj *direct = pop();
    as_obj *target = current_target();
    if (direct == target)
        direct = NULL;
    if (name->kind == V_EVENT)
        return send_event(name, direct, params, target);
    /* A handler called with labelled parameters (or none). */
    as_obj *script = target && target->kind == V_SCRIPT ? target : f->script;
    as_obj *h = find_handler(script, name);
    if (!h) {
        char s[128];
        as_show(name, s, sizeof s);
        return as_raise(asErrNotHandled, name, "\xC7script\xC8 doesn't understand the %s message.", s) != NULL;
    }
    return enter(script, h, NULL, 0);
}

static bool positional_send(frame *f, uint32_t li)
{
    as_obj *name = lit(f, li);
    if (!name)
        return false;
    as_obj *cnt = pop();
    int32_t n = cnt->kind == V_INT ? cnt->i : 0;
    if (n < 0 || (uint32_t)n > sp - f->sp)
        n = 0;
    as_obj *args[64];
    if (n > 64)
        return as_raise(asErrUnsupported, NULL, "Too many parameters.") != NULL;
    for (int32_t i = n; i-- > 0;) {
        as_obj *v = pop();
        if (v->kind == V_REF && !as_app_rooted(v)) {
            v = as_deref(v);
            if (!v)
                return false;
        }
        args[i] = v;
    }
    as_obj *target = pop();
    if (target->kind == V_REF)
        target = as_deref(target);
    if (!target)
        return false;
    if (target->kind == V_SCRIPT) {
        as_obj *h = find_handler(target, name);
        if (h)
            return enter(target, h, args, (uint32_t)n);
    }
    char s[128], t[128];
    as_show(name, s, sizeof s);
    as_show(target, t, sizeof t);
    return as_raise(asErrNotHandled, target, "%s doesn't understand the %s message.", t, s) != NULL;
}

static as_obj *get_local(frame *f, uint32_t i)
{
    if (i >= f->nlocals)
        return as_raise(asErrCorruptData, NULL, "The script refers to variable %u, which it doesn't have.", i);
    if (!f->locals[i]) {
        char n[128] = "a variable";
        if (f->handler && i < f->handler->n)
            as_show(f->handler->items[i], n, sizeof n);
        return as_raise(asErrNotDefined, NULL, "The variable %s is not defined.", n);
    }
    return f->locals[i];
}

static bool set_local(frame *f, uint32_t i, as_obj *v)
{
    if (i >= f->nlocals)
        return as_raise(asErrCorruptData, NULL, "The script refers to variable %u, which it doesn't have.", i) != NULL;
    f->locals[i] = v;
    return true;
}

/* A variable named in a handler (on error's): a local of that name, else a global. */
static void set_named(frame *f, as_obj *name, as_obj *v)
{
    if (!name || name->kind != V_IDENT)
        return;
    for (uint32_t i = 0; f->handler && i < f->handler->n && i < f->nlocals; i++)
        if (f->handler->items[i] && key_eq(f->handler->items[i], name)) {
            f->locals[i] = v;
            return;
        }
    set_global(f, name, v);
}

static block *innermost(int kind)
{
    frame *f = top();
    for (uint32_t i = nblocks; f && i-- > f->blk;)
        if (blocks[i].kind == kind)
            return &blocks[i];
    return NULL;
}

/* Leave a loop for its exit with the loop's value (none) on the stack. */
static void exit_loop(frame *f, block *b)
{
    sp = b->sp;
    nblocks = (uint32_t)(b - blocks);
    f->pc = b->target;
    push(as_undef());
}

/* The repeat instructions: on the first pass their operands come off the
 * stack; on later ones the stack holds the body's value, dropped. */
static bool repeat_op(frame *f, int op, uint32_t var)
{
    block *b = innermost(BLK_LOOP);
    if (!b)
        return as_raise(asErrCorruptData, NULL, "A repeat outside a loop.") != NULL;
    as_obj *cond = NULL;
    if (op == 0x19 || op == 0x1a) /* while, until: the condition is evaluated each time */
        cond = as_deref(pop());
    if (op == 0x19 || op == 0x1a) {
        if (!cond)
            return false;
        bool t;
        if (!as_truth(cond, &t))
            return cant_make(cond, T("bool")) != NULL;
        sp = b->sp;
        if (t != (op == 0x19))
            exit_loop(f, b);
        return true;
    }
    if (!b->state) {
        b->state = 1;
        pop(); /* the slot for the body's value */
        double d;
        bool is_int;
        if (op == 0x18) { /* n times: count, 1 */
            as_obj *start = as_deref(pop()), *count = as_deref(pop());
            if (!start || !count)
                return false;
            if (!as_to_number(count, &d, &is_int))
                return cant_make(count, T("long")) != NULL;
            b->end = floor(d);
            b->cur = 0;
            b->step = 1;
        } else if (op == 0x1c) { /* with i from a to b by c */
            as_obj *step = as_deref(pop()), *end = as_deref(pop()), *start = as_deref(pop());
            if (!step || !end || !start)
                return false;
            double s = 1, e, a;
            if (step->kind != V_UNDEF && !as_to_number(step, &s, &is_int))
                return cant_make(step, T("long")) != NULL;
            if (!as_to_number(end, &e, &is_int))
                return cant_make(end, T("long")) != NULL;
            if (!as_to_number(start, &a, &is_int))
                return cant_make(start, T("long")) != NULL;
            if (s == 0)
                return as_raise(asErrUnsupported, step, "A repeat can't go by 0.") != NULL;
            b->cur = a - s;
            b->end = e;
            b->step = s;
            b->var = (int)var;
        } else { /* with x in a list: the list and its count, 1 */
            pop();
            as_obj *count = as_deref(pop()), *coll = as_deref(pop());
            if (!count || !coll)
                return false;
            if (coll->kind != V_LIST)
                coll = as_coerce(coll, T("list"));
            if (!coll)
                return false;
            b->v = coll;
            b->cur = 0;
            b->end = coll->n;
            b->step = 1;
            b->var = (int)var;
        }
    }
    sp = b->sp;
    b->cur += b->step;
    bool done = op == 0x18 || op == 0x1b ? b->cur > b->end : (b->step > 0 ? b->cur > b->end : b->cur < b->end);
    if (done) {
        exit_loop(f, b);
        return true;
    }
    if (op == 0x1c)
        return set_local(f, (uint32_t)b->var, as_number(b->cur));
    if (op == 0x1b) {
        uint32_t i = (uint32_t)b->cur - 1;
        if (!b->v || i >= b->v->n) {
            exit_loop(f, b);
            return true;
        }
        return set_local(f, (uint32_t)b->var, b->v->items[i]);
    }
    return true;
}

/* Raise the error an `error` statement gives: message, then parameters. */
static bool error_op(void)
{
    as_obj *msg = as_deref(pop());
    as_obj *cnt = pop();
    int32_t n = cnt->kind == V_INT ? cnt->i : 0;
    int32_t num = -2700;
    as_obj *offending = NULL, *partial = NULL;
    for (int32_t i = n - 2; i >= 0; i -= 2) {
        (void)i;
        as_obj *v = as_deref(pop()), *k = pop();
        if (!v)
            return false;
        uint32_t code = k->kind == V_CONST ? (uint32_t)k->code : 0;
        double d;
        bool is_int;
        if (code == T("errn") && as_to_number(v, &d, &is_int))
            num = (int32_t)d;
        else if (code == T("erob"))
            offending = v;
        else if (code == T("ptlr"))
            partial = v;
    }
    pop(); /* the target */
    char text[512] = "";
    if (msg && msg->kind != V_UNDEF) {
        as_obj *t = as_to_text(msg);
        if (!t)
            return false;
        snprintf(text, sizeof text, "%.*s", (int)t->n, (const char *)t->bytes);
    } else if (num != -2700) {
        snprintf(text, sizeof text, "%s", error_text(num));
    }
    as_raise(num, offending, "%s", text);
    as_last_error.partial = partial;
    return false;
}

static as_obj *coerce_op(void)
{
    as_obj *type = as_deref(pop()), *v = pop();
    if (!type)
        return NULL;
    if (type->kind != V_CONST)
        return as_raise(asErrCantCoerce, type, "Can't coerce to that.");
    return as_coerce(v, (uint32_t)type->code);
}

/* Unwind to the innermost try in the frames above base (true: caught). */
static bool unwind(uint32_t base_frames)
{
    if (error_abort)
        return false;
    for (;;) {
        frame *f = top();
        if (!f || nframes <= base_frames)
            return false;
        for (uint32_t i = nblocks; i-- > f->blk;) {
            if (blocks[i].kind == BLK_TRY) {
                block b = blocks[i];
                nblocks = i;
                sp = b.sp;
                f->pc = b.target;
                error_pending = false;
                caught_offending = as_last_error.offending;
                caught_partial = as_last_error.partial;
                return true;
            }
        }
        if (nframes == base_frames + 1)
            return false;
        leave();
    }
}

/* Each instruction counts as some of the processor's on the machine's
 * clock (AppleScript 1.1 ran a few tens of thousands a second on a 68040),
 * so the instruction limit and the clock go on while a script runs. */
enum { INSTRUCTION_COST = 1000 };

static bool out_of_time(void)
{
    if (!g_cpu)
        return false;
    g_cpu->icount += INSTRUCTION_COST;
    if (g_cpu->halted)
        return true;
    uint64_t cap = trap_instruction_limit();
    if (cap && g_cpu->icount >= cap) {
        cpu_halt(g_cpu, "instruction limit reached in a script");
        return true;
    }
    if ((instructions & 1023) == 0)
        machine_advance_ticks();
    return false;
}

static bool step(frame *f);

/* Run until the frame at depth base returns: its result, or NULL with the
 * error in as_last_error. */
static as_obj *run(uint32_t base)
{
    as_obj *result = NULL;
    while (nframes > base) {
        frame *f = top();
        /* Only the outermost run collects: a nested one (an event the
         * script sent itself) runs inside a command whose values are in C
         * variables. */
        if (base == 0 && allocated_since > 200000)
            as_collect();
        instructions++;
        if (out_of_time()) {
            as_raise(asErrUserCanceled, NULL, "The script was stopped.");
            error_abort = true;
        } else if (f->pc >= f->len) {
            /* fell off the end: return the statement's value */
            as_obj *v = sp > f->sp ? stack[sp - 1] : as_undef();
            leave();
            if (nframes == base) {
                result = v;
                break;
            }
            push(v);
            continue;
        } else if (f->code[f->pc] == 0x0F) { /* Return: the value, to the caller */
            f->pc++;
            as_obj *v = sp > f->sp ? stack[--sp] : as_undef();
            if (v->kind == V_REF && !as_app_rooted(v))
                v = as_deref(v);
            if (v) {
                leave();
                if (nframes == base) {
                    result = v;
                    break;
                }
                push(v);
            }
        } else {
            step(f);
        }
        if (error_pending) {
            if (!unwind(base)) {
                while (nframes > base)
                    leave();
                return NULL;
            }
        }
    }
    return result ? result : as_undef();
}

/* BLUEBOX_AS_TRACE: each instruction to the log, with the stack's depth
 * and its top (for working out what a script does). */
static int trace = -1;

static void trace_op(frame *f)
{
    char top[96] = "";
    if (sp > f->sp)
        as_show(stack[sp - 1], top, sizeof top);
    char name[48] = "?";
    if (f->handler)
        as_show(f->handler->a, name, sizeof name);
    log_printf("AS %s %04X %02X sp %u  %s\n", name, f->pc, f->code[f->pc], sp - f->sp, top);
}

static bool step(frame *f)
{
    if (trace < 0)
        trace = getenv("BLUEBOX_AS_TRACE") != NULL;
    if (trace)
        trace_op(f);
    uint8_t op = f->code[f->pc++];
    as_obj *a, *b, *v;
    switch (op) {
    case 0x00: /* Equal */
    case 0x01: /* NotEqual */
        b = as_deref(pop());
        a = as_deref(pop());
        if (!a || !b)
            return false;
        push(as_bool(as_equal(a, b) == (op == 0x00)));
        return true;
    case 0x02: /* GreaterThan */
    case 0x03: /* GreaterThanOrEqual */
    case 0x04: /* LessThan */
    case 0x05: { /* LessThanOrEqual */
        b = as_deref(pop());
        a = as_deref(pop());
        int c;
        if (!a || !b || !compare(a, b, &c))
            return false;
        bool r = op == 0x02 ? c > 0 : op == 0x03 ? c >= 0 : op == 0x04 ? c < 0 : c <= 0;
        push(as_bool(r));
        return true;
    }
    case 0x06: /* StartsWith */
    case 0x07: /* EndsWith */
    case 0x08: /* Contains */
        b = as_deref(pop());
        a = as_deref(pop());
        if (!a || !b)
            return false;
        v = containment(a, b, op == 0x08 ? 0 : op == 0x06 ? 1 : 2);
        if (!v)
            return false;
        push(v);
        return true;
    case 0x09: /* And */
    case 0x0A: { /* Or */
        uint32_t t = jump_target(f);
        a = as_deref(pop());
        bool x;
        if (!a)
            return false;
        if (!as_truth(a, &x))
            return cant_make(a, T("bool")) != NULL;
        if (x == (op == 0x0A)) {
            push(as_bool(x));
            f->pc = t;
        }
        return true;
    }
    case 0x0B: /* Not */
    {
        a = as_deref(pop());
        bool x;
        if (!a)
            return false;
        if (!as_truth(a, &x))
            return cant_make(a, T("bool")) != NULL;
        push(as_bool(!x));
        return true;
    }
    case 0x0C: /* MessageSend */
        return message_send(f, operand(f));
    case 0x0D: /* MakeList */
    case 0x76: { /* MakeVector */
        as_obj *cnt = pop();
        int32_t n = cnt->kind == V_INT ? cnt->i : 0;
        if (n < 0 || (uint32_t)n > sp - f->sp)
            n = 0;
        as_obj *l = as_list((uint32_t)n);
        for (int32_t i = 0; i < n; i++) {
            as_obj *e = stack[sp - (uint32_t)n + (uint32_t)i];
            if (e->kind == V_REF && !as_app_rooted(e))
                e = as_deref(e);
            if (!e)
                return false;
            as_list_push(l, e);
        }
        sp -= (uint32_t)n;
        push(l);
        return true;
    }
    case 0x0E: { /* MakeRecord */
        as_obj *cnt = pop();
        int32_t n = cnt->kind == V_INT ? cnt->i : 0;
        if (n < 0 || (uint32_t)n > sp - f->sp)
            n = 0;
        as_obj *r = as_record();
        for (int32_t i = 0; i + 1 < n; i += 2) {
            as_obj *e = stack[sp - (uint32_t)n + (uint32_t)i + 1];
            if (e->kind == V_REF && !as_app_rooted(e))
                e = as_deref(e);
            if (!e)
                return false;
            as_record_set(r, stack[sp - (uint32_t)n + (uint32_t)i], e);
        }
        sp -= (uint32_t)n;
        push(r);
        return true;
    }
    case 0x10: /* Continue */
    case 0x4A: { /* PositionalContinue: the event goes on to the applet */
        uint32_t li = operand(f);
        as_obj *name = lit(f, li);
        if (!name)
            return false;
        as_obj *cnt = pop();
        int32_t n = cnt->kind == V_INT ? cnt->i : 0;
        for (int32_t i = 0; i < n; i++)
            pop();
        pop();
        if (name->kind == V_EVENT && name->ev[0] == T("aevt") && name->ev[1] == T("quit"))
            as_quit_requested = true;
        push(as_undef());
        return true;
    }
    case 0x11: /* ObjectAliasQuote: `a reference to`, which GetData leaves as it is */
        a = pop();
        if (a->kind == V_REF) {
            v = as_new(V_REF);
            *v = (as_obj){ .kind = V_REF, .btype = a->btype, .gc_next = v->gc_next, .i = 1,
                           .a = a->a, .b = a->b, .c = a->c, .d = a->d };
            a = v;
        }
        push(a);
        return true;
    case 0x12: /* Tell */
    {
        operand(f);
        a = pop();
        if (a->kind == V_REF && !as_app_rooted(a))
            a = as_deref(a);
        if (!a)
            return false;
        block *bl = push_block(BLK_TELL);
        bl->v = a;
        return true;
    }
    case 0x13: { /* Consider: considering, then ignoring */
        operand(f);
        as_obj *cons = pop();
        pop();
        bool cs = considering_case();
        if (cons->kind == V_LIST)
            for (uint32_t i = 0; i < cons->n; i++)
                if (cons->items[i]->kind == V_CONST && (uint32_t)cons->items[i]->code == T("case"))
                    cs = true;
        block *bl = push_block(BLK_CONSIDER);
        bl->case_sensitive = cs;
        return true;
    }
    case 0x14: { /* ErrorHandler */
        uint32_t t = jump_target(f);
        block *bl = push_block(BLK_TRY);
        bl->target = t;
        return true;
    }
    case 0x15: /* Error */
        return error_op();
    case 0x16: { /* Exit */
        block *bl = innermost(BLK_LOOP);
        if (!bl)
            return as_raise(asErrCorruptData, NULL, "An exit outside a loop.") != NULL;
        exit_loop(f, bl);
        return true;
    }
    case 0x17: { /* LinkRepeat */
        uint32_t t = jump_target(f);
        block *bl = push_block(BLK_LOOP);
        bl->target = t;
        return true;
    }
    case 0x18: /* RepeatNTimes */
    case 0x19: /* RepeatWhile */
    case 0x1A: /* RepeatUntil */
        return repeat_op(f, op, 0);
    case 0x1B: /* RepeatInCollection */
    case 0x1C: /* RepeatInRange */
        return repeat_op(f, op, operand(f));
    case 0x1D: { /* TestIf */
        uint32_t t = jump_target(f);
        a = as_deref(pop());
        bool x;
        if (!a)
            return false;
        if (!as_truth(a, &x))
            return cant_make(a, T("bool")) != NULL;
        if (!x)
            f->pc = t;
        return true;
    }
    case 0x1E: /* Add */
    case 0x1F: /* Subtract */
    case 0x20: /* Multiply */
    case 0x21: /* Divide */
    case 0x22: /* Quotient */
    case 0x23: /* Remainder */
    case 0x24: /* Power */
        b = as_deref(pop());
        a = as_deref(pop());
        if (!a || !b)
            return false;
        v = arith(op - 0x1E, a, b);
        if (!v)
            return false;
        push(v);
        return true;
    case 0x25: /* Concatenate */
        b = as_deref(pop());
        a = as_deref(pop());
        if (!a || !b)
            return false;
        v = concat(a, b);
        if (!v)
            return false;
        push(v);
        return true;
    case 0x26: /* Coerce */
        v = coerce_op();
        if (!v)
            return false;
        push(v);
        return true;
    case 0x27: /* Negate */
        a = as_deref(pop());
        if (!a)
            return false;
        v = arith(1, as_int(0), a);
        if (!v)
            return false;
        push(v);
        return true;
    case 0x28: /* GetData */
    case 0x45:
        a = pop();
        if (a->kind == V_REF && a->i) { /* a reference to: the reference itself */
            v = as_new(V_REF);
            *v = (as_obj){ .kind = V_REF, .btype = a->btype, .gc_next = v->gc_next, .a = a->a, .b = a->b,
                           .c = a->c, .d = a->d };
            push(v);
            return true;
        }
        v = as_deref(a);
        if (!v)
            return false;
        push(v);
        return true;
    case 0x29: /* PushMe */
        push(f->script);
        return true;
    case 0x2A: /* PushIt */
        push(current_target());
        return true;
    case 0x2B: /* PositionalMessageSend */
        return positional_send(f, operand(f));
    case 0x2C: case 0x2D: case 0x2E: case 0x2F: case 0x30: case 0x31:
    case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: /* MakeObjectAlias */
        v = make_ref(op - 0x2C);
        if (!v)
            return false;
        push(v);
        return true;
    case 0x46: /* SetData */
    case 0x47: { /* CopyData */
        as_obj *target = pop();
        v = pop();
        if (v->kind == V_REF && !as_app_rooted(v))
            v = as_deref(v);
        if (!v)
            return false;
        if (op == 0x47)
            v = as_clone(v);
        if (!set_ref(target, v))
            return false;
        push(v);
        return true;
    }
    case 0x4F: /* StoreResult */
        f->result = pop();
        return true;
    case 0x50: /* GetResult */
        push(f->result);
        return true;
    case 0x51: /* Clone */
        a = as_deref(pop());
        if (!a)
            return false;
        push(as_clone(a));
        return true;
    case 0x55: { /* EndTell */
        block *bl = innermost(BLK_TELL);
        if (bl)
            nblocks = (uint32_t)(bl - blocks);
        return true;
    }
    case 0x56: { /* EndConsider */
        block *bl = innermost(BLK_CONSIDER);
        if (bl)
            nblocks = (uint32_t)(bl - blocks);
        return true;
    }
    case 0x57: { /* EndErrorHandler: the try's body ended without an error */
        uint32_t t = jump_target(f);
        block *bl = innermost(BLK_TRY);
        if (bl)
            nblocks = (uint32_t)(bl - blocks);
        f->pc = t;
        return true;
    }
    case 0x58: { /* HandleError: on error's variables */
        uint32_t m = operand(f), r = operand(f);
        as_obj *mv = lit(f, m), *rv = lit(f, r);
        if (!mv || !rv)
            return false;
        if (mv->kind == V_IDENT)
            set_named(f, mv, as_cstr(as_last_error.message));
        if (rv->kind == V_RECORD) {
            for (uint32_t i = 0; i < rv->n; i++) {
                uint32_t code = rv->keys[i]->kind == V_CONST ? (uint32_t)rv->keys[i]->code : 0;
                as_obj *val = code == T("errn")   ? as_int(as_last_error.number)
                              : code == T("erob") ? (caught_offending ? caught_offending : as_missing())
                              : code == T("ptlr") ? (caught_partial ? caught_partial : as_missing())
                                                  : as_missing();
                set_named(f, rv->items[i], val);
            }
        }
        return true;
    }
    case 0x59: /* Jump */
        f->pc = jump_target(f);
        return true;
    case 0x5A: /* Pop */
        pop();
        return true;
    case 0x5B: /* Dup */
        a = pop();
        push(a);
        push(a);
        return true;
    case 0x5C: /* GCSwap */
        b = pop();
        a = pop();
        push(b);
        push(a);
        return true;
    case 0x5D: /* PushVariableExtended */
        v = get_local(f, operand(f));
        if (!v)
            return false;
        push(v);
        return true;
    case 0x5E: /* PopVariableExtended */
        a = pop();
        push(a);
        return set_local(f, operand(f), a);
    case 0x5F: /* PushGlobalExtended */
        a = lit(f, operand(f));
        if (!a || !(v = get_global(f, a)))
            return false;
        push(v);
        return true;
    case 0x60: /* PopGlobalExtended */
        a = lit(f, operand(f));
        if (!a)
            return false;
        b = pop();
        push(b);
        set_global(f, a, b);
        return true;
    case 0x61: /* PushLiteralExtended */
        a = lit(f, operand(f));
        if (!a)
            return false;
        push(a->kind == V_LIST || a->kind == V_RECORD ? as_clone(a) : a);
        return true;
    case 0x62: /* PushParentVariable */
    case 0x63: { /* PopParentVariable: the script's properties */
        uint32_t level = operand(f), index = operand(f);
        as_obj *s = f->script;
        (void)level;
        if (!s || s->kind != V_SCRIPT || index >= s->b->n)
            return as_raise(asErrCorruptData, NULL, "The script refers to property %u, which it doesn't have.", index) != NULL;
        if (op == 0x62) {
            push(s->b->items[index]);
        } else {
            a = pop();
            push(a);
            s->b->items[index] = a;
        }
        return true;
    }
    case 0x65: /* PushTrue */
        push(as_bool(true));
        return true;
    case 0x66: /* PushFalse */
        push(as_bool(false));
        return true;
    case 0x67: /* PushEmpty */
        push(as_list(0));
        return true;
    case 0x68: /* PushUndefined */
        push(as_undef());
        return true;
    case 0x69: /* PushMinus1 */
    case 0x6A: /* Push0 */
    case 0x6B: /* Push1 */
    case 0x6C: /* Push2 */
    case 0x6D: /* Push3 */
        push(as_int(op - 0x6A));
        return true;
    case 0x6E: /* BeginTimeout */
        pop();
        push_block(BLK_TIMEOUT);
        return true;
    case 0x6F: { /* EndTimeout */
        block *bl = innermost(BLK_TIMEOUT);
        if (bl)
            nblocks = (uint32_t)(bl - blocks);
        return true;
    }
    case 0x70: /* BeginTransaction */
        push_block(BLK_TRANSACTION);
        return true;
    case 0x71: { /* EndTransaction */
        block *bl = innermost(BLK_TRANSACTION);
        if (bl)
            nblocks = (uint32_t)(bl - blocks);
        return true;
    }
    }
    if (op >= 0xA0 && op <= 0xAF) { /* PushVariable */
        v = get_local(f, op & 0xF);
        if (!v)
            return false;
        push(v);
        return true;
    }
    if (op >= 0xB0 && op <= 0xBF) { /* PopVariable: stores, leaving the value */
        a = pop();
        push(a);
        return set_local(f, op & 0xF, a);
    }
    if (op >= 0xC0 && op <= 0xCF) { /* PushGlobal */
        a = lit(f, op & 0xF);
        if (!a || !(v = get_global(f, a)))
            return false;
        push(v);
        return true;
    }
    if (op >= 0xD0 && op <= 0xDF) { /* PopGlobal */
        a = lit(f, op & 0xF);
        if (!a)
            return false;
        b = pop();
        push(b);
        set_global(f, a, b);
        return true;
    }
    if (op >= 0xE0) { /* PushLiteral */
        a = lit(f, op & 0x1F);
        if (!a)
            return false;
        push(a->kind == V_LIST || a->kind == V_RECORD ? as_clone(a) : a);
        return true;
    }
    /* Script objects (DefineActor ...), filters (whose) and the rest that
     * no script we have uses yet. */
    LOGF(LOG_UNIMPL, "AppleScript: instruction %02X at %04X isn't supported\n", op, f->pc - 1);
    return as_raise(asErrUnsupported, NULL, "This script uses something our AppleScript can't do yet (instruction %02X).",
                    op) != NULL;
}

/* ---------------------------------------------------------------------- */
/* Calls from outside */

static as_obj *call(as_obj *script, as_obj *h, as_obj **args, uint32_t n)
{
    error_pending = error_abort = false;
    uint32_t base = nframes, saved_sp = sp, saved_blk = nblocks;
    if (!enter(script, h, args, n))
        return NULL;
    as_obj *r = run(base);
    sp = saved_sp;
    nblocks = saved_blk;
    return r;
}

as_obj *as_call_event(as_obj *script, uint32_t cls, uint32_t id, as_obj *direct, bool *found)
{
    as_obj *h = find_event_handler(script, cls, id);
    *found = h != NULL;
    if (!h)
        return NULL;
    as_obj *args[1] = { direct ? direct : as_undef() };
    return call(script, h, args, h->i == 2 ? 1 : 0);
}

as_obj *as_call_named(as_obj *script, const char *name, as_obj *args, bool *found)
{
    as_obj *h = find_handler(script, as_ident(name));
    *found = h != NULL;
    if (!h)
        return NULL;
    return call(script, h, args ? args->items : NULL, args ? args->n : 0);
}

uint32_t as_random(void)
{
    static uint64_t state;
    if (!state)
        state = (g_cpu ? g_cpu->icount : 0) ^ 0x9E3779B97F4A7C15ull ^ (uint64_t)rd32(LM_Ticks) << 17;
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return (uint32_t)(state >> 11);
}

/* ---------------------------------------------------------------------- */
/* IDs (OSAID) and the public calls */

typedef struct {
    as_obj *v;
    uint8_t *data; /* a loaded script's data, for OSAStore */
    size_t len;
} id_slot;

static id_slot *ids;
static uint32_t nids;

static uint32_t new_id(as_obj *v)
{
    if (!v)
        return 0;
    for (uint32_t i = 0; i < nids; i++)
        if (!ids[i].v) {
            ids[i].v = v;
            as_pin(v);
            return i + 1;
        }
    ids = realloc(ids, (nids + 1) * sizeof *ids);
    ids[nids] = (id_slot){ v, NULL, 0 };
    as_pin(v);
    return ++nids;
}

static as_obj *of_id(uint32_t id) { return id && id <= nids ? ids[id - 1].v : NULL; }

void as_reset(void)
{
    for (uint32_t i = 0; i < nids; i++)
        free(ids[i].data);
    free(ids);
    ids = NULL;
    nids = 0;
    while (nframes)
        leave();
    sp = nblocks = 0;
    npinned = 0;
    delimiters = NULL;
    caught_offending = caught_partial = NULL;
    as_clear_error();
    as_quit_requested = false;
    s_undef = s_null = s_missing = s_true = s_false = NULL;
    as_collect();
    instructions = 0;
}

int32_t as_load_script(const uint8_t *data, size_t len, uint32_t *id_out)
{
    char err[160];
    as_clear_error();
    as_obj *s = as_fasload(data, len, err, sizeof err);
    *id_out = 0;
    if (!s) {
        as_raise(asErrCorruptData, NULL, "The script's data is damaged (%s).", err);
        LOGF(LOG_UNIMPL, "AppleScript: can't load a script: %s\n", err);
        return asErrCorruptData;
    }
    *id_out = new_id(s);
    ids[*id_out - 1].data = malloc(len ? len : 1);
    memcpy(ids[*id_out - 1].data, data, len);
    ids[*id_out - 1].len = len;
    return 0;
}

const uint8_t *as_script_data(uint32_t id, size_t *len)
{
    if (!id || id > nids || !ids[id - 1].data)
        return NULL;
    *len = ids[id - 1].len;
    return ids[id - 1].data;
}

static int32_t finish_call(as_obj *r, uint32_t *result)
{
    if (result)
        *result = 0;
    if (!r) {
        if (!as_last_error.number)
            as_raise(asErrUnsupported, NULL, NULL);
        error_pending = false;
        return as_last_error.number;
    }
    if (result && r->kind != V_UNDEF)
        *result = new_id(r);
    return 0;
}

int32_t as_run_event(uint32_t id, uint32_t cls, uint32_t eid, uint32_t direct, uint32_t *result, int *found)
{
    as_obj *s = of_id(id);
    bool f = false;
    as_clear_error();
    if (result)
        *result = 0;
    if (!s || s->kind != V_SCRIPT) {
        if (found)
            *found = 0;
        return -1751; /* errOSAInvalidID */
    }
    as_obj *r = as_call_event(s, cls, eid, direct ? of_id(direct) : NULL, &f);
    if (found)
        *found = f;
    if (!f)
        return 0;
    return finish_call(r, result);
}

int32_t as_run_script(uint32_t id, uint32_t *result)
{
    int found;
    int32_t err = as_run_event(id, T("aevt"), T("oapp"), 0, result, &found);
    return err;
}

int32_t as_run_idle(uint32_t id, int *found)
{
    uint32_t r = 0;
    int32_t err = as_run_event(id, T("misc"), T("idle"), 0, &r, found);
    if (err)
        return err;
    double d;
    bool is_int;
    as_obj *v = of_id(r);
    int32_t secs = v && as_to_number(v, &d, &is_int) && d > 0 ? (int32_t)d : 30;
    if (r)
        as_dispose(r);
    return secs;
}

int32_t as_display(uint32_t id, char *out, size_t cap)
{
    as_obj *v = of_id(id);
    if (!v)
        return -1751;
    as_show(v, out, cap);
    return 0;
}

void as_dispose(uint32_t id)
{
    if (!id || id > nids || !ids[id - 1].v)
        return;
    as_unpin(ids[id - 1].v);
    ids[id - 1].v = NULL;
    free(ids[id - 1].data);
    ids[id - 1].data = NULL;
}

uint32_t as_copy_id(uint32_t id)
{
    as_obj *v = of_id(id);
    if (!v)
        return 0;
    uint32_t n = new_id(v);
    size_t len;
    const uint8_t *d = as_script_data(id, &len);
    if (d) {
        ids[n - 1].data = malloc(len);
        memcpy(ids[n - 1].data, d, len);
        ids[n - 1].len = len;
    }
    return n;
}

uint32_t as_new_text(const char *s) { return new_id(as_cstr(s)); }
uint32_t as_new_int(int32_t v) { return new_id(as_int(v)); }
uint32_t as_new_from_desc(uint32_t desc) { return new_id(as_from_desc(desc)); }
int32_t as_error_number(void) { return as_last_error.number; }
const char *as_error_message(void) { return as_last_error.message; }
uint64_t as_instructions(void) { return instructions; }

/* ---------------------------------------------------------------------- */
/* A script's properties and handlers by name (ASDebugging.h) */

/* The script an ID names, or NULL. */
static as_obj *script_of(uint32_t id)
{
    as_obj *s = of_id(id);
    return s && s->kind == V_SCRIPT ? s : NULL;
}

/* The nth named property (what 0) or named handler (what 1): its index in
 * the script, or -1. */
static int named_at(as_obj *s, int what, uint32_t nth)
{
    uint32_t seen = 0;
    for (uint32_t i = 0; s->a && i < s->a->n; i++) {
        as_obj *n = s->a->items[i];
        if (!n || n->kind != V_IDENT)
            continue;
        if ((s->b->items[i]->kind == V_HANDLER) != (what == 1))
            continue;
        if (seen++ == nth)
            return (int)i;
    }
    return -1;
}

bool as_is_script(uint32_t id) { return script_of(id) != NULL; }

uint32_t as_name_count(uint32_t id, int what)
{
    as_obj *s = script_of(id);
    uint32_t n = 0;
    for (int i = 0; s && s->a && i < (int)s->a->n; i++)
        if (s->a->items[i]->kind == V_IDENT && (s->b->items[i]->kind == V_HANDLER) == (what == 1))
            n++;
    return n;
}

const char *as_name_at(uint32_t id, int what, uint32_t nth, size_t *len)
{
    as_obj *s = script_of(id);
    int i = s ? named_at(s, what, nth) : -1;
    if (i < 0)
        return NULL;
    *len = s->a->items[i]->n;
    return (const char *)s->a->items[i]->bytes;
}

/* A property's (or global's) value as a new value ID; 0 if there is none. */
uint32_t as_property_value(uint32_t id, const char *name, size_t len)
{
    as_obj *s = script_of(id);
    if (!s)
        return 0;
    as_obj *key = as_ident("");
    set_bytes(key, name, len);
    as_obj *v = script_property(s, key);
    if (!v && s->c && s->c->kind == V_RECORD)
        v = as_record_get(s->c, key);
    return v ? new_id(v) : 0;
}

/* Set a property or global to a value: errOSAInvalidID for a script not
 * there, -1753 (errOSAScriptError) for a name not bound when it may not be
 * defined. */
int32_t as_set_property_value(uint32_t id, const char *name, size_t len, uint32_t value, bool define)
{
    as_obj *s = script_of(id), *v = of_id(value);
    if (!s || !v)
        return -1751;
    as_obj *key = as_ident("");
    set_bytes(key, name, len);
    for (uint32_t i = 0; s->a && i < s->a->n; i++)
        if (s->a->items[i]->kind == V_IDENT && key_eq(s->a->items[i], key) && s->b->items[i]->kind != V_HANDLER) {
            s->b->items[i] = v;
            return 0;
        }
    if (s->c && s->c->kind == V_RECORD && record_find(s->c, key) >= 0) {
        as_record_set(s->c, key, v);
        return 0;
    }
    if (!define)
        return -1753;
    if (!s->a || !s->c || s->c->kind != V_RECORD)
        return -1753;
    as_record_set(s->c, key, v); /* a new variable of the script */
    return 0;
}
