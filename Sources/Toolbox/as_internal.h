/*
 * AppleScript, our own runtime (M14): shared declarations between the
 * loader and interpreter (applescript.c), the scripting additions and
 * Apple Events a script sends (as_osax.c) and the Open Scripting
 * Architecture's components and the applet shell (osa.c).
 *
 * Nothing here runs on the host's AppleScript: compiled scripts are read
 * and run by our own code, and what they do goes no further than the
 * guest (its Toolbox, its file system, other Blue Box runs).
 */
#ifndef AS_INTERNAL_H
#define AS_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "applescript.h"

/* Values.  Every value is a heap object, collected by a mark and sweep
 * between bytecode instructions (never during one), so native code may
 * hold values in C variables while it works. */
typedef enum {
    V_UNDEF,   /* no value (a statement's result with none) */
    V_NULL,    /* the loader's nil */
    V_MISSING, /* missing value */
    V_BOOL,
    V_INT,
    V_REAL,
    V_TEXT,    /* MacRoman */
    V_LIST,
    V_RECORD,  /* keys: V_CONST (a property's code) or V_IDENT (a user label) */
    V_CONST,   /* a class, property or type (code < 2^32) or an enumerator (type << 32 | value) */
    V_IDENT,   /* a user identifier (a variable's or handler's name) */
    V_VECTOR,  /* the loader's value and pointer blocks (btype: the block type, -1 for a pointer block) */
    V_EVENT,   /* a command's event identifier */
    V_BYTES,   /* untyped data (handler bytecode) */
    V_DESC,    /* a descriptor kept as it is: type and data (an alias, a date's raw form ...) */
    V_APP,     /* an application: its name and signature */
    V_SCRIPT,  /* a script object: properties, handlers, globals */
    V_HANDLER,
    V_REF,     /* an object specifier */
    V_DATE,    /* seconds since 1904 */
} as_kind;

/* Object specifier forms, the MakeObjectAlias instructions in order. */
enum {
    REF_PROPERTY, REF_EVERY, REF_SOME, REF_INDEX, REF_KEY, REF_NAME, REF_RANGE, REF_RELATIVE, REF_FILTER,
    REF_BEGINNING, REF_END, REF_MIDDLE,
};

typedef struct as_obj as_obj;
struct as_obj {
    uint8_t kind;
    uint8_t mark;
    int16_t btype;     /* V_VECTOR: the block type; V_REF: the form */
    as_obj *gc_next;
    int32_t i;         /* V_BOOL, V_INT */
    double r;          /* V_REAL, V_DATE */
    uint64_t code;     /* V_CONST; V_DESC and V_APP: the type or signature */
    uint32_t n, cap;   /* V_TEXT, V_IDENT, V_BYTES, V_DESC: bytes; V_LIST, V_VECTOR, V_RECORD: items */
    uint8_t *bytes;
    as_obj **items;    /* V_LIST, V_VECTOR, V_RECORD (values) */
    as_obj **keys;     /* V_RECORD */
    as_obj *a, *b, *c, *d; /* V_REF: container, class, key, key 2; V_SCRIPT, V_HANDLER: see below */
    uint32_t ev[6];    /* V_EVENT: class, ID, result type, flags, direct type, its flags */
};

/* A script object (V_SCRIPT): a = its property names (a V_VECTOR of
 * V_IDENT, or V_EVENT for event handlers), b = their values (V_LIST),
 * c = globals (a V_RECORD), d = the text item delimiters' owner (the
 * script's AppleScript object state: a V_LIST of delimiters). */
/* A handler (V_HANDLER): a = name (V_IDENT or V_EVENT), b = parameters
 * (V_LIST of V_IDENT; NULL for an event handler without a direct
 * parameter), c = literals (V_VECTOR), d = bytecode (V_BYTES); n = the
 * number of local variables, items = their names (V_IDENT); i: 1 when the
 * parameters are positional, 2 for an event handler's direct parameter. */

as_obj *as_new(as_kind k);
as_obj *as_undef(void);
as_obj *as_null(void);
as_obj *as_missing(void);
as_obj *as_bool(bool b);
as_obj *as_int(int32_t v);
as_obj *as_real(double v);
as_obj *as_number(double v); /* an integer when it is one and fits */
as_obj *as_text(const void *s, size_t n);
as_obj *as_cstr(const char *s);
as_obj *as_ident(const char *s);
as_obj *as_const(uint64_t code);
as_obj *as_list(uint32_t n);
as_obj *as_record(void);
as_obj *as_desc(uint32_t type, const void *data, size_t n);
void as_list_push(as_obj *l, as_obj *v);
void as_list_insert(as_obj *l, uint32_t at, as_obj *v);
as_obj *as_record_get(as_obj *r, as_obj *key);
as_obj *as_record_get_code(as_obj *r, uint32_t code);
void as_record_set(as_obj *r, as_obj *key, as_obj *v);
as_obj *as_clone(as_obj *v);
void as_text_append(as_obj *t, const void *s, size_t n);

/* Errors: a script error (number, message, the offending object).  Natives
 * raise one and return NULL; the interpreter then unwinds to the nearest
 * try. */
as_obj *as_raise(int32_t num, as_obj *offending, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
bool as_error_pending(void);

/* Coercions and the language's operations (applescript.c). */
as_obj *as_deref(as_obj *v);                 /* an object specifier's value; NULL with an error raised */
as_obj *as_coerce(as_obj *v, uint32_t type); /* NULL with an error raised */
bool as_to_number(as_obj *v, double *out, bool *is_int);
bool as_truth(as_obj *v, bool *out);
as_obj *as_to_text(as_obj *v);               /* NULL with an error raised */
bool as_equal(as_obj *a, as_obj *b);
/* The value as AppleScript source shows it (OSADisplay, error messages). */
void as_show(as_obj *v, char *out, size_t cap);
const char *as_code_name(uint32_t code);     /* "button returned" ... or NULL */
/* The text item delimiters (the AppleScript object's property). */
as_obj *as_delimiters(void);
/* Random numbers (from the guest's clock, so headless runs repeat). */
uint32_t as_random(void);

/* Commands a script sends (as_osax.c): an event with its direct parameter
 * (NULL: none), its other parameters (a V_RECORD keyed by V_CONST) and its
 * target (the tell target).  Returns the result, or NULL with an error
 * raised (errAEEventNotHandled when it isn't one of ours). */
as_obj *as_command(as_obj *event, as_obj *direct, as_obj *params, as_obj *target);
/* A property of an application or an object in it (an object specifier
 * rooted at an application), got or set through an Apple Event to it. */
as_obj *as_app_get(as_obj *ref);
as_obj *as_app_set(as_obj *ref, as_obj *value);
bool as_app_rooted(as_obj *v);
/* A file's path (an alias or FSSpec value): "Disk:Folder:File", from the
 * guest's Alias Manager, else as the alias recorded it; NULL if neither. */
as_obj *as_file_path(as_obj *file);
/* A descriptor in guest memory as a value (lists and records too). */
as_obj *as_from_desc(uint32_t desc);
/* A dialog shown by the host (display dialog, the applet's startup
 * screen and errors); answered at once without one (as_osax.c). */
#include "machine.h"
int as_ask(machine_script_dialog *d);
/* The applet's own events while a script runs (quit). */
extern bool as_quit_requested;

/* Calls into a script (applescript.c): a handler of the script for the
 * event (class, ID), with an optional direct parameter; NULL when the
 * script has no such handler (*found false) or with an error raised. */
as_obj *as_call_event(as_obj *script, uint32_t cls, uint32_t id, as_obj *direct, bool *found);
as_obj *as_call_named(as_obj *script, const char *name, as_obj *args, bool *found);

/* The loader (applescript.c). */
as_obj *as_fasload(const uint8_t *data, size_t len, char *err, size_t errlen);

/* The garbage collector's roots beyond the interpreter's own: the OSA
 * component's script IDs (osa.c). */
void as_pin(as_obj *v);
void as_unpin(as_obj *v);
void as_collect(void);

/* The last error, for OSAScriptError. */
typedef struct {
    int32_t number;
    char message[512];
    as_obj *offending;
    as_obj *partial;
} as_error_info;
extern as_error_info as_last_error;
void as_clear_error(void);

#endif
