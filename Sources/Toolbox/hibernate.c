/*
 * Snapshots of the C targets' state (hibernate.h), and the relocation
 * that lets another process of the same build put one back.
 *
 * What is saved: the state sections (__bbdata, __bbbss) whole, every
 * tracked block (bbmem.c) with its type, and the regions (the guest's
 * address space: only the pages the host gave it, and not the zero ones).
 * The host's sections are not saved.
 *
 * Pointers.  The pointer map (statemap_gen.c, from the DWARF by
 * scripts/state-map.py) says which fields of each saved static and of each
 * tagged type are pointers.  Each such field that holds one is written as
 * zero in the snapshot, with a relocation beside it saying what it pointed
 * to, by what its value was:
 *   a tracked block      the block's number and the offset into it
 *   a static             the state section and the offset into it
 *   a region             the region and the offset
 *   the binary           by name: the symbol it is in (with its file, for
 *                        a static one) and the offset into it; a string
 *                        constant by its text and the offset into it.  So
 *                        another build puts a pointer to a function back
 *                        at that function wherever it is now.  A pointer
 *                        into the middle of a symbol whose size is not
 *                        what it was, or to a symbol that is gone, is
 *                        refused.  What has no symbol is the offset from
 *                        the binary's Mach-O header, for the same build
 *                        only
 *   a library            the library, the symbol dladdr names and the
 *                        offset from it (a library's place changes with
 *                        every boot)
 *   anything else        the host's memory (a library's object, Swift's):
 *                        kept as it was only when put back in the process
 *                        that saved it, else zero; counted as "host"
 * A field in a union with data is a pointer only if its value is one of
 * the first four; otherwise it is left as data.  The statics are found by
 * name, from the binary's own symbol table (its debug map, which names
 * each static's file), so the map does not depend on where the linker
 * put anything.  The snapshot holds no address of the process that wrote
 * it, so putting it back anywhere is the same as putting it back here.
 *
 * A restore makes every block again (wherever malloc puts it now: the old
 * blocks are still there while the new ones are made, so in the same
 * process they land elsewhere too), copies the sections and the regions
 * back, applies the relocations, frees the blocks the process had and
 * drops the JIT's translations.  A block's type is written by what it is
 * (its name, file and size), not where its descriptor is.
 *
 * Which builds put a snapshot back: the one that wrote it (its LC_UUID),
 * or any whose saved state has the same shape: the state fingerprint, a
 * hash of the manifest (manifest_make): each saved static's file, name,
 * place in its section, size, pointer layout and whole type (the map's
 * shape), each tagged type's, the sections' sizes, the format, and the
 * guest-side layout the host lays the guest's memory out by (bbmem.h's
 * layout records).  The code may differ: a commit that changes a
 * function's body but no saved type keeps every snapshot.  A snapshot
 * file carries its build's manifest in its head, so a refusal names the
 * first thing that differs without inflating the rest.
 */
#include "hibernate.h"
#include "hibernate_map.h"
#include "lowmem_gen.h"

#include "mem.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/stab.h>
#endif

#define MAGIC "BBHIBER2"
#define VERSION 2

/* R_SYM: a symbol of the binary (the snapshot's symbol table) and the
 * offset into it; R_CSTR: a string constant (its text in the pool) and
 * the offset into it; R_IMAGE: the offset from the binary's header (no
 * symbol: the same build only). */
enum { R_BLOCK = 1, R_STATE, R_REGION, R_IMAGE, R_EXTERN, R_HOST, R_SYM, R_CSTR };

typedef struct {
    char magic[8];
    uint32_t version, flags;
    uint8_t uuid[16];
    uint8_t fingerprint[16];
    uint64_t session;
    uint64_t nsections, nblocks, nregions, nrelocs, nexterns, nsyms, ncstrs, ntypes;
    uint64_t sections_off, blocks_off, regions_off, relocs_off, externs_off, syms_off, cstrs_off, types_off;
    uint64_t pool_off, pool_len, total;
} file_header;

typedef struct {
    char name[16];
    uint64_t size;
} file_section;

typedef struct {
    uint32_t type; /* in the snapshot's table of types */
    uint32_t size;
} file_block;

/* A symbol of the binary a pointer is in: its file (a static one's, else
 * empty) and name, in the pool; its size, and whether a pointer goes past
 * its start (then it must be as large where it is put back). */
enum { SYM_INTERIOR = 1 << 0, SYM_CODE = 1 << 1 };
typedef struct {
    uint32_t file, name, flags, pad;
    uint64_t size;
} file_sym;

/* A string constant: its text in the pool. */
typedef struct {
    uint32_t text, len;
} file_cstr;

/* A block's type, as its descriptor says (bbmem.h): name and file in the
 * pool. */
typedef struct {
    uint32_t name, file, size, form, line, pad;
} file_type;

typedef struct {
    char name[16];
    uint64_t size, nruns;
} file_region;

typedef struct {
    uint64_t off, len;
} file_run;

/* where: a section (< nsections) or a block (nsections + its number);
 * what: the kind in the top byte, its section, block, region or library
 * below; value: the offset into it, or the host's pointer. */
typedef struct {
    uint32_t where, what;
    uint64_t off, value;
} file_reloc;

/* ---- The binary ---- */

#define NSAVED 2
#define NHOSTSECT 2
static const char *const saved_names[NSAVED] = { "__bbdata", "__bbbss" };
static const char *const host_names[NHOSTSECT] = { "__bbhost", "__bbhostz" };

typedef struct {
    uint8_t *base; /* the Mach-O header */
    uintptr_t lo, hi; /* the whole image */
    intptr_t slide;
    uint8_t uuid[16];
    uint8_t *sect[NSAVED];
    size_t sect_size[NSAVED];
    uint8_t *host[NHOSTSECT];
    size_t host_size[NHOSTSECT];
    /* Every section, by its number in the symbol table (from 1). */
    struct {
        uintptr_t lo, hi;
        bool code, cstrings;
    } sects[256];
    uint32_t nsects;
    const bb_layout_rec *layout;
    size_t nlayout;
#if defined(__APPLE__)
    const struct nlist_64 *syms;
    uint32_t nsyms;
    const char *strs;
#endif
} image_info;

/* A saved static the map places: its address, size and layout. */
typedef struct {
    uintptr_t addr;
    uint32_t size, layout;
    const hb_static *map;
} placed;

BB_HOST static image_info img;
BB_HOST static bool img_ok;
BB_HOST static placed *statics;
BB_HOST static size_t nstatics;
BB_HOST static uint64_t session;

static int cmp_placed(const void *a, const void *b)
{
    uintptr_t x = ((const placed *)a)->addr, y = ((const placed *)b)->addr;
    return x < y ? -1 : x > y;
}

static int cmp_static_key(const char *file, const char *sym, const hb_static *s)
{
    int c = strcmp(file, s->file);
    return c ? c : strcmp(sym, s->sym);
}

static const hb_static *map_static(const char *file, const char *sym)
{
    size_t lo = 0, hi = hb_nstatics;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = cmp_static_key(file, sym, &hb_statics[mid]);
        if (c == 0)
            return &hb_statics[mid];
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

static const hb_typemap *map_type(const char *file, const char *name)
{
    size_t lo = 0, hi = hb_ntypes;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(file, hb_types[mid].file);
        if (!c)
            c = strcmp(name, hb_types[mid].name);
        if (c == 0)
            return &hb_types[mid];
        if (c < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

static bool in_saved(uintptr_t a, int *which)
{
    for (int i = 0; i < NSAVED; i++)
        if (img.sect[i] && a >= (uintptr_t)img.sect[i] && a < (uintptr_t)img.sect[i] + img.sect_size[i]) {
            if (which)
                *which = i;
            return true;
        }
    return false;
}

#if defined(__APPLE__)

static bool image_load(char *err, size_t errlen)
{
    Dl_info info;
    if (!dladdr((const void *)hb_statics, &info) || !info.dli_fbase) {
        snprintf(err, errlen, "the binary holding the Toolbox is not found");
        return false;
    }
    const struct mach_header_64 *mh = info.dli_fbase;
    img.base = (uint8_t *)info.dli_fbase;
    const struct load_command *lc = (const void *)(mh + 1);
    const struct segment_command_64 *text = NULL, *linkedit = NULL;
    const struct symtab_command *symtab = NULL;
    bool have_uuid = false;
    for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (const void *)lc;
            if (!strcmp(seg->segname, SEG_TEXT))
                text = seg;
            else if (!strcmp(seg->segname, SEG_LINKEDIT))
                linkedit = seg;
        } else if (lc->cmd == LC_UUID) {
            memcpy(img.uuid, ((const struct uuid_command *)lc)->uuid, 16);
            have_uuid = true;
        } else if (lc->cmd == LC_SYMTAB) {
            symtab = (const void *)lc;
        }
    }
    if (!text || !have_uuid) {
        snprintf(err, errlen, "the binary has no %s", text ? "LC_UUID" : "__TEXT");
        return false;
    }
    img.slide = (intptr_t)((uintptr_t)mh - text->vmaddr);
    img.lo = UINTPTR_MAX;
    img.hi = 0;
    lc = (const void *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT_64)
            continue;
        const struct segment_command_64 *seg = (const void *)lc;
        if (!strcmp(seg->segname, SEG_PAGEZERO) || seg->vmsize == 0)
            continue;
        uintptr_t s = (uintptr_t)(seg->vmaddr + img.slide), e = s + seg->vmsize;
        if (s < img.lo)
            img.lo = s;
        if (e > img.hi)
            img.hi = e;
        const struct section_64 *sec = (const void *)(seg + 1);
        for (uint32_t k = 0; k < seg->nsects; k++, sec++) {
            if (img.nsects + 1 < sizeof img.sects / sizeof img.sects[0]) {
                uint32_t n = ++img.nsects;
                img.sects[n].lo = (uintptr_t)(sec->addr + img.slide);
                img.sects[n].hi = img.sects[n].lo + sec->size;
                img.sects[n].code = sec->flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS);
                img.sects[n].cstrings = (sec->flags & SECTION_TYPE) == S_CSTRING_LITERALS;
            }
            if (!strncmp(sec->segname, "__DATA_CONST", 16) && !strncmp(sec->sectname, "__bblayout", 16)) {
                img.layout = (const bb_layout_rec *)(sec->addr + img.slide);
                img.nlayout = sec->size / sizeof(bb_layout_rec);
            }
            if (strncmp(sec->segname, "__DATA", 16))
                continue;
            for (int n = 0; n < NSAVED; n++)
                if (!strncmp(sec->sectname, saved_names[n], 16)) {
                    img.sect[n] = (uint8_t *)(sec->addr + img.slide);
                    img.sect_size[n] = sec->size;
                }
            for (int n = 0; n < NHOSTSECT; n++)
                if (!strncmp(sec->sectname, host_names[n], 16)) {
                    img.host[n] = (uint8_t *)(sec->addr + img.slide);
                    img.host_size[n] = sec->size;
                }
        }
    }
    if (symtab && linkedit) {
        const uint8_t *le = (const uint8_t *)(linkedit->vmaddr + img.slide - linkedit->fileoff);
        img.syms = (const void *)(le + symtab->symoff);
        img.nsyms = symtab->nsyms;
        img.strs = (const char *)(le + symtab->stroff);
    }
    return true;
}

/* Each data symbol of the debug map: its file (as Target/path, from the
 * N_SO before it), its name without the underscore, and its address. */
typedef void (*sym_fn)(void *ctx, const char *file, const char *name, uintptr_t addr);

static uint64_t hash_str(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    while (*s)
        h = (h ^ (uint8_t)*s++) * 1099511628211ull;
    return h;
}

static bool each_debug_symbol(sym_fn fn, void *ctx, char *err, size_t errlen)
{
    if (!img.syms) {
        snprintf(err, errlen, "the binary has no symbol table");
        return false;
    }
    /* A global's stab has no address: the symbol table's own entry does. */
    size_t cap = 1;
    while (cap < (size_t)img.nsyms / 4 + 16)
        cap <<= 1;
    const struct nlist_64 **ext = bb_host_calloc(cap, sizeof *ext);
    if (!ext) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    for (uint32_t i = 0; i < img.nsyms; i++) {
        const struct nlist_64 *n = &img.syms[i];
        if ((n->n_type & N_STAB) || (n->n_type & N_TYPE) != N_SECT || !(n->n_type & N_EXT))
            continue;
        for (size_t h = hash_str(img.strs + n->n_un.n_strx) & (cap - 1);; h = (h + 1) & (cap - 1))
            if (!ext[h]) {
                ext[h] = n;
                break;
            }
    }
    char dir[1024] = "", path[2048] = "";
    const char *file = NULL;
    bool stabs = false;
    for (uint32_t i = 0; i < img.nsyms; i++) {
        const struct nlist_64 *n = &img.syms[i];
        if (!(n->n_type & N_STAB))
            continue;
        const char *s = img.strs + n->n_un.n_strx;
        switch (n->n_type) {
        case N_SO:
            if (!*s) {
                dir[0] = 0;
                file = NULL;
            } else if (s[strlen(s) - 1] == '/') {
                snprintf(dir, sizeof dir, "%s", s);
            } else {
                snprintf(path, sizeof path, "%s%s", s[0] == '/' ? "" : dir, s);
                const char *t = strstr(path, "/Sources/");
                file = t ? t + 9 : NULL;
            }
            break;
        case N_STSYM:
        case N_GSYM: {
            stabs = true;
            if (!file || *s != '_')
                break;
            uintptr_t a = (uintptr_t)(n->n_value + img.slide);
            if (n->n_type == N_GSYM) {
                a = 0;
                for (size_t h = hash_str(s) & (cap - 1); ext[h]; h = (h + 1) & (cap - 1))
                    if (!strcmp(img.strs + ext[h]->n_un.n_strx, s)) {
                        a = (uintptr_t)(ext[h]->n_value + img.slide);
                        break;
                    }
                if (!a)
                    break;
            }
            fn(ctx, file, s + 1, a);
            break;
        }
        }
    }
    bb_host_free(ext);
    if (!stabs) {
        snprintf(err, errlen, "the binary has no debug map (stripped?): its statics cannot be found");
        return false;
    }
    return true;
}

static const struct mach_header *lib_named(const char *path)
{
    for (uint32_t i = 0; i < _dyld_image_count(); i++)
        if (!strcmp(_dyld_get_image_name(i), path))
            return _dyld_get_image_header(i);
    return NULL;
}

/* ---- The binary's symbols, for pointers into it by name ----
 *
 * Every symbol of the binary's own (not a stab, in a section, a C or
 * Swift name), with its size: a function's from the debug map, anything
 * else's as far as the next symbol in its section.  A static symbol is
 * named with its file (two files may each have a static of a name), as
 * the debug map has it; a global by its name alone. */

typedef struct {
    uintptr_t addr;
    uint64_t size;
    const char *name; /* without the underscore */
    const char *file; /* a static one's, as Target/path; else "" */
    uint32_t sect;
    bool code;
} isym;

BB_HOST static isym *isyms;
BB_HOST static size_t nisyms;
BB_HOST static uint32_t *isym_slots; /* by file and name: index + 1 */
BB_HOST static size_t isym_cap;

typedef struct {
    uintptr_t addr;
    const char *file;
    uint64_t size;
} stab_info;

static int cmp_stab(const void *a, const void *b)
{
    uintptr_t x = ((const stab_info *)a)->addr, y = ((const stab_info *)b)->addr;
    return x < y ? -1 : x > y;
}

static int cmp_isym(const void *a, const void *b)
{
    const isym *x = a, *y = b;
    if (x->addr != y->addr)
        return x->addr < y->addr ? -1 : 1;
    int c = strcmp(x->name, y->name);
    return c ? c : strcmp(x->file, y->file);
}

static uint64_t hash_key(const char *file, const char *name)
{
    return hash_str(file) * 31 ^ hash_str(name);
}

static bool symbols_ready(char *err, size_t errlen)
{
    if (isyms)
        return true;
    if (!img.syms) {
        snprintf(err, errlen, "the binary has no symbol table");
        return false;
    }
    /* The debug map: each function's and static's file, each function's size. */
    stab_info *st = bb_host_malloc(((size_t)img.nsyms + 1) * sizeof *st);
    isym *list = bb_host_malloc(((size_t)img.nsyms + 1) * sizeof *list);
    if (!st || !list) {
        bb_host_free(st);
        bb_host_free(list);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    size_t nst = 0, n = 0;
    char dir[1024] = "";
    const char *file = "";
    stab_info *fun = NULL;
    for (uint32_t i = 0; i < img.nsyms; i++) {
        const struct nlist_64 *e = &img.syms[i];
        if (!(e->n_type & N_STAB))
            continue;
        const char *s = img.strs + e->n_un.n_strx;
        if (e->n_type == N_SO) {
            if (!*s) {
                dir[0] = 0;
                file = "";
            } else if (s[strlen(s) - 1] == '/') {
                snprintf(dir, sizeof dir, "%s", s);
            } else {
                char path[2048];
                snprintf(path, sizeof path, "%s%s", s[0] == '/' ? "" : dir, s);
                const char *t = strstr(path, "/Sources/");
                t = t ? t + 9 : path;
                char *copy = bb_host_malloc(strlen(t) + 1); /* kept for good, as the index is */
                file = copy ? strcpy(copy, t) : "";
            }
        } else if (e->n_type == N_FUN) {
            if (*s) {
                st[nst] = (stab_info){ (uintptr_t)(e->n_value + img.slide), file, 0 };
                fun = &st[nst++];
            } else if (fun) {
                fun->size = e->n_value;
                fun = NULL;
            }
        } else if (e->n_type == N_STSYM) {
            st[nst++] = (stab_info){ (uintptr_t)(e->n_value + img.slide), file, 0 };
        }
    }
    qsort(st, nst, sizeof *st, cmp_stab);
    for (uint32_t i = 0; i < img.nsyms; i++) {
        const struct nlist_64 *e = &img.syms[i];
        const char *s = img.strs + e->n_un.n_strx;
        if ((e->n_type & N_STAB) || (e->n_type & N_TYPE) != N_SECT || s[0] != '_' || !e->n_sect ||
            e->n_sect > img.nsects)
            continue;
        uintptr_t a = (uintptr_t)(e->n_value + img.slide);
        size_t lo = 0, hi = nst;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (st[mid].addr < a)
                lo = mid + 1;
            else
                hi = mid;
        }
        const stab_info *at = lo < nst && st[lo].addr == a ? &st[lo] : NULL;
        list[n++] = (isym){ a, at ? at->size : 0, s + 1, !(e->n_type & N_EXT) && at ? at->file : "", e->n_sect,
                            img.sects[e->n_sect].code };
    }
    bb_host_free(st);
    qsort(list, n, sizeof *list, cmp_isym);
    for (size_t i = 0; i < n; i++) {
        if (list[i].size)
            continue;
        uintptr_t end = img.sects[list[i].sect].hi;
        for (size_t j = i + 1; j < n; j++)
            if (list[j].addr > list[i].addr) {
                if (list[j].addr < end)
                    end = list[j].addr;
                break;
            }
        list[i].size = end > list[i].addr ? end - list[i].addr : 0;
    }
    size_t cap = 1;
    while (cap < n * 2 + 16)
        cap <<= 1;
    uint32_t *slots = bb_host_calloc(cap, sizeof *slots);
    if (!slots) {
        bb_host_free(list);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    for (size_t i = 0; i < n; i++)
        for (size_t h = hash_key(list[i].file, list[i].name) & (cap - 1);; h = (h + 1) & (cap - 1)) {
            if (!slots[h]) {
                slots[h] = (uint32_t)i + 1;
                break;
            }
            const isym *o = &list[slots[h] - 1];
            if (!strcmp(o->name, list[i].name) && !strcmp(o->file, list[i].file))
                break; /* an alias at another address: the first stands */
        }
    isyms = list;
    nisyms = n;
    isym_slots = slots;
    isym_cap = cap;
    return true;
}

#else

static bool image_load(char *err, size_t errlen)
{
    snprintf(err, errlen, "snapshots need the Mach-O reader; this host has none yet");
    return false;
}

typedef void (*sym_fn)(void *ctx, const char *file, const char *name, uintptr_t addr);
static bool each_debug_symbol(sym_fn fn, void *ctx, char *err, size_t errlen)
{
    snprintf(err, errlen, "no symbol reader on this host");
    return false;
}

static const void *lib_named(const char *path)
{
    return NULL;
}

typedef struct {
    uintptr_t addr;
    uint64_t size;
    const char *name, *file;
    uint32_t sect;
    bool code;
} isym;

BB_HOST static isym *isyms;
BB_HOST static size_t nisyms;
BB_HOST static uint32_t *isym_slots;
BB_HOST static size_t isym_cap;

static uint64_t hash_key(const char *file, const char *name)
{
    return 0;
}

static bool symbols_ready(char *err, size_t errlen)
{
    snprintf(err, errlen, "no symbol reader on this host");
    return false;
}

#endif

/* The symbol v is in (a pointer one past an array's end is in the next
 * symbol: such a pointer is the same in another build only if that is). */
static const isym *isym_at(uintptr_t v)
{
    size_t lo = 0, hi = nisyms;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (isyms[mid].addr <= v)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (!lo)
        return NULL;
    const isym *s = &isyms[lo - 1];
    return v - s->addr < (s->size ? s->size : 1) ? s : NULL;
}

static const isym *isym_named(const char *file, const char *name)
{
    if (!isym_cap)
        return NULL;
    for (size_t h = hash_key(file, name) & (isym_cap - 1); isym_slots[h]; h = (h + 1) & (isym_cap - 1)) {
        const isym *s = &isyms[isym_slots[h] - 1];
        if (!strcmp(s->name, name) && !strcmp(s->file, file))
            return s;
    }
    return NULL;
}

/* The section of string constants v is in (its number), or 0. */
static uint32_t cstring_section(uintptr_t v)
{
    for (uint32_t i = 1; i <= img.nsects; i++)
        if (img.sects[i].cstrings && v >= img.sects[i].lo && v < img.sects[i].hi)
            return i;
    return 0;
}

static uint64_t hash_text(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ (uint8_t)s[i]) * 1099511628211ull;
    return h;
}

/* The binary's string constants by their text, for putting a pointer to
 * one back (made when first wanted). */
BB_HOST static const char **cstr_slots;
BB_HOST static size_t cstr_cap;

static bool cstrings_ready(void)
{
    if (cstr_slots)
        return true;
    size_t count = 0;
    for (uint32_t i = 1; i <= img.nsects; i++)
        if (img.sects[i].cstrings)
            for (const char *p = (const char *)img.sects[i].lo; p < (const char *)img.sects[i].hi; p++)
                count += !*p;
    size_t cap = 1;
    while (cap < count * 2 + 16)
        cap <<= 1;
    const char **slots = bb_host_calloc(cap, sizeof *slots);
    if (!slots)
        return false;
    for (uint32_t i = 1; i <= img.nsects; i++) {
        if (!img.sects[i].cstrings)
            continue;
        const char *p = (const char *)img.sects[i].lo, *end = (const char *)img.sects[i].hi;
        while (p < end) {
            size_t n = strnlen(p, (size_t)(end - p));
            if (n < (size_t)(end - p))
                for (size_t h = hash_text(p, n) & (cap - 1);; h = (h + 1) & (cap - 1)) {
                    if (!slots[h]) {
                        slots[h] = p;
                        break;
                    }
                    if (!strcmp(slots[h], p))
                        break;
                }
            p += n + 1;
        }
    }
    cstr_slots = slots;
    cstr_cap = cap;
    return true;
}

static const char *cstring_named(const char *text, size_t n)
{
    if (!cstr_cap)
        return NULL;
    for (size_t h = hash_text(text, n) & (cstr_cap - 1); cstr_slots[h]; h = (h + 1) & (cstr_cap - 1))
        if (!strncmp(cstr_slots[h], text, n) && !cstr_slots[h][n])
            return cstr_slots[h];
    return NULL;
}

/* A block's type in this binary, by what the snapshot says it is: a
 * descriptor of the same type written at the same line, else anywhere
 * (each BB_TAG is a descriptor of its own), else for bytes or an untyped
 * block the generic one. */
BB_HOST static const bb_type **descs;
BB_HOST static size_t ndescs;

static bool descriptors_ready(void)
{
    if (descs)
        return true;
    descs = bb_host_malloc((nisyms + 2) * sizeof *descs);
    if (!descs)
        return false;
    for (size_t i = 0; i < nisyms; i++)
        if (!isyms[i].code && strstr(isyms[i].name, ".bb_tag_") && isyms[i].size >= sizeof(bb_type))
            descs[ndescs++] = (const bb_type *)isyms[i].addr;
    descs[ndescs++] = &bb_type_bytes;
    descs[ndescs++] = &bb_type_untyped;
    return true;
}

static const bb_type *descriptor_for(const char *name, const char *file, uint32_t size, uint32_t form, uint32_t line)
{
    const bb_type *any = NULL;
    for (size_t i = 0; i < ndescs; i++) {
        const bb_type *t = descs[i];
        if (t->form != form || t->size != size || strcmp(t->name, name) || strcmp(t->file, file))
            continue;
        if (t->line == line)
            return t;
        if (!any)
            any = t;
    }
    if (any)
        return any;
    if (form == BB_FORM_BYTES)
        return &bb_type_bytes;
    if (form == BB_FORM_UNTYPED)
        return &bb_type_untyped;
    return NULL;
}

typedef struct {
    placed *list;
    size_t n, cap;
} place_ctx;

static void place_one(void *vctx, const char *file, const char *name, uintptr_t addr)
{
    place_ctx *c = vctx;
    if (!in_saved(addr, NULL))
        return;
    const hb_static *m = map_static(file, name);
    if (!m)
        return;
    if (c->n == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 1024;
        placed *l = bb_host_realloc(c->list, cap * sizeof *l);
        if (!l)
            return;
        c->list = l;
        c->cap = cap;
    }
    c->list[c->n++] = (placed){ addr, m->size, m->layout, m };
}

/* ---- The state fingerprint ----
 *
 * The manifest: a line for each thing a snapshot's layout depends on,
 * tab separated, the kind first:
 *   V  the format's version, the machine, a pointer's size
 *   R  a state section and its size
 *   S  a saved static: file, name, section, offset, size, a hash of its
 *      pointers' layout, its shape (the whole type, from the map)
 *   T  a tagged type: file, the type as written, size, layout, shape
 *   L  a record of the guest-side layout (bbmem.h): name and value
 * The fingerprint is a hash of it; two builds with the same manifest put
 * back each other's snapshots. */

BB_HOST static char *manifest;
BB_HOST static size_t manifest_len, manifest_cap;
BB_HOST static uint8_t fingerprint[16];

static void manifest_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void manifest_add(const char *fmt, ...)
{
    if (manifest_len == SIZE_MAX)
        return;
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        size_t room = manifest_cap - manifest_len;
        int n = vsnprintf(manifest ? manifest + manifest_len : NULL, room, fmt, ap);
        va_end(ap);
        if (n < 0) {
            manifest_len = SIZE_MAX;
            return;
        }
        if ((size_t)n < room) {
            manifest_len += (size_t)n;
            return;
        }
        size_t cap = manifest_cap ? manifest_cap * 2 : 1 << 18;
        while (cap - manifest_len <= (size_t)n)
            cap *= 2;
        char *m = bb_host_realloc(manifest, cap);
        if (!m) {
            manifest_len = SIZE_MAX;
            return;
        }
        manifest = m;
        manifest_cap = cap;
    }
}

static uint64_t runs_hash(uint32_t layout)
{
    uint64_t h = bb_layout_hash(0, "runs", 4);
    if (layout >= hb_nlayouts)
        return h;
    const hb_layout *l = &hb_layouts[layout];
    for (uint32_t r = 0; r < l->n; r++) {
        const hb_run *run = &hb_runs[l->first + r];
        uint32_t v[6] = { run->off, (uint32_t)(uint8_t)run->kind, run->count, run->stride, run->count2, run->stride2 };
        h = bb_layout_hash(h, v, sizeof v);
    }
    return h;
}

static int cmp_layout_rec(const void *a, const void *b)
{
    const bb_layout_rec *x = *(const bb_layout_rec *const *)a, *y = *(const bb_layout_rec *const *)b;
    return strcmp(x->name, y->name);
}

static bool manifest_make(void)
{
    manifest_len = 0;
#if defined(__aarch64__)
    const char *arch = "arm64";
#elif defined(__x86_64__)
    const char *arch = "x86_64";
#else
    const char *arch = "?";
#endif
    manifest_add("V\t%d\t%s\t%zu\n", VERSION, arch, sizeof(void *));
    for (int i = 0; i < NSAVED; i++)
        manifest_add("R\t%s\t%zu\n", saved_names[i], img.sect_size[i]);
    for (size_t i = 0; i < nstatics; i++) {
        int w = 0;
        in_saved(statics[i].addr, &w);
        manifest_add("S\t%s\t%s\t%d\t%llu\t%u\t%016llx\t%016llx\n", statics[i].map->file, statics[i].map->sym, w,
                     (unsigned long long)(statics[i].addr - (uintptr_t)img.sect[w]), statics[i].size,
                     (unsigned long long)runs_hash(statics[i].layout), (unsigned long long)statics[i].map->shape);
    }
    for (unsigned i = 0; i < hb_ntypes; i++)
        manifest_add("T\t%s\t%s\t%u\t%016llx\t%016llx\n", hb_types[i].file, hb_types[i].name, hb_types[i].size,
                     (unsigned long long)runs_hash(hb_types[i].layout), (unsigned long long)hb_types[i].shape);
    const bb_layout_rec **recs = bb_host_malloc((img.nlayout + 1) * sizeof *recs);
    if (!recs)
        return false;
    for (size_t i = 0; i < img.nlayout; i++)
        recs[i] = &img.layout[i];
    qsort(recs, img.nlayout, sizeof *recs, cmp_layout_rec);
    for (size_t i = 0; i < img.nlayout; i++)
        manifest_add("L\t%s\t%016llx\n", recs[i]->name,
                     (unsigned long long)(recs[i]->hash ? recs[i]->hash() : recs[i]->value));
    bb_host_free(recs);
    if (manifest_len == SIZE_MAX)
        return false;
    uint64_t a = bb_layout_hash(0, manifest, manifest_len);
    uint64_t b = bb_layout_hash(0x84222325cbf29ce4ull, manifest, manifest_len);
    memcpy(fingerprint, &a, 8);
    memcpy(fingerprint + 8, &b, 8);
    return true;
}

/* The fields of a manifest line (up to 8), split in place of a copy. */
static int manifest_fields(const char *line, size_t n, char *copy, size_t room, const char **f)
{
    if (n >= room)
        n = room - 1;
    memcpy(copy, line, n);
    copy[n] = 0;
    int k = 0;
    for (char *p = copy; k < 8;) {
        f[k++] = p;
        char *t = strchr(p, '\t');
        if (!t)
            break;
        *t = 0;
        p = t + 1;
    }
    return k;
}

/* The fields that name a line's thing (the kind and its name). */
static int manifest_key_fields(char kind)
{
    return kind == 'S' || kind == 'T' ? 3 : kind == 'V' ? 1 : 2;
}

static const char *manifest_find(const char *m, size_t len, const char *const *key, int nkey, size_t *linelen)
{
    for (const char *p = m; p < m + len;) {
        const char *e = memchr(p, '\n', (size_t)(m + len - p));
        size_t n = e ? (size_t)(e - p) : (size_t)(m + len - p);
        const char *q = p;
        int k = 0;
        for (; k < nkey; k++) {
            size_t kl = strlen(key[k]);
            if ((size_t)(p + n - q) < kl || memcmp(q, key[k], kl) || (q + kl < p + n && q[kl] != '\t') ||
                (q + kl == p + n && k + 1 < nkey))
                break;
            q += kl + 1;
        }
        if (k == nkey) {
            *linelen = n;
            return p;
        }
        p += n + 1;
    }
    return NULL;
}

/* Why a snapshot whose manifest is old cannot be put back here: the first
 * thing that differs, and how many do. */
static void manifest_explain(const char *old, size_t oldlen, char *why, size_t whylen)
{
    int count = 0;
    char first[512] = "";
    char a[1024], b[1024];
    const char *fa[8], *fb[8];
    for (int pass = 0; pass < 2; pass++) {
        const char *m = pass ? manifest : old, *other = pass ? old : manifest;
        size_t len = pass ? manifest_len : oldlen, olen = pass ? oldlen : manifest_len;
        for (const char *p = m; p < m + len;) {
            const char *e = memchr(p, '\n', (size_t)(m + len - p));
            size_t n = e ? (size_t)(e - p) : (size_t)(m + len - p);
            int na = manifest_fields(p, n, a, sizeof a, fa);
            p += n + 1;
            if (na < 2)
                continue;
            int nkey = manifest_key_fields(fa[0][0]);
            if (na < nkey)
                continue;
            size_t ol;
            const char *o = manifest_find(other, olen, fa, nkey, &ol);
            if (o && pass)
                continue; /* both have it: the first pass told */
            if (o && ol == n && !memcmp(o, p - n - 1, n))
                continue;
            count++;
            if (*first)
                continue;
            const char *what = fa[0][0] == 'S' ? "the static" : fa[0][0] == 'T' ? "the tagged type" : "";
            if (!o) {
                if (fa[0][0] == 'S' || fa[0][0] == 'T')
                    snprintf(first, sizeof first, "%s %s (%s) is %s", what, fa[2], fa[1],
                             pass ? "new in this build" : "not in this build");
                else if (fa[0][0] == 'L')
                    snprintf(first, sizeof first, "the guest-side layout %s is %s", fa[1],
                             pass ? "new in this build" : "not in this build");
                else
                    snprintf(first, sizeof first, "its format differs");
                continue;
            }
            int nb = manifest_fields(o, ol, b, sizeof b, fb);
            if (fa[0][0] == 'S' && na >= 8 && nb >= 8) {
                const char *how = strcmp(fa[5], fb[5]) ? "is another size"
                                  : strcmp(fa[7], fb[7]) ? "has another type"
                                  : strcmp(fa[6], fb[6]) ? "has its pointers elsewhere"
                                                         : "is elsewhere in the saved state";
                if (strcmp(fa[5], fb[5]))
                    snprintf(first, sizeof first, "the static %s (%s) is %s bytes in the snapshot, %s here", fa[2],
                             fa[1], fa[5], fb[5]);
                else
                    snprintf(first, sizeof first, "the static %s (%s) %s", fa[2], fa[1], how);
            } else if (fa[0][0] == 'T' && na >= 6 && nb >= 6) {
                if (strcmp(fa[3], fb[3]))
                    snprintf(first, sizeof first, "the tagged type %s (%s) is %s bytes in the snapshot, %s here", fa[2],
                             fa[1], fa[3], fb[3]);
                else
                    snprintf(first, sizeof first, "the tagged type %s (%s) differs", fa[2], fa[1]);
            } else if (fa[0][0] == 'R' && na >= 3 && nb >= 3) {
                snprintf(first, sizeof first, "the saved state (%s) is %s bytes in the snapshot, %s here", fa[1], fa[2],
                         fb[2]);
            } else if (fa[0][0] == 'L') {
                snprintf(first, sizeof first, "the guest-side layout %s differs", fa[1]);
            } else {
                snprintf(first, sizeof first, "it was saved in another format or for another machine");
            }
        }
    }
    if (!count)
        snprintf(why, whylen, "its saved state differs from this build's");
    else if (count == 1)
        snprintf(why, whylen, "%s", first);
    else
        snprintf(why, whylen, "%s (and %d more differences)", first, count - 1);
}

static bool ready(char *err, size_t errlen)
{
    if (img_ok)
        return true;
    if (!image_load(err, errlen))
        return false;
    if (!img.sect[0] && !img.sect[1]) {
        snprintf(err, errlen, "the binary has no state sections");
        return false;
    }
    place_ctx c = { 0 };
    if (!each_debug_symbol(place_one, &c, err, errlen)) {
        bb_host_free(c.list);
        return false;
    }
    qsort(c.list, c.n, sizeof *c.list, cmp_placed);
    statics = c.list;
    nstatics = c.n;
    if (!manifest_make()) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    if (!session) {
        int local;
#if defined(__APPLE__)
        session = ((uint64_t)arc4random() << 32) | arc4random();
#else
        session = ((uint64_t)getpid() << 32) ^ (uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)&local;
#endif
        session ^= (uint64_t)(uintptr_t)&local;
        if (!session)
            session = 1;
    }
    img_ok = true;
    return true;
}

void hb_fingerprint(char out[33])
{
    char err[128];
    if (!ready(err, sizeof err)) {
        out[0] = 0;
        return;
    }
    for (int i = 0; i < 16; i++)
        snprintf(out + i * 2, 3, "%02x", fingerprint[i]);
}

size_t hb_manifest(const char **out)
{
    char err[128];
    if (!ready(err, sizeof err)) {
        *out = "";
        return 0;
    }
    *out = manifest;
    return manifest_len;
}

void hb_build_id(char out[33])
{
    char err[128];
    if (!ready(err, sizeof err)) {
        out[0] = 0;
        return;
    }
    for (int i = 0; i < 16; i++)
        snprintf(out + i * 2, 3, "%02x", img.uuid[i]);
}

/* ---- Layouts ---- */

typedef void (*field_fn)(void *ctx, size_t off, char kind);

/* Every field of a layout at base within a container of the given size. */
static void each_field(uint32_t layout, size_t base, size_t limit, field_fn fn, void *ctx)
{
    if (layout >= hb_nlayouts)
        return;
    const hb_layout *l = &hb_layouts[layout];
    for (uint32_t r = 0; r < l->n; r++) {
        const hb_run *run = &hb_runs[l->first + r];
        for (uint32_t i = 0; i < run->count; i++)
            for (uint32_t j = 0; j < run->count2; j++) {
                size_t off = base + run->off + (size_t)i * run->stride + (size_t)j * run->stride2;
                if (off + 8 <= limit)
                    fn(ctx, off, run->kind);
            }
    }
}

/* A block's layout from its descriptor, remembered by descriptor. */
typedef struct {
    const bb_type *type;
    int32_t layout; /* -1: none (bytes, untyped, or not in the map) */
    uint32_t size;
} type_slot;

typedef struct {
    type_slot *slots;
    size_t cap;
    uint32_t stale;
} type_cache;

static int32_t type_layout(type_cache *tc, const bb_type *t, uint32_t *size)
{
    size_t h = ((uintptr_t)t >> 4) & (tc->cap - 1);
    while (tc->slots[h].type && tc->slots[h].type != t)
        h = (h + 1) & (tc->cap - 1);
    type_slot *s = &tc->slots[h];
    if (!s->type) {
        s->type = t;
        s->layout = -1;
        if (t->form == BB_FORM_ARRAY || t->form == BB_FORM_HEAD) {
            const hb_typemap *m = map_type(t->file, t->name);
            if (m && m->size == t->size) {
                s->layout = (int32_t)m->layout;
                s->size = m->size;
            } else {
                tc->stale++;
            }
        }
    }
    *size = s->size;
    return s->layout;
}

static void each_block_field(type_cache *tc, const bb_block *b, field_fn fn, void *ctx)
{
    uint32_t esize;
    int32_t layout = type_layout(tc, b->type, &esize);
    if (layout <= 0 || esize == 0)
        return;
    if (b->type->form == BB_FORM_HEAD) {
        if (b->size >= esize)
            each_field((uint32_t)layout, 0, b->size, fn, ctx);
        return;
    }
    for (size_t e = 0; e + esize <= b->size; e += esize)
        each_field((uint32_t)layout, e, b->size, fn, ctx);
}

/* ---- What a value points to ---- */

typedef struct {
    uintptr_t start;
    uint32_t size, index;
} block_span;

typedef struct {
    block_span *spans; /* by address */
    bb_block **blocks; /* by number */
    size_t n;
} block_index;

static int cmp_span(const void *a, const void *b)
{
    uintptr_t x = ((const block_span *)a)->start, y = ((const block_span *)b)->start;
    return x < y ? -1 : x > y;
}

static bool index_blocks(block_index *bi)
{
    size_t n = bb_blocks_count();
    bi->n = 0;
    bi->spans = bb_host_malloc((n ? n : 1) * sizeof *bi->spans);
    bi->blocks = bb_host_malloc((n ? n : 1) * sizeof *bi->blocks);
    if (!bi->spans || !bi->blocks)
        return false;
    for (bb_block *b = bb_blocks_first(); b && bi->n < n; b = b->next) {
        bi->spans[bi->n] = (block_span){ (uintptr_t)BB_BLOCK_DATA(b), b->size, (uint32_t)bi->n };
        bi->blocks[bi->n] = b;
        bi->n++;
    }
    qsort(bi->spans, bi->n, sizeof *bi->spans, cmp_span);
    return true;
}

static void free_index(block_index *bi)
{
    bb_host_free(bi->spans);
    bb_host_free(bi->blocks);
}

/* The block holding v (one past its end counts: a block's end pointer). */
static const block_span *block_at(const block_index *bi, uintptr_t v)
{
    size_t lo = 0, hi = bi->n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (bi->spans[mid].start <= v)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return NULL;
    const block_span *s = &bi->spans[lo - 1];
    return v - s->start <= s->size ? s : NULL;
}

typedef struct {
    int kind;
    uint32_t target;
    uint64_t value;
} target;

/* Without asking dladdr (which the scan cannot afford for every word). */
static bool classify_known(const block_index *bi, uintptr_t v, target *t)
{
    const block_span *s = block_at(bi, v);
    if (s) {
        *t = (target){ R_BLOCK, s->index, v - s->start };
        return true;
    }
    int w;
    if (in_saved(v, &w)) {
        *t = (target){ R_STATE, (uint32_t)w, v - (uintptr_t)img.sect[w] };
        return true;
    }
    for (int i = 0; i < bb_region_count(); i++) {
        const bb_region *r = bb_region_at(i);
        if (v >= (uintptr_t)r->base && v <= (uintptr_t)r->base + r->size) {
            *t = (target){ R_REGION, (uint32_t)i, v - (uintptr_t)r->base };
            return true;
        }
    }
    if (v >= img.lo && v < img.hi) {
        *t = (target){ R_IMAGE, 0, v - (uintptr_t)img.base };
        return true;
    }
    return false;
}

/* ---- Writing ---- */

typedef struct {
    uint8_t *buf;
    size_t len, cap;
    bool failed;
} writer;

static void *w_reserve(writer *w, size_t n)
{
    size_t need = (w->len + n + 7) & ~(size_t)7;
    if (need > w->cap) {
        size_t cap = w->cap ? w->cap : 1 << 20;
        while (cap < need)
            cap *= 2;
        uint8_t *b = bb_host_realloc(w->buf, cap);
        if (!b) {
            w->failed = true;
            return NULL;
        }
        w->buf = b;
        w->cap = cap;
    }
    void *p = w->buf + w->len;
    memset(w->buf + w->len, 0, need - w->len);
    w->len = need;
    return p;
}

static size_t w_put(writer *w, const void *src, size_t n)
{
    size_t at = w->len;
    void *p = w_reserve(w, n);
    if (p && n)
        memcpy(p, src, n);
    return at;
}

typedef struct {
    char lib[1024], sym[256];
} extern_ref;

typedef struct {
    writer *w;
    block_index *bi;
    type_cache *tc;
    hb_stats *st;
    /* the container being written */
    const uint8_t *live;
    size_t out_at, size;
    uint32_t where;
    /* relocations and libraries so far */
    file_reloc *relocs;
    size_t nrelocs, cap;
    extern_ref *externs;
    size_t nexterns, ecap;
    /* the binary's symbols, string constants and block types pointed to,
     * each once (by its index here: in the binary's symbols, a constant's
     * address, a descriptor), with their names in the pool */
    file_sym *syms;
    size_t nsyms, scap;
    uint32_t *sym_of; /* the binary's symbol -> its index + 1 */
    file_cstr *cstrs;
    uintptr_t *cstr_addr;
    size_t ncstrs, ccap;
    file_type *types;
    const bb_type **type_desc;
    size_t ntypes, tcap;
    char *pool;
    size_t pool_len, pool_cap;
} save_ctx;

static bool grow(void **p, size_t *cap, size_t n, size_t each)
{
    if (n < *cap)
        return true;
    size_t c = *cap ? *cap * 2 : 256;
    void *q = bb_host_realloc(*p, c * each);
    if (!q)
        return false;
    *p = q;
    *cap = c;
    return true;
}

static uint32_t pool_add(save_ctx *c, const char *s, size_t n)
{
    if (c->pool_len + n + 1 > c->pool_cap) {
        size_t cap = c->pool_cap ? c->pool_cap : 1 << 16;
        while (cap < c->pool_len + n + 1)
            cap *= 2;
        char *p = bb_host_realloc(c->pool, cap);
        if (!p) {
            c->w->failed = true;
            return 0;
        }
        c->pool = p;
        c->pool_cap = cap;
    }
    uint32_t at = (uint32_t)c->pool_len;
    memcpy(c->pool + at, s, n);
    c->pool[at + n] = 0;
    c->pool_len += n + 1;
    return at;
}

/* A pointer into the binary, by name: the symbol it is in, or the string
 * constant; false when it is in neither. */
static bool symbolize(save_ctx *c, uintptr_t v, target *t)
{
    if (!isyms)
        return false;
    uint32_t cs = cstring_section(v);
    if (cs) {
        const char *lo = (const char *)img.sects[cs].lo, *hi = (const char *)img.sects[cs].hi;
        const char *p = (const char *)v, *start = p;
        while (start > lo && start[-1])
            start--;
        size_t n = strnlen(start, (size_t)(hi - start));
        if (start + n == hi)
            return false;
        for (size_t i = 0; i < c->ncstrs; i++)
            if (c->cstr_addr[i] == (uintptr_t)start) {
                *t = (target){ R_CSTR, (uint32_t)i, (uint64_t)(p - start) };
                return true;
            }
        if (!grow((void **)&c->cstrs, &c->ccap, c->ncstrs, sizeof *c->cstrs) ||
            !(c->cstr_addr = bb_host_realloc(c->cstr_addr, c->ccap * sizeof *c->cstr_addr))) {
            c->w->failed = true;
            return false;
        }
        c->cstrs[c->ncstrs] = (file_cstr){ pool_add(c, start, n), (uint32_t)n };
        c->cstr_addr[c->ncstrs] = (uintptr_t)start;
        *t = (target){ R_CSTR, (uint32_t)c->ncstrs++, (uint64_t)(p - start) };
        return true;
    }
    const isym *s = isym_at(v);
    if (!s)
        return false;
    size_t k = (size_t)(s - isyms);
    if (!c->sym_of[k]) {
        if (!grow((void **)&c->syms, &c->scap, c->nsyms, sizeof *c->syms)) {
            c->w->failed = true;
            return false;
        }
        c->syms[c->nsyms] = (file_sym){ pool_add(c, s->file, strlen(s->file)), pool_add(c, s->name, strlen(s->name)),
                                        s->code ? SYM_CODE : 0, 0, s->size };
        c->sym_of[k] = (uint32_t)++c->nsyms;
    }
    uint32_t i = c->sym_of[k] - 1;
    if (v != s->addr)
        c->syms[i].flags |= SYM_INTERIOR;
    *t = (target){ R_SYM, i, v - s->addr };
    return true;
}

/* A block's type, in the snapshot's table. */
static uint32_t type_index(save_ctx *c, const bb_type *t)
{
    for (size_t i = 0; i < c->ntypes; i++)
        if (c->type_desc[i] == t)
            return (uint32_t)i;
    if (!grow((void **)&c->types, &c->tcap, c->ntypes, sizeof *c->types) ||
        !(c->type_desc = bb_host_realloc(c->type_desc, c->tcap * sizeof *c->type_desc))) {
        c->w->failed = true;
        return 0;
    }
    c->types[c->ntypes] = (file_type){ pool_add(c, t->name, strlen(t->name)), pool_add(c, t->file, strlen(t->file)),
                                       t->size, t->form, t->line, 0 };
    c->type_desc[c->ntypes] = t;
    return (uint32_t)c->ntypes++;
}

static uint32_t extern_index(save_ctx *c, const char *lib, const char *sym)
{
    for (size_t i = 0; i < c->nexterns; i++)
        if (!strcmp(c->externs[i].lib, lib) && !strcmp(c->externs[i].sym, sym))
            return (uint32_t)i;
    if (c->nexterns == c->ecap) {
        size_t cap = c->ecap ? c->ecap * 2 : 16;
        extern_ref *e = bb_host_realloc(c->externs, cap * sizeof *e);
        if (!e)
            return UINT32_MAX;
        c->externs = e;
        c->ecap = cap;
    }
    extern_ref *e = &c->externs[c->nexterns];
    snprintf(e->lib, sizeof e->lib, "%s", lib);
    snprintf(e->sym, sizeof e->sym, "%s", sym);
    return (uint32_t)c->nexterns++;
}

static void add_reloc(save_ctx *c, size_t off, int kind, uint32_t tgt, uint64_t value)
{
    if (c->nrelocs == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 4096;
        file_reloc *r = bb_host_realloc(c->relocs, cap * sizeof *r);
        if (!r) {
            c->w->failed = true;
            return;
        }
        c->relocs = r;
        c->cap = cap;
    }
    c->relocs[c->nrelocs++] = (file_reloc){ c->where, ((uint32_t)kind << 24) | (tgt & 0xFFFFFFu), off, value };
    c->st->relocs++;
    switch (kind) {
    case R_BLOCK: c->st->to_blocks++; break;
    case R_STATE: c->st->to_statics++; break;
    case R_REGION: c->st->to_regions++; break;
    case R_IMAGE: c->st->to_image++; break;
    case R_EXTERN: c->st->to_libraries++; break;
    case R_HOST: c->st->host++; break;
    case R_SYM:
        c->st->to_image++;
        if (c->syms[tgt].flags & SYM_CODE)
            *(value ? &c->st->to_code_inside : &c->st->to_functions) += 1;
        else
            *(value ? &c->st->to_data_inside : &c->st->to_data) += 1;
        break;
    case R_CSTR: c->st->to_image++; c->st->to_strings++; break;
    }
    if (kind == R_IMAGE)
        c->st->to_image_unnamed++;
}

static void save_field(void *vctx, size_t off, char kind)
{
    save_ctx *c = vctx;
    if (kind == 'Y' || kind == 'D' || kind == 'I' || kind == 'X' || kind == 'x')
        return;
    uint64_t v;
    memcpy(&v, c->live + off, 8);
    if (!v)
        return;
    target t;
    bool known = kind != 'H' && classify_known(c->bi, (uintptr_t)v, &t);
    if (!known && kind == 'U')
        return; /* data */
    if (known && t.kind == R_IMAGE)
        symbolize(c, (uintptr_t)v, &t);
    if (!known && kind != 'H') {
        Dl_info info;
        if (dladdr((const void *)(uintptr_t)v, &info) && info.dli_fname && info.dli_fbase) {
            const char *sym = info.dli_sname && info.dli_saddr ? info.dli_sname : "";
            uintptr_t from = *sym ? (uintptr_t)info.dli_saddr : (uintptr_t)info.dli_fbase;
            uint32_t e = extern_index(c, info.dli_fname, sym);
            if (e != UINT32_MAX) {
                t = (target){ R_EXTERN, e, v - from };
                known = true;
            }
        }
    }
    if (!known)
        t = (target){ R_HOST, 0, v };
    memset(c->w->buf + c->out_at + off, 0, 8);
    add_reloc(c, off, t.kind, t.target, t.value);
}

static void save_static_fields(save_ctx *c, int sect)
{
    uintptr_t lo = (uintptr_t)img.sect[sect], hi = lo + img.sect_size[sect];
    for (size_t i = 0; i < nstatics; i++)
        if (statics[i].addr >= lo && statics[i].addr < hi && statics[i].layout)
            each_field(statics[i].layout, statics[i].addr - lo, img.sect_size[sect], save_field, c);
}

/* The pages of a region that hold anything: resident and not all zero. */
static bool page_used(const uint8_t *p, size_t page)
{
    const uint64_t *q = (const uint64_t *)p;
    for (size_t i = 0; i < page / 8; i++)
        if (q[i])
            return true;
    return false;
}

static size_t region_extent(const bb_region *r)
{
    size_t n = r->extent ? r->extent() : r->size;
    return n < r->size ? n : r->size;
}

static void save_region(writer *w, const bb_region *r, hb_stats *st)
{
    size_t page = (size_t)getpagesize(), extent = region_extent(r) & ~(page - 1);
    file_region fr = { { 0 }, r->size, 0 };
    snprintf(fr.name, sizeof fr.name, "%s", r->name);
    size_t head = w_put(w, &fr, sizeof fr);
    /* Runs first (counted), then their bytes. */
    size_t runs_at = w->len, nruns = 0;
    const size_t chunk = (size_t)1 << 26; /* mincore's vector a chunk at a time */
    unsigned char *vec = bb_host_malloc(chunk / page);
    if (!vec) {
        w->failed = true;
        return;
    }
    for (size_t base = 0; base < extent; base += chunk) {
        size_t n = extent - base < chunk ? extent - base : chunk;
        if (mincore(r->base + base, n, (void *)vec) != 0)
            memset(vec, 1, n / page);
        size_t run_start = 0, run_len = 0;
        for (size_t p = 0; p <= n / page; p++) {
            bool used = p < n / page && (vec[p] & 1) && page_used(r->base + base + p * page, page);
            if (used) {
                if (!run_len)
                    run_start = base + p * page;
                run_len += page;
            } else if (run_len) {
                file_run fr2 = { run_start, run_len };
                w_put(w, &fr2, sizeof fr2);
                nruns++;
                run_len = 0;
            }
        }
    }
    bb_host_free(vec);
    if (w->failed)
        return;
    ((file_region *)(w->buf + head))->nruns = nruns;
    for (size_t i = 0; i < nruns; i++) {
        file_run fr2;
        memcpy(&fr2, w->buf + runs_at + i * sizeof fr2, sizeof fr2);
        w_put(w, r->base + fr2.off, fr2.len);
        st->region_bytes += fr2.len;
    }
}

bool hb_save_buffer(uint8_t **out, size_t *len, hb_stats *stats, char *err, size_t errlen)
{
    hb_stats local;
    hb_stats *st = stats ? stats : &local;
    memset(st, 0, sizeof *st);
    if (!ready(err, errlen))
        return false;
    writer w = { 0 };
    file_header h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, MAGIC, 8);
    h.version = VERSION;
    memcpy(h.uuid, img.uuid, 16);
    memcpy(h.fingerprint, fingerprint, 16);
    h.session = session;
    w_put(&w, &h, sizeof h);
    /* Without the binary's symbols its pointers are saved by offset: for
     * this build only. */
    char serr[128];
    symbols_ready(serr, sizeof serr);

    bb_blocks_lock();
    block_index bi;
    type_cache tc = { 0 };
    tc.cap = 4096;
    tc.slots = bb_host_calloc(tc.cap, sizeof *tc.slots);
    save_ctx c;
    memset(&c, 0, sizeof c);
    c.w = &w;
    c.bi = &bi;
    c.tc = &tc;
    c.st = st;
    c.sym_of = bb_host_calloc(nisyms + 1, sizeof *c.sym_of);
    if (!index_blocks(&bi) || !tc.slots || !c.sym_of) {
        bb_blocks_unlock();
        free_index(&bi);
        bb_host_free(tc.slots);
        bb_host_free(c.sym_of);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    st->statics = (uint32_t)nstatics;

    ((file_header *)w.buf)->sections_off = w.len;
    for (int i = 0; i < NSAVED; i++) {
        file_section fs = { { 0 }, img.sect_size[i] };
        snprintf(fs.name, sizeof fs.name, "%s", saved_names[i]);
        w_put(&w, &fs, sizeof fs);
        c.live = img.sect[i];
        c.size = img.sect_size[i];
        c.where = (uint32_t)i;
        c.out_at = w_put(&w, img.sect[i], img.sect_size[i]);
        if (!w.failed)
            save_static_fields(&c, i);
        st->section_bytes += img.sect_size[i];
        st->sections++;
    }

    if (!w.failed)
        ((file_header *)w.buf)->blocks_off = w.len;
    for (size_t i = 0; i < bi.n && !w.failed; i++) {
        bb_block *b = bi.blocks[i];
        uintptr_t t = (uintptr_t)b->type;
        if (t < img.lo || t >= img.hi) {
            bb_blocks_unlock();
            free_index(&bi);
            bb_host_free(tc.slots);
            snprintf(err, errlen, "a block's type is not in the binary");
            return false;
        }
        file_block fb = { type_index(&c, b->type), b->size };
        w_put(&w, &fb, sizeof fb);
        c.live = BB_BLOCK_DATA(b);
        c.size = b->size;
        c.where = (uint32_t)(NSAVED + i);
        c.out_at = w_put(&w, BB_BLOCK_DATA(b), b->size);
        if (w.failed)
            break;
        if (b->type->form == BB_FORM_UNTYPED)
            st->untyped_blocks++;
        each_block_field(&tc, b, save_field, &c);
        st->block_bytes += b->size;
        st->blocks++;
    }
    st->stale_types = tc.stale;
    size_t nblocks = bi.n;
    bb_blocks_unlock();
    free_index(&bi);
    bb_host_free(tc.slots);

    if (!w.failed)
        ((file_header *)w.buf)->regions_off = w.len;
    int nregions = bb_region_count();
    for (int i = 0; i < nregions && !w.failed; i++) {
        save_region(&w, bb_region_at(i), st);
        st->regions++;
    }
    if (!w.failed)
        ((file_header *)w.buf)->relocs_off = w.len;
    if (!w.failed)
        w_put(&w, c.relocs, c.nrelocs * sizeof *c.relocs);
    if (!w.failed)
        ((file_header *)w.buf)->externs_off = w.len;
    for (size_t i = 0; i < c.nexterns && !w.failed; i++)
        w_put(&w, &c.externs[i], sizeof c.externs[i]);
    if (!w.failed)
        ((file_header *)w.buf)->syms_off = w_put(&w, c.syms, c.nsyms * sizeof *c.syms);
    if (!w.failed)
        ((file_header *)w.buf)->cstrs_off = w_put(&w, c.cstrs, c.ncstrs * sizeof *c.cstrs);
    if (!w.failed)
        ((file_header *)w.buf)->types_off = w_put(&w, c.types, c.ntypes * sizeof *c.types);
    if (!w.failed)
        ((file_header *)w.buf)->pool_off = w_put(&w, c.pool, c.pool_len);
    bb_host_free(c.relocs);
    bb_host_free(c.externs);
    bb_host_free(c.syms);
    bb_host_free(c.sym_of);
    bb_host_free(c.cstrs);
    bb_host_free(c.cstr_addr);
    bb_host_free(c.types);
    bb_host_free(c.type_desc);
    bb_host_free(c.pool);
    if (w.failed) {
        bb_host_free(w.buf);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    file_header *hp = (file_header *)w.buf;
    hp->nsyms = c.nsyms;
    hp->ncstrs = c.ncstrs;
    hp->ntypes = c.ntypes;
    hp->pool_len = c.pool_len;

    hp->nsections = NSAVED;
    hp->nblocks = nblocks;
    hp->nregions = (uint64_t)nregions;
    hp->nrelocs = c.nrelocs;
    hp->nexterns = c.nexterns;
    hp->total = w.len;
    st->bytes = w.len;
    *out = w.buf;
    *len = w.len;
    return true;
}

void hb_free_buffer(uint8_t *buf)
{
    bb_host_free(buf);
}

/* ---- Files ----
 *
 * A snapshot file: a header (its own magic, the build's UUID, the sizes),
 * the caller's record of the launch (bytes, kept as they are, so a host
 * can read what program it is without inflating the rest), then the
 * snapshot deflated with zlib (most of it is zero: the state sections'
 * bss, the guest's memory the program never touched). */
#define FILE_MAGIC "BBHIBZ01"

typedef struct {
    char magic[8];
    uint32_t version, flags;
    uint8_t uuid[16];
    uint64_t raw_len, data_len, extra_len;
    /* From version 2: the state fingerprint, and the manifest (deflated,
     * after the caller's bytes), which says why another build's cannot
     * be put back without inflating the rest. */
    uint8_t fingerprint[16];
    uint64_t manifest_len, manifest_raw;
} file_wrapper;

/* A wrapper's size as version 1 wrote it. */
#define WRAPPER_V1 56

static bool write_all(FILE *f, const void *p, size_t n) { return !n || fwrite(p, 1, n, f) == n; }

bool hb_save_file_extra(const char *path, const void *extra, size_t extra_len, hb_stats *stats, char *err,
                        size_t errlen)
{
    uint8_t *buf;
    size_t len;
    if (!hb_save_buffer(&buf, &len, stats, err, errlen))
        return false;
    bool ok = hb_write_file_extra(path, buf, len, extra, extra_len, stats, err, errlen);
    hb_free_buffer(buf);
    return ok;
}

bool hb_write_file_extra(const char *path, const uint8_t *buf, size_t len, const void *extra, size_t extra_len,
                         hb_stats *stats, char *err, size_t errlen)
{
    if (!ready(err, errlen))
        return false;
    uLongf zlen = compressBound((uLong)len);
    uint8_t *z = bb_host_malloc(zlen);
    if (!z || compress2(z, &zlen, buf, (uLong)len, 6) != Z_OK) {
        bb_host_free(z);
        snprintf(err, errlen, "out of memory compressing the snapshot");
        return false;
    }
    /* This build's manifest (hb_save_buffer's snapshots are this build's),
     * which says why another build cannot put it back. */
    uLongf mlen = compressBound((uLong)manifest_len);
    uint8_t *mz = bb_host_malloc(mlen);
    if (!mz || compress2(mz, &mlen, (const Bytef *)manifest, (uLong)manifest_len, 6) != Z_OK) {
        bb_host_free(z);
        bb_host_free(mz);
        snprintf(err, errlen, "out of memory compressing the snapshot");
        return false;
    }
    file_wrapper fw;
    memset(&fw, 0, sizeof fw);
    memcpy(fw.magic, FILE_MAGIC, 8);
    fw.version = VERSION;
    memcpy(fw.uuid, img.uuid, 16);
    memcpy(fw.fingerprint, fingerprint, 16);
    fw.raw_len = len;
    fw.data_len = zlen;
    fw.extra_len = extra_len;
    fw.manifest_len = mlen;
    fw.manifest_raw = manifest_len;
    /* Written beside, then renamed over: never half a snapshot. */
    char tmp[2048];
    snprintf(tmp, sizeof tmp, "%s.partial", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && write_all(f, &fw, sizeof fw) && write_all(f, extra, extra_len) && write_all(f, mz, mlen) &&
              write_all(f, z, zlen);
    if (f && fclose(f) != 0)
        ok = false;
    bb_host_free(z);
    bb_host_free(mz);
    if (ok && rename(tmp, path) != 0)
        ok = false;
    if (!ok) {
        unlink(tmp);
        snprintf(err, errlen, "%s: cannot write", path);
        return false;
    }
    if (stats)
        stats->file_bytes = sizeof fw + extra_len + mlen + zlen;
    return true;
}

bool hb_save_file(const char *path, hb_stats *stats, char *err, size_t errlen)
{
    return hb_save_file_extra(path, NULL, 0, stats, err, errlen);
}

static bool read_wrapper(FILE *f, const char *path, file_wrapper *fw, char *err, size_t errlen)
{
    memset(fw, 0, sizeof *fw);
    if (fread(fw, 1, WRAPPER_V1, f) != WRAPPER_V1 || memcmp(fw->magic, FILE_MAGIC, 8)) {
        snprintf(err, errlen, "%s: not a snapshot", path);
        return false;
    }
    if (fw->version != VERSION && fw->version != 1) {
        snprintf(err, errlen, "%s: not a snapshot this version of Blue Box reads", path);
        return false;
    }
    if (fw->version == VERSION && fread((uint8_t *)fw + WRAPPER_V1, 1, sizeof *fw - WRAPPER_V1, f) !=
                                      sizeof *fw - WRAPPER_V1) {
        snprintf(err, errlen, "%s: the snapshot is damaged", path);
        return false;
    }
    if (fw->extra_len > (1u << 24) || fw->raw_len > ((uint64_t)1 << 36) || fw->data_len > ((uint64_t)1 << 36) ||
        fw->manifest_len > (1u << 26) || fw->manifest_raw > (1u << 28)) {
        snprintf(err, errlen, "%s: the snapshot is damaged", path);
        return false;
    }
    return true;
}

/* Whether this build puts back a snapshot with this wrapper, and if not
 * why (f just after the caller's bytes: the manifest is next). */
static bool wrapper_fits(FILE *f, const file_wrapper *fw, char *why, size_t whylen)
{
    if (fw->version == 1) {
        snprintf(why, whylen, "it was saved by an older Blue Box, whose snapshots resume only in the build "
                              "that saved them");
        return false;
    }
    if (!memcmp(fw->uuid, img.uuid, 16) || !memcmp(fw->fingerprint, fingerprint, 16))
        return true;
    uint8_t *z = bb_host_malloc(fw->manifest_len + 1);
    char *m = bb_host_malloc(fw->manifest_raw + 1);
    uLongf mlen = (uLongf)fw->manifest_raw;
    char first[512] = "";
    if (z && m && fw->manifest_len && fread(z, 1, fw->manifest_len, f) == fw->manifest_len &&
        uncompress((Bytef *)m, &mlen, z, (uLong)fw->manifest_len) == Z_OK)
        manifest_explain(m, mlen, first, sizeof first);
    snprintf(why, whylen, "it was saved by a build of Blue Box whose saved state differs from this one's%s%s",
             *first ? ": " : "", first);
    bb_host_free(z);
    bb_host_free(m);
    return false;
}

bool hb_file_extra(const char *path, uint8_t **extra, size_t *extra_len, bool *resumable, char *why, size_t whylen,
                   char *err, size_t errlen)
{
    *extra = NULL;
    *extra_len = 0;
    if (why && whylen)
        *why = 0;
    if (!ready(err, errlen))
        return false;
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "%s: cannot open", path);
        return false;
    }
    file_wrapper fw;
    bool ok = read_wrapper(f, path, &fw, err, errlen);
    uint8_t *e = ok ? bb_host_malloc(fw.extra_len + 1) : NULL;
    if (ok && (!e || fread(e, 1, fw.extra_len, f) != fw.extra_len)) {
        snprintf(err, errlen, "%s: cannot read", path);
        ok = false;
    }
    char reason[512] = "";
    bool fits = ok && wrapper_fits(f, &fw, reason, sizeof reason);
    fclose(f);
    if (!ok) {
        bb_host_free(e);
        return false;
    }
    e[fw.extra_len] = 0;
    *extra = e;
    *extra_len = fw.extra_len;
    if (resumable)
        *resumable = fits;
    if (why && whylen)
        snprintf(why, whylen, "%s", reason);
    return true;
}

bool hb_file_load(const char *path, uint8_t **out, size_t *len, char *err, size_t errlen)
{
    *out = NULL;
    *len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "%s: cannot open", path);
        return false;
    }
    file_wrapper fw;
    bool ok = read_wrapper(f, path, &fw, err, errlen);
    if (ok && fw.version != VERSION) {
        snprintf(err, errlen, "%s: saved by an older Blue Box, whose snapshots resume only in the build that saved them",
                 path);
        ok = false;
    }
    uint8_t *z = ok ? bb_host_malloc(fw.data_len ? fw.data_len : 1) : NULL;
    uint8_t *raw = ok ? bb_host_malloc(fw.raw_len ? fw.raw_len : 1) : NULL;
    if (ok && (!z || !raw || fseek(f, (long)(fw.extra_len + fw.manifest_len), SEEK_CUR) != 0 ||
               fread(z, 1, fw.data_len, f) != fw.data_len)) {
        snprintf(err, errlen, "%s: cannot read", path);
        ok = false;
    }
    fclose(f);
    uLongf rlen = (uLongf)fw.raw_len;
    if (ok && (uncompress(raw, &rlen, z, (uLong)fw.data_len) != Z_OK || rlen != fw.raw_len)) {
        snprintf(err, errlen, "%s: the snapshot is damaged", path);
        ok = false;
    }
    bb_host_free(z);
    if (!ok) {
        bb_host_free(raw);
        return false;
    }
    *out = raw;
    *len = rlen;
    return true;
}

/* ---- Reading ---- */

typedef struct {
    const uint8_t *buf;
    size_t len;
} reader;

static bool span_ok(const reader *r, uint64_t off, uint64_t n)
{
    return off <= r->len && n <= r->len - off;
}

static uint64_t pad8(uint64_t n)
{
    return (n + 7) & ~(uint64_t)7;
}

static void *lib_symbol(const extern_ref *e)
{
#if defined(__APPLE__)
    if (*e->sym) {
        void *h = dlopen(e->lib, RTLD_LAZY | RTLD_NOLOAD);
        void *p = h ? dlsym(h, e->sym) : NULL;
        if (h)
            dlclose(h);
        return p;
    }
#endif
    return (void *)lib_named(e->lib);
}

/* What a snapshot's names are in this binary. */
typedef struct {
    uintptr_t *syms, *cstrs;
    bool *code;
    const bb_type **types;
} resolved;

static void free_resolved(resolved *rs)
{
    bb_host_free(rs->syms);
    bb_host_free(rs->cstrs);
    bb_host_free(rs->code);
    bb_host_free(rs->types);
}

static const char *pool_at(const uint8_t *buf, const file_header *h, uint32_t off)
{
    return off < h->pool_len ? (const char *)buf + h->pool_off + off : "";
}

/* Each symbol, string constant and block type the snapshot names, found
 * here; false (and why) when one is not, or is not as it was. */
static bool resolve(const uint8_t *buf, const file_header *h, bool same_build, uint64_t unnamed, resolved *rs,
                    char *why, size_t whylen)
{
    memset(rs, 0, sizeof *rs);
    if (unnamed && !same_build) {
        snprintf(why, whylen, "the snapshot holds %llu pointers into the binary that have no name: only the build "
                              "that saved it can put it back", (unsigned long long)unnamed);
        return false;
    }
    char err[128] = "";
    if ((h->nsyms || h->ncstrs || h->ntypes) && (!symbols_ready(err, sizeof err) || !descriptors_ready())) {
        snprintf(why, whylen, "%s", *err ? err : "out of memory");
        return false;
    }
    rs->syms = bb_host_calloc(h->nsyms + 1, sizeof *rs->syms);
    rs->code = bb_host_calloc(h->nsyms + 1, sizeof *rs->code);
    rs->cstrs = bb_host_calloc(h->ncstrs + 1, sizeof *rs->cstrs);
    rs->types = bb_host_calloc(h->ntypes + 1, sizeof *rs->types);
    if (!rs->syms || !rs->code || !rs->cstrs || !rs->types || (h->ncstrs && !cstrings_ready())) {
        free_resolved(rs);
        snprintf(why, whylen, "out of memory");
        return false;
    }
    const file_sym *fs = (const file_sym *)(buf + h->syms_off);
    for (uint64_t i = 0; i < h->nsyms; i++) {
        const char *file = pool_at(buf, h, fs[i].file), *name = pool_at(buf, h, fs[i].name);
        const isym *s = isym_named(file, name);
        if (!s) {
            snprintf(why, whylen, "the snapshot points to %s%s%s%s, which this build does not have", name,
                     *file ? " (" : "", file, *file ? ")" : "");
            free_resolved(rs);
            return false;
        }
        if ((fs[i].flags & SYM_INTERIOR) && s->size != fs[i].size) {
            snprintf(why, whylen, "the snapshot points into %s%s%s%s, which is %llu bytes in this build and was %llu",
                     name, *file ? " (" : "", file, *file ? ")" : "", (unsigned long long)s->size,
                     (unsigned long long)fs[i].size);
            free_resolved(rs);
            return false;
        }
        rs->syms[i] = s->addr;
        rs->code[i] = s->code;
    }
    const file_cstr *fc = (const file_cstr *)(buf + h->cstrs_off);
    for (uint64_t i = 0; i < h->ncstrs; i++) {
        const char *text = pool_at(buf, h, fc[i].text);
        rs->cstrs[i] = (uintptr_t)cstring_named(text, strlen(text));
    }
    const file_type *ft = (const file_type *)(buf + h->types_off);
    for (uint64_t i = 0; i < h->ntypes; i++) {
        const char *name = pool_at(buf, h, ft[i].name), *file = pool_at(buf, h, ft[i].file);
        rs->types[i] = descriptor_for(name, file, ft[i].size, ft[i].form, ft[i].line);
        if (!rs->types[i]) {
            snprintf(why, whylen, "the snapshot holds a block of %s (%s, %u bytes), a type this build does not tag",
                     name, file, ft[i].size);
            free_resolved(rs);
            return false;
        }
    }
    return true;
}

bool hb_restore_buffer(const uint8_t *buf, size_t len, int flags, hb_stats *stats, char *err, size_t errlen)
{
    hb_stats local;
    hb_stats *st = stats ? stats : &local;
    memset(st, 0, sizeof *st);
    if (!ready(err, errlen))
        return false;
    reader r = { buf, len };
    file_header h;
    if (len < sizeof h) {
        snprintf(err, errlen, "not a snapshot");
        return false;
    }
    memcpy(&h, buf, sizeof h);
    if (memcmp(h.magic, MAGIC, 8) || h.version != VERSION) {
        snprintf(err, errlen, "not a snapshot this version of Blue Box reads");
        return false;
    }
    if (h.total != len || h.nsections != NSAVED || h.nblocks > UINT32_MAX || h.nsyms > 0xFFFFFF ||
        h.ncstrs > 0xFFFFFF || h.ntypes > UINT32_MAX || !span_ok(&r, h.relocs_off, h.nrelocs * sizeof(file_reloc)) ||
        !span_ok(&r, h.externs_off, h.nexterns * sizeof(extern_ref)) ||
        !span_ok(&r, h.syms_off, h.nsyms * sizeof(file_sym)) || !span_ok(&r, h.cstrs_off, h.ncstrs * sizeof(file_cstr)) ||
        !span_ok(&r, h.types_off, h.ntypes * sizeof(file_type)) || !span_ok(&r, h.pool_off, h.pool_len) ||
        (h.pool_len && buf[h.pool_off + h.pool_len - 1])) {
        snprintf(err, errlen, "the snapshot is damaged");
        return false;
    }
    bool same_build = !memcmp(h.uuid, img.uuid, 16);
    if (!same_build && memcmp(h.fingerprint, fingerprint, 16)) {
        /* (A file's head says what differs: hb_restore_file.) */
        snprintf(err, errlen, "it was saved by a build of Blue Box whose saved state differs from this one's");
        return false;
    }
    bool same_process = h.session == session;

    /* Check everything before changing anything. */
    uint64_t at = h.sections_off;
    uint64_t sect_at[NSAVED];
    for (int i = 0; i < NSAVED; i++) {
        file_section fs;
        if (!span_ok(&r, at, sizeof fs)) {
            snprintf(err, errlen, "the snapshot is damaged");
            return false;
        }
        memcpy(&fs, buf + at, sizeof fs);
        if (strncmp(fs.name, saved_names[i], sizeof fs.name) || fs.size != img.sect_size[i] ||
            !span_ok(&r, at + sizeof fs, fs.size)) {
            snprintf(err, errlen, "the snapshot's state sections do not fit this binary");
            return false;
        }
        sect_at[i] = at + sizeof fs;
        at = pad8(at + sizeof fs + fs.size);
    }
    uint64_t *block_at_ = bb_host_malloc((h.nblocks ? h.nblocks : 1) * sizeof *block_at_);
    uint8_t **fresh_blocks = bb_host_calloc(h.nblocks ? h.nblocks : 1, sizeof *fresh_blocks);
    if (!block_at_ || !fresh_blocks) {
        bb_host_free(block_at_);
        bb_host_free(fresh_blocks);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    at = h.blocks_off;
    for (uint64_t i = 0; i < h.nblocks; i++) {
        file_block fb;
        if (!span_ok(&r, at, sizeof fb)) {
            at = UINT64_MAX;
            break;
        }
        memcpy(&fb, buf + at, sizeof fb);
        if (fb.type >= h.ntypes || !span_ok(&r, at + sizeof fb, fb.size)) {
            at = UINT64_MAX;
            break;
        }
        block_at_[i] = at;
        at = pad8(at + sizeof fb + fb.size);
    }
    uint64_t unnamed = 0;
    for (uint64_t i = 0; i < h.nrelocs && at != UINT64_MAX; i++) {
        file_reloc rl;
        memcpy(&rl, buf + h.relocs_off + i * sizeof rl, sizeof rl);
        int kind = (int)(rl.what >> 24);
        uint32_t tgt = rl.what & 0xFFFFFFu;
        uint64_t limit = rl.where < NSAVED ? img.sect_size[rl.where]
                         : rl.where - NSAVED < h.nblocks
                             ? ((const file_block *)(buf + block_at_[rl.where - NSAVED]))->size
                             : 0;
        if (rl.off + 8 > limit || kind < R_BLOCK || kind > R_CSTR || (kind == R_BLOCK && tgt >= h.nblocks) ||
            (kind == R_STATE && tgt >= NSAVED) || (kind == R_REGION && tgt >= h.nregions) ||
            (kind == R_EXTERN && tgt >= h.nexterns) || (kind == R_SYM && tgt >= h.nsyms) ||
            (kind == R_CSTR && tgt >= h.ncstrs))
            at = UINT64_MAX;
        if (kind == R_IMAGE)
            unnamed++;
    }
    if (at == UINT64_MAX) {
        bb_host_free(block_at_);
        bb_host_free(fresh_blocks);
        snprintf(err, errlen, "the snapshot is damaged");
        return false;
    }
    /* The regions: each must be here, as large. */
    uint8_t *region_base[8] = { 0 };
    uint8_t *region_old[8] = { 0 };
    size_t region_size[8] = { 0 };
    size_t (*region_ext[8])(void) = { 0 };
    uint64_t region_at[8] = { 0 };
    const char *region_name[8] = { 0 };
    if (h.nregions > 8) {
        bb_host_free(block_at_);
        bb_host_free(fresh_blocks);
        snprintf(err, errlen, "the snapshot is damaged");
        return false;
    }
    at = h.regions_off;
    for (uint64_t i = 0; i < h.nregions; i++) {
        file_region fr;
        const bb_region *cur = NULL;
        if (span_ok(&r, at, sizeof fr)) {
            memcpy(&fr, buf + at, sizeof fr);
            fr.name[sizeof fr.name - 1] = 0;
            for (int k = 0; k < bb_region_count(); k++)
                if (!strcmp(bb_region_at(k)->name, fr.name))
                    cur = bb_region_at(k);
        }
        if (!cur || cur->size < fr.size || fr.nruns > fr.size ||
            !span_ok(&r, at + sizeof fr, fr.nruns * sizeof(file_run))) {
            bb_host_free(block_at_);
            bb_host_free(fresh_blocks);
            snprintf(err, errlen, "the snapshot's memory does not fit this machine");
            return false;
        }
        region_at[i] = at;
        region_name[i] = cur->name;
        region_old[i] = cur->base;
        region_size[i] = cur->size;
        region_ext[i] = cur->extent;
        uint64_t data = at + sizeof fr + fr.nruns * sizeof(file_run);
        for (uint64_t k = 0; k < fr.nruns; k++) {
            file_run run;
            memcpy(&run, buf + at + sizeof fr + k * sizeof run, sizeof run);
            if (run.off + run.len > fr.size || !span_ok(&r, data, run.len)) {
                bb_host_free(block_at_);
                bb_host_free(fresh_blocks);
                snprintf(err, errlen, "the snapshot is damaged");
                return false;
            }
            data += pad8(run.len);
        }
        at = data;
    }

    /* What the snapshot points to in the binary, and its blocks' types,
     * found here by name. */
    resolved rs;
    char why[512] = "";
    if (!resolve(buf, &h, same_build, unnamed, &rs, why, sizeof why)) {
        bb_host_free(block_at_);
        bb_host_free(fresh_blocks);
        snprintf(err, errlen, "%s", why);
        return false;
    }

    /* The blocks, made again (the old ones still there meanwhile). */
    bb_block *old = bb_blocks_detach();
    for (uint64_t i = 0; i < h.nblocks; i++) {
        const file_block *fb = (const file_block *)(buf + block_at_[i]);
        const bb_type *t = rs.types[fb->type];
        uint8_t *p = bb_malloc(fb->size, t);
        if (!p) {
            /* Too late to go back: what was there is gone too. */
            snprintf(err, errlen, "out of memory putting the snapshot back");
            bb_host_free(block_at_);
            bb_host_free(fresh_blocks);
            return false;
        }
        memcpy(p, fb + 1, fb->size);
        fresh_blocks[i] = p;
        st->block_bytes += fb->size;
        st->blocks++;
    }

    /* The regions: in fresh memory, or over what is there. */
    for (uint64_t i = 0; i < h.nregions; i++) {
        file_region fr;
        memcpy(&fr, buf + region_at[i], sizeof fr);
        uint8_t *base = region_old[i];
        size_t page = (size_t)getpagesize();
        if (flags & HB_RESTORE_MOVE_REGIONS) {
            void *m = mmap(NULL, region_size[i], PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
            if (m == MAP_FAILED) {
                snprintf(err, errlen, "no room for the guest's memory");
                return false;
            }
            base = m;
        } else {
            /* Zero what the region holds now and the snapshot does not. */
            const bb_region *cur = NULL;
            for (int k = 0; k < bb_region_count(); k++)
                if (!strcmp(bb_region_at(k)->name, region_name[i]))
                    cur = bb_region_at(k);
            size_t extent = cur ? region_extent(cur) & ~(page - 1) : 0;
            const size_t chunk = (size_t)1 << 26;
            unsigned char *vec = bb_host_malloc(chunk / page);
            for (size_t b0 = 0; vec && b0 < extent; b0 += chunk) {
                size_t n = extent - b0 < chunk ? extent - b0 : chunk;
                if (mincore(base + b0, n, (void *)vec) != 0)
                    memset(vec, 1, n / page);
                for (size_t p = 0; p < n / page; p++)
                    if ((vec[p] & 1) && page_used(base + b0 + p * page, page))
                        memset(base + b0 + p * page, 0, page);
            }
            bb_host_free(vec);
        }
        uint64_t data = region_at[i] + sizeof fr + fr.nruns * sizeof(file_run);
        for (uint64_t k = 0; k < fr.nruns; k++) {
            file_run run;
            memcpy(&run, buf + region_at[i] + sizeof fr + k * sizeof run, sizeof run);
            memcpy(base + run.off, buf + data, run.len);
            data += pad8(run.len);
            st->region_bytes += run.len;
        }
        region_base[i] = base;
        st->regions++;
    }

    /* The sections, then every pointer. */
    for (int i = 0; i < NSAVED; i++) {
        memcpy(img.sect[i], buf + sect_at[i], img.sect_size[i]);
        st->section_bytes += img.sect_size[i];
        st->sections++;
    }
    const extern_ref *externs = (const extern_ref *)(buf + h.externs_off);
    void **lib_at = bb_host_calloc(h.nexterns ? h.nexterns : 1, sizeof *lib_at);
    for (uint64_t i = 0; lib_at && i < h.nexterns; i++)
        lib_at[i] = lib_symbol(&externs[i]);
    for (uint64_t i = 0; i < h.nrelocs; i++) {
        file_reloc rl;
        memcpy(&rl, buf + h.relocs_off + i * sizeof rl, sizeof rl);
        int kind = (int)(rl.what >> 24);
        uint32_t tgt = rl.what & 0xFFFFFFu;
        uint8_t *dst = rl.where < NSAVED ? img.sect[rl.where] + rl.off : fresh_blocks[rl.where - NSAVED] + rl.off;
        uint64_t v = 0;
        switch (kind) {
        case R_BLOCK: v = (uint64_t)(uintptr_t)fresh_blocks[tgt] + rl.value; st->to_blocks++; break;
        case R_STATE: v = (uint64_t)(uintptr_t)img.sect[tgt] + rl.value; st->to_statics++; break;
        case R_REGION: v = (uint64_t)(uintptr_t)region_base[tgt] + rl.value; st->to_regions++; break;
        case R_IMAGE: v = (uint64_t)(uintptr_t)img.base + rl.value; st->to_image++; break;
        case R_EXTERN:
            v = lib_at && lib_at[tgt] ? (uint64_t)(uintptr_t)lib_at[tgt] + rl.value : 0;
            st->to_libraries++;
            break;
        case R_HOST: v = same_process && !(flags & HB_RESTORE_FORGET_HOST) ? rl.value : 0; st->host++; break;
        case R_SYM:
            v = (uint64_t)rs.syms[tgt] + rl.value;
            st->to_image++;
            if (rs.code[tgt])
                *(rl.value ? &st->to_code_inside : &st->to_functions) += 1;
            else
                *(rl.value ? &st->to_data_inside : &st->to_data) += 1;
            break;
        case R_CSTR:
            if (!rs.cstrs[tgt]) {
                /* A string this build has not: kept in a block of its own. */
                const file_cstr *fc = (const file_cstr *)(buf + h.cstrs_off) + tgt;
                rs.cstrs[tgt] = (uintptr_t)bb_strdup((const char *)buf + h.pool_off + fc->text);
                st->strings_kept++;
            }
            v = rs.cstrs[tgt] ? (uint64_t)rs.cstrs[tgt] + rl.value : 0;
            st->to_image++;
            st->to_strings++;
            break;
        }
        if (kind == R_IMAGE)
            st->to_image_unnamed++;
        memcpy(dst, &v, 8);
        st->relocs++;
    }
    bb_host_free(lib_at);

    /* What the process had before goes. */
    for (uint64_t i = 0; i < h.nregions; i++)
        if (region_base[i] != region_old[i]) {
            bb_region_add(region_name[i], region_base[i], region_size[i], region_ext[i]);
            munmap(region_old[i], region_size[i]);
        }
    bb_blocks_release(old);
    bb_host_free(block_at_);
    bb_host_free(fresh_blocks);
    free_resolved(&rs);
    /* The JIT's translations are of the code that was there; the
     * instruction cache came back with the rest of the machine. */
    mem_code_after_restore();
    st->bytes = len;
    return true;
}

bool hb_restore_file(const char *path, int flags, hb_stats *stats, char *err, size_t errlen)
{
    /* Another build's that does not fit: why, from the file's head. */
    uint8_t *extra;
    size_t extra_len;
    bool fits = false;
    if (!hb_file_extra(path, &extra, &extra_len, &fits, err, errlen, err, errlen))
        return false;
    hb_free_buffer(extra);
    if (!fits)
        return false;
    uint8_t *buf;
    size_t n;
    if (!hb_file_load(path, &buf, &n, err, errlen))
        return false;
    bool ok = hb_restore_buffer(buf, n, flags, stats, err, errlen);
    hb_free_buffer(buf);
    return ok;
}

/* ---- The scan ---- */

typedef struct {
    char where[160];
    uint64_t off;
    int kind;
    char what[96];
    uint32_t count;
    uint64_t example;
} finding;

/* Each word of the container being scanned: data, a word to look at, or
 * a field the map knows: a pointer, one in a union with data, a library's
 * handle. */
enum { W_DATA, W_LOOK, W_PTR, W_UNION, W_HANDLE };

typedef struct {
    finding *list;
    size_t n, cap;
    uint8_t *word;
    size_t words, room;
} scan_ctx;

static void mark_field(void *vctx, size_t off, char kind)
{
    scan_ctx *c = vctx;
    uint8_t m = kind == 'I' ? W_LOOK : kind == 'Y' || kind == 'D' || kind == 'X' || kind == 'x' ? W_DATA
              : kind == 'U' ? W_UNION : kind == 'H' ? W_HANDLE : W_PTR;
    /* A field at any offset: every word it touches (a word a field of
     * the map's starts at is that field's; one it only reaches into is
     * not looked at). */
    for (size_t w = off / 8; w <= (off + 7) / 8 && w < c->words; w++) {
        if (w * 8 == off)
            c->word[w] = m;
        else if (c->word[w] == W_LOOK)
            c->word[w] = W_DATA;
    }
}

static bool want_words(scan_ctx *c, size_t size, uint8_t fill)
{
    size_t words = size / 8;
    if (words > c->room || !c->word) {
        bb_host_free(c->word);
        c->word = bb_host_malloc(words ? words : 1);
        if (!c->word)
            return false;
        c->room = words;
    }
    c->words = words;
    memset(c->word, fill, words ? words : 1);
    return true;
}

static void set_words(scan_ctx *c, size_t from, size_t to, uint8_t m)
{
    for (size_t w = (from + 7) / 8; w < to / 8 && w < c->words; w++)
        c->word[w] = m;
}

static const char *kind_name(int k)
{
    switch (k) {
    case R_BLOCK: return "a tracked block";
    case R_STATE: return "a static";
    case R_REGION: return "the guest's memory";
    case R_IMAGE: return "the binary";
    case R_HOST: return "the host's memory (kept only in this process)";
    }
    return "?";
}

static void describe_static(uintptr_t a, char *out, size_t n, uint64_t *off)
{
    size_t lo = 0, hi = nstatics;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (statics[mid].addr <= a)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo && a < statics[lo - 1].addr + (statics[lo - 1].size ? statics[lo - 1].size : 1)) {
        snprintf(out, n, "%s: %s", statics[lo - 1].map->file, statics[lo - 1].map->sym);
        *off = a - statics[lo - 1].addr;
    } else {
        snprintf(out, n, "a static the map does not know");
        *off = 0;
    }
}

static void describe_target(const block_index *bi, const target *t, char *out, size_t n)
{
    if (t->kind == R_BLOCK) {
        const bb_block *b = bi->blocks[t->target];
        if (b->type->form <= BB_FORM_HEAD)
            snprintf(out, n, "a block of %s", b->type->name);
        else
            snprintf(out, n, "a block (%s:%u)", b->type->file, b->type->line);
    } else if (t->kind == R_STATE) {
        uint64_t off;
        describe_static((uintptr_t)img.sect[t->target] + t->value, out, n, &off);
    } else {
        snprintf(out, n, "%s", kind_name(t->kind));
    }
}

static void add_finding(scan_ctx *c, const char *where, uint64_t off, const block_index *bi, const target *t,
                        uint64_t value)
{
    char what[96];
    describe_target(bi, t, what, sizeof what);
    for (size_t i = 0; i < c->n; i++)
        if (c->list[i].off == off && !strcmp(c->list[i].where, where) && !strcmp(c->list[i].what, what)) {
            c->list[i].count++;
            return;
        }
    if (c->n == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 256;
        finding *l = bb_host_realloc(c->list, cap * sizeof *l);
        if (!l)
            return;
        c->list = l;
        c->cap = cap;
    }
    finding *f = &c->list[c->n++];
    snprintf(f->where, sizeof f->where, "%s", where);
    f->off = off;
    f->kind = t->kind;
    snprintf(f->what, sizeof f->what, "%s", what);
    f->count = 1;
    f->example = value;
}

static int cmp_finding(const void *a, const void *b)
{
    const finding *x = a, *y = b;
    return x->count != y->count ? (x->count < y->count ? 1 : -1) : strcmp(x->where, y->where);
}

/* A field the map knows: whether it holds the host's memory (a library's
 * object, Swift's), which a snapshot can carry only within the process. */
static bool holds_host(const block_index *bi, uint8_t state, uint64_t v)
{
    if (!v || state == W_UNION)
        return false;
    if (state == W_HANDLE)
        return true;
    target t;
    if (classify_known(bi, (uintptr_t)v, &t))
        return false;
    Dl_info info;
    return !(dladdr((const void *)(uintptr_t)v, &info) && info.dli_fbase);
}

hb_scan_result hb_scan(FILE *out, int max, int flags)
{
    hb_scan_result res = { 0 };
    char err[256];
    if (!ready(err, sizeof err)) {
        if (out)
            fprintf(out, "hibernate scan: %s\n", err);
        return res;
    }
    bool all = flags & HB_SCAN_ALL_WORDS;
    scan_ctx c;
    memset(&c, 0, sizeof c);
    bb_blocks_lock();
    block_index bi;
    type_cache tc;
    memset(&tc, 0, sizeof tc);
    tc.cap = 4096;
    tc.slots = bb_host_calloc(tc.cap, sizeof *tc.slots);
    if (!tc.slots || !index_blocks(&bi)) {
        bb_blocks_unlock();
        bb_host_free(tc.slots);
        return res;
    }
    /* The saved sections: every word no static covers, and the statics'
     * pointer-wide integers. */
    for (int s = 0; s < NSAVED; s++) {
        if (!img.sect[s] || !want_words(&c, img.sect_size[s], W_LOOK))
            continue;
        uintptr_t lo = (uintptr_t)img.sect[s];
        for (size_t i = 0; i < nstatics; i++) {
            if (statics[i].addr < lo || statics[i].addr >= lo + img.sect_size[s])
                continue;
            if (!all)
                set_words(&c, statics[i].addr - lo, statics[i].addr - lo + statics[i].size, W_DATA);
            if (statics[i].layout)
                each_field(statics[i].layout, statics[i].addr - lo, img.sect_size[s], mark_field, &c);
        }
        for (size_t w = 0; w < c.words; w++) {
            uint64_t v;
            memcpy(&v, img.sect[s] + w * 8, 8);
            if (c.word[w] >= W_PTR && holds_host(&bi, c.word[w], v)) {
                char where[160];
                uint64_t off;
                describe_static(lo + w * 8, where, sizeof where, &off);
                target t = { R_HOST, 0, v };
                add_finding(&c, where, off, &bi, &t, v);
                res.host_fields++;
                continue;
            }
            if (c.word[w] != W_LOOK)
                continue;
            target t;
            if (v && classify_known(&bi, (uintptr_t)v, &t)) {
                char where[160];
                uint64_t off;
                describe_static(lo + w * 8, where, sizeof where, &off);
                add_finding(&c, where, off, &bi, &t, v);
                res.in_statics++;
            }
        }
    }
    /* The blocks: a typed one's pointer-wide integers and what no element
     * covers; every word of an untyped one; a block of bytes for pointers
     * to blocks only (its bytes are a file's, a picture's: anything). */
    for (size_t i = 0; i < bi.n; i++) {
        bb_block *b = bi.blocks[i];
        if (!want_words(&c, b->size, W_LOOK))
            continue;
        uint32_t esize = 0;
        int32_t layout = type_layout(&tc, b->type, &esize);
        bool typed = b->type->form == BB_FORM_ARRAY || b->type->form == BB_FORM_HEAD;
        if (typed && layout >= 0 && esize && !all) {
            size_t covered = b->type->form == BB_FORM_HEAD ? (b->size >= esize ? esize : 0)
                                                           : (b->size / esize) * esize;
            set_words(&c, 0, covered, W_DATA);
        }
        each_block_field(&tc, b, mark_field, &c);
        for (size_t w = 0; w < c.words; w++) {
            uint64_t v;
            memcpy(&v, (uint8_t *)BB_BLOCK_DATA(b) + w * 8, 8);
            target t;
            bool host = c.word[w] >= W_PTR && holds_host(&bi, c.word[w], v);
            if (host) {
                t = (target){ R_HOST, 0, v };
            } else {
                if (c.word[w] != W_LOOK || !v || !classify_known(&bi, (uintptr_t)v, &t))
                    continue;
                if (b->type->form == BB_FORM_BYTES && t.kind != R_BLOCK && !all)
                    continue;
            }
            char where[160];
            uint64_t off = w * 8;
            if (typed) {
                snprintf(where, sizeof where, "%s (%s)%s", b->type->name, b->type->file,
                         layout < 0 ? " [not in the map]" : "");
                if (b->type->form == BB_FORM_ARRAY && esize && off < (b->size / esize) * esize)
                    off %= esize;
            } else {
                snprintf(where, sizeof where, "%s block (%s:%u)", b->type->name, b->type->file, b->type->line);
            }
            add_finding(&c, where, off, &bi, &t, v);
            if (host) {
                res.host_fields++;
            } else {
                res.in_blocks++;
                if (b->type->form == BB_FORM_UNTYPED)
                    res.in_untyped++;
            }
        }
    }
    /* The host's sections: a pointer into a tracked block dangles after a restore. */
    for (int s = 0; s < NHOSTSECT; s++) {
        for (size_t w = 0; img.host[s] && w < img.host_size[s] / 8; w++) {
            uint64_t v;
            memcpy(&v, img.host[s] + w * 8, 8);
            const block_span *sp = v ? block_at(&bi, (uintptr_t)v) : NULL;
            if (!sp)
                continue;
            char where[160];
            snprintf(where, sizeof where, "the host's %s at +%zu", host_names[s], w * 8);
            target t = { R_BLOCK, sp->index, 0 };
            add_finding(&c, where, 0, &bi, &t, v);
            res.host_to_blocks++;
        }
    }
    bb_blocks_unlock();
    if (out && c.n) {
        qsort(c.list, c.n, sizeof *c.list, cmp_finding);
        for (size_t i = 0; i < c.n && (int)i < max; i++)
            fprintf(out, "hibernate scan: %s +%llu points to %s (%u, e.g. 0x%llx)\n", c.list[i].where,
                    (unsigned long long)c.list[i].off, c.list[i].what, c.list[i].count,
                    (unsigned long long)c.list[i].example);
        if ((int)c.n > max)
            fprintf(out, "hibernate scan: ... and %zu more places\n", c.n - (size_t)max);
    }
    bb_host_free(c.list);
    bb_host_free(c.word);
    free_index(&bi);
    bb_host_free(tc.slots);
    return res;
}

/* ---- Comparing two snapshots ---- */

typedef struct {
    const uint8_t *buf;
    file_header h;
    const uint8_t *sect[NSAVED];
    uint64_t sect_size[NSAVED];
    const file_block **blocks;
    const uint8_t *regions;
    const file_reloc *relocs;
    const extern_ref *externs;
    const file_sym *syms;
    const file_cstr *cstrs;
    const file_type *types;
} snap_view;

static const char *view_str(const snap_view *v, uint32_t off)
{
    return pool_at(v->buf, &v->h, off);
}

/* What a relocation of a snapshot points to, in words, for comparing two
 * snapshots of different builds by name (empty for other kinds). */
static void reloc_name(const snap_view *v, const file_reloc *rl, char *out, size_t n)
{
    uint32_t tgt = rl->what & 0xFFFFFFu;
    int kind = (int)(rl->what >> 24);
    if (kind == R_SYM && tgt < v->h.nsyms)
        snprintf(out, n, "%s:%s", view_str(v, v->syms[tgt].file), view_str(v, v->syms[tgt].name));
    else if (kind == R_CSTR && tgt < v->h.ncstrs)
        snprintf(out, n, "%s", view_str(v, v->cstrs[tgt].text));
    else if (kind == R_EXTERN && tgt < v->h.nexterns)
        snprintf(out, n, "%s:%s", v->externs[tgt].lib, v->externs[tgt].sym);
    else
        *out = 0;
}

static bool view_of(const uint8_t *buf, size_t len, snap_view *v)
{
    memset(v, 0, sizeof *v);
    reader r = { buf, len };
    if (len < sizeof v->h)
        return false;
    memcpy(&v->h, buf, sizeof v->h);
    if (memcmp(v->h.magic, MAGIC, 8) || v->h.version != VERSION || v->h.total != len || v->h.nsections != NSAVED ||
        !span_ok(&r, v->h.relocs_off, v->h.nrelocs * sizeof(file_reloc)) ||
        !span_ok(&r, v->h.externs_off, v->h.nexterns * sizeof(extern_ref)) ||
        !span_ok(&r, v->h.syms_off, v->h.nsyms * sizeof(file_sym)) ||
        !span_ok(&r, v->h.cstrs_off, v->h.ncstrs * sizeof(file_cstr)) ||
        !span_ok(&r, v->h.types_off, v->h.ntypes * sizeof(file_type)) || !span_ok(&r, v->h.pool_off, v->h.pool_len) ||
        (v->h.pool_len && buf[v->h.pool_off + v->h.pool_len - 1]))
        return false;
    v->buf = buf;
    v->syms = (const file_sym *)(buf + v->h.syms_off);
    v->cstrs = (const file_cstr *)(buf + v->h.cstrs_off);
    v->types = (const file_type *)(buf + v->h.types_off);
    uint64_t at = v->h.sections_off;
    for (int i = 0; i < NSAVED; i++) {
        file_section fs;
        if (!span_ok(&r, at, sizeof fs))
            return false;
        memcpy(&fs, buf + at, sizeof fs);
        if (!span_ok(&r, at + sizeof fs, fs.size))
            return false;
        v->sect[i] = buf + at + sizeof fs;
        v->sect_size[i] = fs.size;
        at = pad8(at + sizeof fs + fs.size);
    }
    v->blocks = bb_host_malloc((v->h.nblocks ? v->h.nblocks : 1) * sizeof *v->blocks);
    if (!v->blocks)
        return false;
    at = v->h.blocks_off;
    for (uint64_t i = 0; i < v->h.nblocks; i++) {
        if (!span_ok(&r, at, sizeof(file_block)))
            return false;
        v->blocks[i] = (const file_block *)(buf + at);
        if (v->blocks[i]->type >= v->h.ntypes || !span_ok(&r, at + sizeof(file_block), v->blocks[i]->size))
            return false;
        at = pad8(at + sizeof(file_block) + v->blocks[i]->size);
    }
    v->regions = buf + v->h.regions_off;
    v->relocs = (const file_reloc *)(buf + v->h.relocs_off);
    v->externs = (const extern_ref *)(buf + v->h.externs_off);
    return true;
}

typedef struct {
    FILE *out;
    int max, n;
} diffs;

static void diff(diffs *d, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void diff(diffs *d, const char *fmt, ...)
{
    if (d->out && d->n < d->max) {
        va_list ap;
        va_start(ap, fmt);
        fputs("hibernate compare: ", d->out);
        vfprintf(d->out, fmt, ap);
        fputc('\n', d->out);
        va_end(ap);
    }
    d->n++;
}

static void describe_where(const snap_view *v, uint32_t where, uint64_t off, char *out, size_t n)
{
    if (where < NSAVED) {
        uint64_t o;
        char s[160];
        describe_static((uintptr_t)img.sect[where] + off, s, sizeof s, &o);
        snprintf(out, n, "%s +%llu", s, (unsigned long long)o);
        return;
    }
    const file_type *t = &v->types[v->blocks[where - NSAVED]->type];
    snprintf(out, n, "block %u (%s, %s:%u) +%llu", where - NSAVED, view_str(v, t->name), view_str(v, t->file), t->line,
             (unsigned long long)off);
}

static int cmp_reloc(const void *a, const void *b)
{
    const file_reloc *x = a, *y = b;
    if (x->where != y->where)
        return x->where < y->where ? -1 : 1;
    return x->off < y->off ? -1 : x->off > y->off;
}

/* The bytes of the fields of a layout at base that hold the host's own
 * values (the map's 'D' descriptors, 'X' and 'x' values of the host's:
 * readings of its clock, counts of its polls), set in mask: two
 * processes' snapshots differ there by nature. */
static void mark_host_values(uint32_t layout, size_t base, size_t limit, uint8_t *mask)
{
    if (layout >= hb_nlayouts)
        return;
    const hb_layout *l = &hb_layouts[layout];
    for (uint32_t r = 0; r < l->n; r++) {
        const hb_run *run = &hb_runs[l->first + r];
        size_t w = run->kind == 'X' ? 8 : run->kind == 'x' || run->kind == 'D' ? 4 : 0;
        for (uint32_t i = 0; w && i < run->count; i++)
            for (uint32_t j = 0; j < run->count2; j++) {
                size_t off = base + run->off + (size_t)i * run->stride + (size_t)j * run->stride2;
                if (off + w <= limit)
                    memset(mask + off, 1, w);
            }
    }
}

/* The first byte of n at x and y that differs and is not masked, or -1. */
static int first_difference(const uint8_t *x, const uint8_t *y, size_t n, const uint8_t *mask)
{
    for (size_t k = 0; k < n; k++)
        if (x[k] != y[k] && !(mask && mask[k]))
            return (int)k;
    return -1;
}

int hb_compare(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen, int flags, FILE *out, int max)
{
    char err[256];
    if (!ready(err, sizeof err))
        return 1;
    diffs d = { out, max, 0 };
    snap_view va, vb;
    if (!view_of(a, alen, &va) || !view_of(b, blen, &vb)) {
        diff(&d, "not two snapshots");
        bb_host_free(va.blocks);
        bb_host_free(vb.blocks);
        return d.n;
    }
    bool ignore_host = flags & HB_COMPARE_IGNORE_HOST;
    char where[256];
    for (int i = 0; i < NSAVED; i++) {
        if (va.sect_size[i] != vb.sect_size[i]) {
            diff(&d, "%s: %llu bytes, then %llu", saved_names[i], (unsigned long long)va.sect_size[i],
                 (unsigned long long)vb.sect_size[i]);
            continue;
        }
        /* The host's values in the statics, left out with the host's pointers. */
        uint8_t *mask = ignore_host ? bb_host_calloc(va.sect_size[i] + 8, 1) : NULL;
        uintptr_t lo = (uintptr_t)img.sect[i], hi = lo + img.sect_size[i];
        for (size_t k = 0; mask && k < nstatics; k++)
            if (statics[k].addr >= lo && statics[k].addr < hi && statics[k].layout)
                mark_host_values(statics[k].layout, statics[k].addr - lo, va.sect_size[i], mask);
        for (uint64_t o = 0; o + 8 <= va.sect_size[i]; o += 8) {
            int k = first_difference(va.sect[i] + o, vb.sect[i] + o, 8, mask ? mask + o : NULL);
            if (k < 0)
                continue;
            /* Named by the byte that differs: a word may hold two statics. */
            uint64_t x, y;
            memcpy(&x, va.sect[i] + o, 8);
            memcpy(&y, vb.sect[i] + o, 8);
            describe_where(&va, (uint32_t)i, o + (uint64_t)k, where, sizeof where);
            if (k)
                diff(&d, "%s: 0x%llx, then 0x%llx (the 8 bytes from %d before)", where, (unsigned long long)x,
                     (unsigned long long)y, k);
            else
                diff(&d, "%s: 0x%llx, then 0x%llx", where, (unsigned long long)x, (unsigned long long)y);
        }
        bb_host_free(mask);
    }
    if (va.h.nblocks != vb.h.nblocks)
        diff(&d, "%llu blocks, then %llu", (unsigned long long)va.h.nblocks, (unsigned long long)vb.h.nblocks);
    for (uint64_t i = 0; i < va.h.nblocks && i < vb.h.nblocks; i++) {
        const file_block *x = va.blocks[i], *y = vb.blocks[i];
        const file_type *tx = &va.types[x->type], *ty = &vb.types[y->type];
        if (x->size != y->size || tx->size != ty->size || tx->form != ty->form ||
            strcmp(view_str(&va, tx->name), view_str(&vb, ty->name)) ||
            strcmp(view_str(&va, tx->file), view_str(&vb, ty->file))) {
            describe_where(&va, (uint32_t)(NSAVED + i), 0, where, sizeof where);
            diff(&d, "%s: %u bytes, then %u bytes of another type or size", where, x->size, y->size);
            continue;
        }
        const uint8_t *px = (const uint8_t *)(x + 1), *py = (const uint8_t *)(y + 1);
        uint8_t *mask = NULL;
        const hb_typemap *m = ignore_host && (tx->form == BB_FORM_ARRAY || tx->form == BB_FORM_HEAD)
                                  ? map_type(view_str(&va, tx->file), view_str(&va, tx->name)) : NULL;
        if (m && m->size == tx->size && m->size && (mask = bb_host_calloc(x->size + 8, 1))) {
            if (tx->form == BB_FORM_HEAD)
                mark_host_values(m->layout, 0, x->size, mask);
            else
                for (size_t e = 0; e + m->size <= x->size; e += m->size)
                    mark_host_values(m->layout, e, x->size, mask);
        }
        for (uint32_t o = 0; o < x->size; o += 8) {
            uint32_t n = x->size - o < 8 ? x->size - o : 8;
            int k = first_difference(px + o, py + o, n, mask ? mask + o : NULL);
            if (k >= 0) {
                describe_where(&va, (uint32_t)(NSAVED + i), o + (uint32_t)k, where, sizeof where);
                diff(&d, "%s differs", where);
            }
        }
        bb_host_free(mask);
    }
    /* The regions' runs and bytes. */
    if (va.h.nregions != vb.h.nregions) {
        diff(&d, "%llu regions, then %llu", (unsigned long long)va.h.nregions, (unsigned long long)vb.h.nregions);
    } else {
        const uint8_t *ra = va.regions, *rb = vb.regions;
        for (uint64_t i = 0; i < va.h.nregions; i++) {
            file_region fa, fb;
            memcpy(&fa, ra, sizeof fa);
            memcpy(&fb, rb, sizeof fb);
            const file_run *runs_a = (const file_run *)(ra + sizeof fa), *runs_b = (const file_run *)(rb + sizeof fb);
            const uint8_t *da = ra + sizeof fa + fa.nruns * sizeof(file_run);
            const uint8_t *db = rb + sizeof fb + fb.nruns * sizeof(file_run);
            if (fa.nruns != fb.nruns || memcmp(runs_a, runs_b, fa.nruns * sizeof(file_run))) {
                diff(&d, "region %.16s: other pages hold anything", fa.name);
                break;
            }
            for (uint64_t k = 0; k < fa.nruns; k++) {
                for (uint64_t o = 0; o < runs_a[k].len; o += 4096) {
                    size_t n = runs_a[k].len - o < 4096 ? runs_a[k].len - o : 4096;
                    /* The guest's Time is the host's date (a resume moves it on). */
                    uint64_t at = runs_a[k].off + o;
                    uint8_t page_mask[4096];
                    bool masked = ignore_host && !strncmp(fa.name, "guest", sizeof fa.name) && LM_Time >= at &&
                                  LM_Time + 4 <= at + n;
                    if (masked) {
                        memset(page_mask, 0, sizeof page_mask);
                        memset(page_mask + (LM_Time - at), 1, 4);
                    }
                    int first = first_difference(da + o, db + o, n, masked ? page_mask : NULL);
                    if (first < 0)
                        continue;
                    size_t last = n - 1;
                    while (last > (size_t)first && (da[o + last] == db[o + last] || (masked && page_mask[last])))
                        last--;
                    diff(&d, "region %.16s: the page at 0x%llx differs (bytes 0x%llx to 0x%llx)", fa.name,
                         (unsigned long long)(runs_a[k].off + o), (unsigned long long)(runs_a[k].off + o + first),
                         (unsigned long long)(runs_a[k].off + o + last));
                }
                da += pad8(runs_a[k].len);
                db += pad8(runs_a[k].len);
            }
            ra = da;
            rb = db;
        }
    }
    /* The relocations, by place. */
    file_reloc *xa = bb_host_malloc((va.h.nrelocs + 1) * sizeof *xa);
    file_reloc *xb = bb_host_malloc((vb.h.nrelocs + 1) * sizeof *xb);
    if (xa && xb) {
        memcpy(xa, va.relocs, va.h.nrelocs * sizeof *xa);
        memcpy(xb, vb.relocs, vb.h.nrelocs * sizeof *xb);
        qsort(xa, va.h.nrelocs, sizeof *xa, cmp_reloc);
        qsort(xb, vb.h.nrelocs, sizeof *xb, cmp_reloc);
        uint64_t i = 0, j = 0;
        while (i < va.h.nrelocs || j < vb.h.nrelocs) {
            int c = i == va.h.nrelocs ? 1 : j == vb.h.nrelocs ? -1 : cmp_reloc(&xa[i], &xb[j]);
            const file_reloc *ra = c <= 0 ? &xa[i] : NULL, *rb = c >= 0 ? &xb[j] : NULL;
            const file_reloc *any = ra ? ra : rb;
            bool host = (ra && ra->what >> 24 == R_HOST) || (rb && rb->what >> 24 == R_HOST);
            /* By name where it is one: two snapshots number the binary's
             * symbols each in its own order, and may be two builds'. */
            bool same = ra && rb && ra->what >> 24 == rb->what >> 24 && ra->value == rb->value;
            if (same) {
                char na[512], nb[512];
                reloc_name(&va, ra, na, sizeof na);
                reloc_name(&vb, rb, nb, sizeof nb);
                same = *na ? !strcmp(na, nb) : ra->what == rb->what;
            }
            if (!same && !(host && ignore_host)) {
                describe_where(any->where < NSAVED || any->where - NSAVED < va.h.nblocks ? &va : &vb, any->where,
                               any->off, where, sizeof where);
                diff(&d, "%s: pointer %s", where, !ra ? "only after" : !rb ? "only before" : "to another place");
            }
            if (c <= 0)
                i++;
            if (c >= 0)
                j++;
        }
    } else {
        diff(&d, "out of memory");
    }
    bb_host_free(xa);
    bb_host_free(xb);
    bb_host_free(va.blocks);
    bb_host_free(vb.blocks);
    if (out && d.n > max)
        fprintf(out, "hibernate compare: ... %d differences in all\n", d.n);
    return d.n;
}

/* ---- The map against the binary ---- */

typedef struct {
    FILE *out;
    int problems;
} check_ctx;

static void check_one(void *vctx, const char *file, const char *name, uintptr_t addr)
{
    check_ctx *c = vctx;
    size_t n = strlen(name);
    if (n > 8 && !strcmp(name + n - 8, ".bb_tag_")) {
        /* A tag's descriptor: its type must be in the map, as large. */
        const bb_type *t = (const bb_type *)addr;
        if (t->form != BB_FORM_ARRAY && t->form != BB_FORM_HEAD)
            return;
        const hb_typemap *m = map_type(t->file, t->name);
        if (!m || m->size != t->size) {
            c->problems++;
            if (c->out)
                fprintf(c->out, "hibernate map: %s: the type %s (%u bytes) is %s\n", t->file, t->name, t->size,
                        m ? "another size in the map" : "not in the map");
        }
        return;
    }
    if (!in_saved(addr, NULL) || map_static(file, name))
        return;
    c->problems++;
    if (c->out)
        fprintf(c->out, "hibernate map: %s: %s is saved but not in the map\n", file, name);
}

int hb_check_map(FILE *out)
{
    char err[256];
    if (!ready(err, sizeof err)) {
        if (out)
            fprintf(out, "hibernate map: %s\n", err);
        return 1;
    }
    check_ctx c = { out, 0 };
    if (!each_debug_symbol(check_one, &c, err, sizeof err)) {
        if (out)
            fprintf(out, "hibernate map: %s\n", err);
        return 1;
    }
    if (c.problems && out)
        fprintf(out, "hibernate map: %d differences: run scripts/state-map.py after a build\n", c.problems);
    return c.problems;
}

/* ---- For tests and tools ---- */

void hb_explain_manifest(const char *old, size_t len, char *why, size_t whylen)
{
    char err[128];
    if (!ready(err, sizeof err)) {
        snprintf(why, whylen, "%s", err);
        return;
    }
    manifest_explain(old, len, why, whylen);
}

bool hb_symbolize(const void *p, char *out, size_t n)
{
    char err[128];
    if (!ready(err, sizeof err) || !symbols_ready(err, sizeof err))
        return false;
    uintptr_t v = (uintptr_t)p;
    uint32_t cs = cstring_section(v);
    if (cs) {
        const char *start = p;
        while (start > (const char *)img.sects[cs].lo && start[-1])
            start--;
        snprintf(out, n, "\"%s\"+%llu", start, (unsigned long long)(v - (uintptr_t)start));
        return true;
    }
    const isym *s = isym_at(v);
    if (!s)
        return false;
    snprintf(out, n, "%s%s%s+%llu", s->file, *s->file ? ":" : "", s->name, (unsigned long long)(v - s->addr));
    return true;
}

const void *hb_resolve_symbol(const char *file, const char *name, uint64_t off)
{
    char err[128];
    if (!ready(err, sizeof err) || !symbols_ready(err, sizeof err))
        return NULL;
    const isym *s = isym_named(file ? file : "", name);
    return s && off <= s->size ? (const void *)(s->addr + off) : NULL;
}
