/*
 * Core Foundation as CarbonLib had it (carbon_cf*.c): what the files share.
 *
 * A CF object is a block in guest memory, from its allocator, that the
 * program only passes back: 'CFob', its type ID, its retain count and the
 * slot of its host-side state; some types keep more after that header
 * (CFData's bytes are elsewhere, from the same allocator).  The state a
 * type keeps on the host starts with a cf_obj.
 */
#ifndef CARBON_CF_H
#define CARBON_CF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(v); c->pc = c->lr; return; } while (0)
#define RETURN_VOID() do { c->pc = c->lr; return; } while (0)

/* Argument word n of a call: GPR slots, then the parameter area. */
static inline uint32_t cf_arg(const ppc_cpu *c, unsigned n) { return n < 8 ? c->gpr[3 + n] : rd32(c->gpr[1] + 24 + 4 * n); }
void cf_return_double(ppc_cpu *c, double d);
/* A result of 64 bits: r3 the high word, r4 the low. */
void cf_return64(ppc_cpu *c, uint64_t v);

enum { CF_HEADER = 16, CF_CONSTANT = 0x7FFFFFFF };
enum { kCFNotFound = -1, kCFCompareLessThan = -1, kCFCompareEqualTo = 0, kCFCompareGreaterThan = 1 };

/* Type IDs, as CFGetTypeID reports them. */
enum {
    CFT_TYPE = 1, CFT_ALLOCATOR, CFT_STRING, CFT_ARRAY, CFT_DICTIONARY, CFT_SET, CFT_BAG, CFT_DATA, CFT_NUMBER,
    CFT_BOOLEAN, CFT_DATE, CFT_URL, CFT_UUID, CFT_CHARSET, CFT_TREE, CFT_BUNDLE, CFT_PLUGIN_INSTANCE,
    CFT_XML_NODE, CFT_XML_PARSER, CFT_COUNT
};

/* A growing UTF-16 buffer (host). */
typedef struct {
    uint16_t *u;
    uint32_t n, cap;
} cf_text;
void text_add(cf_text *t, const uint16_t *u, uint32_t n);
void text_add_unit(cf_text *t, uint16_t u);
void text_add_utf8(cf_text *t, const char *s);
void text_printf(cf_text *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void text_free(cf_text *t);

typedef struct cf_obj cf_obj;
typedef struct {
    const char *name;
    /* What it holds released (callbacks may run); then its host memory. */
    void (*finalize)(cf_obj *o);
    void (*dispose)(cf_obj *o);
    bool (*equal)(cf_obj *a, cf_obj *b);
    uint32_t (*hash)(cf_obj *o);
    /* CFCopyDescription's text after "<CFName 0x... [0x...]>". */
    void (*describe)(cf_obj *o, cf_text *out);
} cf_class;

struct cf_obj {
    gaddr ref;
    uint32_t type;
    gaddr alloc; /* the allocator it came from (0: the system's) */
    uint32_t slot;
};

void cf_register_class(uint32_t type, const cf_class *cls);
/* A new object of a type: its host state (size bytes, zeroed, starting
 * with the cf_obj) and its guest block (the header and extra bytes, zeroed).
 * NULL when the allocator gives nothing. */
void *cf_new(uint32_t type, size_t size, gaddr alloc, uint32_t extra);
/* Never freed (CFSTR's strings, kCFBooleanTrue). */
void cf_make_constant(cf_obj *o);
cf_obj *cf_get(gaddr ref);
void *cf_get_type(gaddr ref, uint32_t type);
uint32_t cf_type_of(gaddr ref);
gaddr cf_retain(gaddr ref);
void cf_release(gaddr ref);
bool cf_equal(gaddr a, gaddr b);
uint32_t cf_hash(gaddr ref);
/* CFCopyDescription's text; %@'s (a string's own characters). */
void cf_describe(gaddr ref, cf_text *out);
void cf_format_describe(gaddr ref, cf_text *out);

/* Allocators: 0 is kCFAllocatorDefault, 1 kCFAllocatorUseContext. */
gaddr cf_allocator_resolve(gaddr alloc);
gaddr cf_allocate(gaddr alloc, uint32_t size);
void cf_deallocate(gaddr alloc, gaddr p);
gaddr cf_reallocate(gaddr alloc, gaddr p, uint32_t size, uint32_t old_size);
gaddr cf_allocator_null(void);
gaddr cf_allocator_system(void);

/* Guest code: a function pointer (a transition vector, or a routine
 * descriptor) called with these words; its r3. */
uint32_t cf_call(gaddr fn, const uint32_t *args, int n);
/* Our routines as transition vectors (for the kCF callback structures). */
gaddr cf_native(void (*fn)(ppc_cpu *), const char *name);

/* Strings (carbon_cfstring.c). */
enum { kCFStringEncodingMacRoman = 0, kCFStringEncodingUnicode = 0x0100, kCFStringEncodingISOLatin1 = 0x0201,
       kCFStringEncodingWindowsLatin1 = 0x0500, kCFStringEncodingASCII = 0x0600,
       kCFStringEncodingNonLossyASCII = 0x0BFF, kCFStringEncodingUTF8 = 0x08000100,
       kCFStringEncodingInvalidId = (int32_t)0xFFFFFFFF };
gaddr cfstr_new(gaddr alloc, const uint16_t *u, uint32_t n, bool mutable_);
gaddr cfstr_from_utf8(const char *s);
gaddr cfstr_from_text(const cf_text *t);
/* A string's characters (host, valid until it changes); NULL if not one. */
const uint16_t *cfstr_chars(gaddr ref, uint32_t *n);
/* A string as UTF-8, malloc'd; NULL if not one. */
char *cfstr_utf8(gaddr ref);
/* A literal never freed, one per text (the kCF keys). */
gaddr cfstr_constant(const char *ascii);
/* Bytes in an encoding to UTF-16 (malloc'd), false if they aren't. */
bool cf_decode(uint32_t enc, const uint8_t *b, size_t n, uint16_t **out, uint32_t *nout);
/* UTF-16 in an encoding, the loss byte for what it lacks (0: stop there);
 * malloc'd, false where it stopped. */
bool cf_encode(uint32_t enc, const uint16_t *u, uint32_t n, uint8_t loss, uint8_t **out, size_t *nout, uint32_t *used);
/* An encoding by its IANA name, or kCFStringEncodingInvalidId. */
uint32_t cf_encoding_of_iana(const char *name);
void cfstr_reset(void);

/* Collections (carbon_cfcoll.c), with the CFType callbacks. */
gaddr cfarray_new(gaddr alloc, const uint32_t *v, uint32_t n, bool mutable_);
uint32_t cfarray_count(gaddr a);
uint32_t cfarray_get(gaddr a, uint32_t i);
void cfarray_append(gaddr a, uint32_t v);
void cfarray_remove_all(gaddr a);
gaddr cfdict_new(gaddr alloc, bool mutable_);
void cfdict_set(gaddr d, uint32_t key, uint32_t value);
bool cfdict_get(gaddr d, uint32_t key, uint32_t *value);
void cfdict_remove(gaddr d, uint32_t key);
uint32_t cfdict_count(gaddr d);
/* The keys and values in order (malloc'd arrays the caller frees). */
uint32_t cfdict_entries(gaddr d, uint32_t **keys, uint32_t **values);
gaddr cfdata_new(gaddr alloc, const uint8_t *b, uint32_t n, bool mutable_);
/* A CFData's bytes in guest memory and its length. */
gaddr cfdata_bytes(gaddr d, uint32_t *n);
gaddr cfset_new(gaddr alloc, const uint32_t *v, uint32_t n, bool mutable_);
uint32_t cfset_entries(gaddr s, uint32_t **values);
gaddr cfbag_new(gaddr alloc, const uint32_t *v, uint32_t n, bool mutable_);
uint32_t cfbag_entries(gaddr s, uint32_t **values);
/* A tree whose context's info is ref (retained). */
gaddr cftree_new_with_info(gaddr alloc, gaddr info);
void cftree_append(gaddr tree, gaddr child);
uint32_t cftree_children(gaddr tree, gaddr **children);
gaddr cftree_info(gaddr tree);
gaddr cftree_parent(gaddr tree);
void cfcoll_reset(void);

/* Numbers, Booleans, dates, UUIDs (carbon_cfnum.c). */
enum { kCFNumberSInt8Type = 1, kCFNumberSInt16Type, kCFNumberSInt32Type, kCFNumberSInt64Type, kCFNumberFloat32Type,
       kCFNumberFloat64Type, kCFNumberCharType, kCFNumberShortType, kCFNumberIntType, kCFNumberLongType,
       kCFNumberLongLongType, kCFNumberFloatType, kCFNumberDoubleType, kCFNumberCFIndexType };
gaddr cfnum_new_int(gaddr alloc, int64_t v);
gaddr cfnum_new_double(gaddr alloc, double v);
/* A number's value: whether it is a float type, its value both ways. */
bool cfnum_value(gaddr n, bool *is_float, int64_t *i, double *d);
gaddr cfbool(bool v);
gaddr cfdate_new(gaddr alloc, double at);
bool cfdate_value(gaddr d, double *at);
double cf_absolute_now(void);
bool cfuuid_bytes(gaddr u, uint8_t out[16]);
/* A UUID's bytes from its text ("68753A44-4D6F-1226-9C60-0050E4C00067"). */
bool cfuuid_parse(gaddr str, uint8_t out[16]);
gaddr cfuuid_new(gaddr alloc, const uint8_t b[16], bool constant);
void cfnum_reset(void);

/* URLs, bundles, plug-ins (carbon_cfurl.c). */
enum { kCFURLPOSIXPathStyle = 0, kCFURLHFSPathStyle = 1, kCFURLWindowsPathStyle = 2 };
gaddr cfurl_new_string(gaddr alloc, gaddr string, gaddr base);
/* The URL of a host path (a file or a folder, from the volume's name). */
gaddr cfurl_of_host_path(gaddr alloc, const char *host);
/* The host path a file: URL names; false if it names none (it needn't exist). */
bool cfurl_host_path(gaddr url, char *out, size_t outsize);
bool cfurl_read_file(gaddr url, uint8_t **bytes, size_t *n);
/* A file's data fork by host path, read whole or written whole (malloc'd). */
bool cf_read_host_file(const char *path, uint8_t **bytes, size_t *n);
bool cf_write_host_file(const char *path, const uint8_t *bytes, size_t n);
gaddr cfurl_string(gaddr url);
/* The main bundle's CFBundleIdentifier, or 0. */
gaddr cfbundle_main_identifier(void);
void cfurl_reset(void);

/* Property lists, preferences, XML (carbon_cfplist.c). */
gaddr cfplist_from_data(gaddr alloc, const uint8_t *b, size_t n, uint32_t mutability, gaddr *error);
/* XML for a property list, malloc'd UTF-8; NULL if it isn't one. */
char *cfplist_to_xml(gaddr plist, size_t *n);
void cfplist_reset(void);

/* Each file's exports. */
extern const ppc_export cfbase_exports[], cfstr_exports[], cfcoll_exports[], cfnum_exports[], cfurl_exports[],
    cfplist_exports[];
extern const unsigned cfbase_export_count, cfstr_export_count, cfcoll_export_count, cfnum_export_count,
    cfurl_export_count, cfplist_export_count;

/* Whether a collection's callback is CF's own (run here, not as guest code). */
bool cf_is_type_retain(gaddr fn);
bool cf_is_type_release(gaddr fn);
bool cf_is_type_equal(gaddr fn);
bool cf_is_type_hash(gaddr fn);
bool cf_is_type_describe(gaddr fn);
bool cf_is_copy_string(gaddr fn);
/* CFRetain, CFRelease and CFCopyDescription as transition vectors (the
 * contexts of trees and parsers CF makes), and a context callback of one
 * argument called: those three here, the program's as guest code. */
gaddr cf_tv_retain(void);
gaddr cf_tv_release(void);
gaddr cf_tv_describe(void);
uint32_t cf_call_info(gaddr fn, uint32_t info);

/* Data exports: a cell holding a value, made once a launch. */
gaddr cf_data_cell(const char *name, uint32_t size);

#endif
