/*
 * Code Fragment Manager: PowerPC applications (PEF containers).
 *
 * An application's 'cfrg' 0 names its PowerPC fragment (usually the whole
 * data fork).  Its sections are instantiated in the application heap
 * (pattern-initialized data unpacked), imports are bound to transition
 * vectors, the loader's relocations are run, and the fragment's init
 * routine and then main run on the PowerPC interpreter.
 *
 * Imports from InterfaceLib (and the other system libraries) bind to the
 * generated glue (ppc_glue_gen.c) or to the natives here; each becomes a
 * transition vector whose code is one NATIVE word.  An import nobody
 * provides binds to a stub that stops with its name when called.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "forks.h"
#include "machine.h"
#include "ppc_bridge.h"
#include "tb_internal.h"
#include "trap.h"

ppc_cpu g_ppc;
bool g_ppc_mode;

/* ---------------------------------------------------------------------- */
/* Natives callable from PowerPC code */

#define MAX_PPC_NATIVES 4096

typedef struct {
    void (*fn)(ppc_cpu *c);
    char name[64];   /* for the unimplemented stub's message */
    gaddr tvector;   /* the transition vector made for it, once */
} ppc_native;

static ppc_native natives[MAX_PPC_NATIVES];
static int nnatives;
static gaddr glue_area; /* code words and transition vectors */
static uint32_t glue_used, glue_size;

static gaddr glue_alloc(uint32_t n)
{
    n = (n + 7) & ~7u;
    if (!glue_area || glue_used + n > glue_size) {
        glue_size = 0x10000;
        glue_area = tb_NewPtr((int32_t)glue_size, true, true);
        glue_used = 0;
        if (!glue_area)
            return 0;
    }
    gaddr p = glue_area + glue_used;
    glue_used += n;
    return p;
}

static void unimplemented_import(ppc_cpu *c);

static void dispatch_native(ppc_cpu *c, uint32_t index)
{
    if (index >= (uint32_t)nnatives) {
        ppc_halt(c, "PowerPC native %u: no such native", index);
        return;
    }
    if (natives[index].fn == unimplemented_import) {
        ppc_halt(c, "unimplemented: %s (PowerPC import)", natives[index].name);
        return;
    }
    trap_set_current_word(0); /* not a trap: no HFS or async bits */
    LOGF(LOG_TRAPS, "%08X PowerPC %s\n", c->lr, natives[index].name);
    natives[index].fn(c);
}

static gaddr native_tvector(void (*fn)(ppc_cpu *), const char *name);

/* A transition vector (code, TOC) whose code calls fn. */
gaddr cfm_native_tvector(void (*fn)(ppc_cpu *), const char *name) { return native_tvector(fn, name); }

static gaddr native_tvector(void (*fn)(ppc_cpu *), const char *name)
{
    for (int i = 0; i < nnatives; i++)
        if (natives[i].fn == fn && fn != unimplemented_import && natives[i].tvector)
            return natives[i].tvector;
    if (nnatives >= MAX_PPC_NATIVES)
        return 0;
    int i = nnatives++;
    natives[i].fn = fn;
    snprintf(natives[i].name, sizeof natives[i].name, "%s", name);
    gaddr code = glue_alloc(4), tv = glue_alloc(8);
    if (!code || !tv)
        return 0;
    wr32(code, PPC_NATIVE_OP | (uint32_t)i);
    wr32(tv, code);
    wr32(tv + 4, 0);
    natives[i].tvector = tv;
    return tv;
}

static void unimplemented_import(ppc_cpu *c) { (void)c; }

/* ---- Hand-written exports: calls whose 68k forms are raw traps ---- */

static void ppc_ExitToShell(ppc_cpu *c)
{
    rm_update_all();
    cpu_halt(g_cpu, "ExitToShell");
    ppc_halt(c, "ExitToShell");
}

static void ppc_Debugger(ppc_cpu *c) { c->pc = c->lr; }

static void ppc_SysBeep(ppc_cpu *c)
{
    tb_SysBeep((int16_t)c->gpr[3]);
    c->pc = c->lr;
}

static const ppc_export manual_exports[] = {
    { "Debugger", ppc_Debugger },
    { "DebugStr", ppc_Debugger },
    { "ExitToShell", ppc_ExitToShell },
    { "SysBeep", ppc_SysBeep },
};

static void (*export_fn(const char *name))(ppc_cpu *)
{
    for (unsigned i = 0; i < sizeof manual_exports / sizeof manual_exports[0]; i++)
        if (!strcmp(manual_exports[i].name, name))
            return manual_exports[i].fn;
    for (unsigned i = 0; i < isp_export_count; i++)
        if (!strcmp(isp_exports[i].name, name))
            return isp_exports[i].fn;
    for (unsigned i = 0; i < dsp_export_count; i++)
        if (!strcmp(dsp_exports[i].name, name))
            return dsp_exports[i].fn;
    for (unsigned i = 0; i < lib_export_count; i++)
        if (!strcmp(lib_exports[i].name, name))
            return lib_exports[i].fn;
    for (unsigned i = 0; i < cfm_export_count; i++)
        if (!strcmp(cfm_exports[i].name, name))
            return cfm_exports[i].fn;
    for (unsigned i = 0; i < mm_export_count; i++)
        if (!strcmp(mm_exports[i].name, name))
            return mm_exports[i].fn;
    for (unsigned i = 0; i < ppc_glue_export_count; i++)
        if (!strcmp(ppc_glue_exports[i].name, name))
            return ppc_glue_exports[i].fn;
    unsigned lo = 0, hi = ppc_export_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        int c = strcmp(ppc_exports[mid].name, name);
        if (!c)
            return ppc_exports[mid].fn;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

void (*cfm_export_fn(const char *name))(ppc_cpu *) { return export_fn(name); }

/* ---------------------------------------------------------------------- */
/* PEF */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

enum { kCode = 0, kUnpackedData, kPatternData, kConstant, kLoader, kDebug, kExecData, kException, kTraceback };

typedef struct {
    gaddr base;
    uint32_t size;
    int kind;
    uint32_t delta; /* base less the section's default address: what relocations add */
} section;

/* Pattern-initialized data's variable-length count: 7 bits a byte, the
 * high bit set on all but the last. */
static uint32_t pat_arg(const uint8_t **p, const uint8_t *end)
{
    uint32_t v = 0;
    while (*p < end) {
        uint8_t b = *(*p)++;
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80))
            break;
    }
    return v;
}

static bool unpack_pattern(const uint8_t *p, const uint8_t *end, gaddr out, uint32_t outsize)
{
    uint32_t at = 0;
#define PUT(byte) do { if (at < outsize) wr8(out + at, (byte)); at++; } while (0)
    while (p < end) {
        uint8_t b = *p++;
        int opcode = b >> 5;
        uint32_t count = b & 31;
        if (!count)
            count = pat_arg(&p, end);
        switch (opcode) {
        case 0: /* zero */
            at += count;
            break;
        case 1: /* block copy */
            for (uint32_t i = 0; i < count && p < end; i++)
                PUT(*p++);
            break;
        case 2: { /* repeated block: count bytes, repeated repeatCount + 1 times */
            uint32_t repeat = pat_arg(&p, end) + 1;
            for (uint32_t r = 0; r < repeat; r++)
                for (uint32_t i = 0; i < count; i++)
                    PUT(p[i]);
            p += count;
            break;
        }
        case 3: /* interleave a repeated common block with custom blocks */
        case 4: { /* the same, the common block being zeros */
            uint32_t common = count, custom = pat_arg(&p, end), repeat = pat_arg(&p, end);
            const uint8_t *cb = p;
            if (opcode == 3)
                p += common;
            for (uint32_t r = 0; r <= repeat; r++) {
                for (uint32_t i = 0; i < common; i++)
                    PUT(opcode == 3 ? cb[i] : 0);
                if (r == repeat)
                    break;
                for (uint32_t i = 0; i < custom && p < end; i++)
                    PUT(*p++);
            }
            break;
        }
        default:
            return false;
        }
    }
#undef PUT
    return true;
}

/* Relocations: the loader's instructions for one section, run as a little
 * machine (Inside Macintosh: PowerPC System Software, 8-41).  The repeat
 * instructions run the preceding instructions again, so the interpreter
 * works on ranges of the instruction stream. */
typedef struct {
    const uint8_t *instr;
    const section *sections;
    int nsections;
    const gaddr *imports;
    uint32_t nimports;
    gaddr target_base, reloc, sect_c, sect_d;
    uint32_t import_index;
    char *err;
    size_t errlen;
    int depth;
} reloc_state;

static gaddr import_at(reloc_state *s, uint32_t index) { return index < s->nimports ? s->imports[index] : 0; }
/* What a relocation by a section adds: its address less its default one. */
static gaddr section_base(reloc_state *s, uint32_t index)
{
    return (int)index < s->nsections ? s->sections[index].delta : 0;
}

static bool reloc_run(reloc_state *s, uint32_t from, uint32_t to)
{
#define ADD(v) do { wr32(s->reloc, rd32(s->reloc) + (v)); s->reloc += 4; } while (0)
    uint32_t i = from;
    while (i < to) {
        uint32_t here = i;
        uint16_t w = be16(s->instr + i * 2);
        i++;
        if ((w & 0xC000) == 0x0000) { /* RelocBySectDWithSkip */
            s->reloc += ((w >> 6) & 0xFF) * 4;
            for (uint32_t k = 0, n = w & 0x3F; k < n; k++)
                ADD(s->sect_d);
        } else if ((w & 0xE000) == 0x4000) { /* the run group */
            uint32_t sub = (w >> 9) & 0xF, run = (w & 0x1FF) + 1;
            for (uint32_t k = 0; k < run; k++) {
                switch (sub) {
                case 0: ADD(s->sect_c); break;                             /* RelocBySectC */
                case 1: ADD(s->sect_d); break;                             /* RelocBySectD */
                case 2: ADD(s->sect_c); ADD(s->sect_d); s->reloc += 4; break; /* RelocTVector12 */
                case 3: ADD(s->sect_c); ADD(s->sect_d); break;             /* RelocTVector8 */
                case 4: ADD(s->sect_d); s->reloc += 4; break;              /* RelocVTable8 */
                case 5: ADD(import_at(s, s->import_index)); s->import_index++; break; /* RelocImportRun */
                default:
                    snprintf(s->err, s->errlen, "relocation run subopcode %u", sub);
                    return false;
                }
            }
        } else if ((w & 0xE000) == 0x6000) { /* the small index group */
            uint32_t sub = (w >> 9) & 0xF, index = w & 0x1FF;
            switch (sub) {
            case 0: ADD(import_at(s, index)); s->import_index = index + 1; break; /* RelocSmByImport */
            case 1: s->sect_c = section_base(s, index); break;                  /* RelocSmSetSectC */
            case 2: s->sect_d = section_base(s, index); break;                  /* RelocSmSetSectD */
            case 3: ADD(section_base(s, index)); break;                         /* RelocSmBySection */
            default:
                snprintf(s->err, s->errlen, "relocation index subopcode %u", sub);
                return false;
            }
        } else if ((w & 0xF000) == 0x8000) { /* RelocIncrPosition */
            s->reloc += (w & 0x0FFF) + 1;
        } else if ((w & 0xF000) == 0x9000 || (w & 0xFC00) == 0xB000) {
            /* RelocSmRepeat / RelocLgRepeat: the preceding `blocks` halfwords,
             * `times` more times. */
            uint32_t blocks, times;
            if ((w & 0xF000) == 0x9000) {
                blocks = ((w >> 8) & 0xF) + 1;
                times = (w & 0xFF) + 1;
            } else {
                blocks = ((w >> 6) & 0xF) + 1;
                times = ((uint32_t)(w & 0x3F) << 16) | be16(s->instr + i * 2);
                i++;
            }
            if (blocks > here || s->depth > 8) {
                snprintf(s->err, s->errlen, "relocation repeat out of range");
                return false;
            }
            s->depth++;
            for (uint32_t t = 0; t < times; t++)
                if (!reloc_run(s, here - blocks, here))
                    return false;
            s->depth--;
        } else if ((w & 0xFC00) == 0xA000) { /* RelocSetPosition */
            uint32_t off = ((uint32_t)(w & 0x3FF) << 16) | be16(s->instr + i * 2);
            i++;
            s->reloc = s->target_base + off;
        } else if ((w & 0xFC00) == 0xA400) { /* RelocLgByImport */
            uint32_t index = ((uint32_t)(w & 0x3FF) << 16) | be16(s->instr + i * 2);
            i++;
            ADD(import_at(s, index));
            s->import_index = index + 1;
        } else if ((w & 0xFC00) == 0xB400) { /* RelocLgSetOrBySection */
            uint32_t sub = (w >> 6) & 0xF;
            uint32_t index = ((uint32_t)(w & 0x3F) << 16) | be16(s->instr + i * 2);
            i++;
            gaddr base = section_base(s, index);
            if (sub == 0)
                ADD(base);
            else if (sub == 1)
                s->sect_c = base;
            else if (sub == 2)
                s->sect_d = base;
        } else {
            snprintf(s->err, s->errlen, "relocation opcode %04X", w);
            return false;
        }
    }
#undef ADD
    return true;
}

/* The imports nothing provides, "Library:symbol" separated by spaces, for
 * the census; data imports are marked with a star. */
static char *missing_imports;
static size_t missing_len, missing_count;

static void note_missing(const char *lib, const char *sym, bool data)
{
    size_t n = strlen(lib) + strlen(sym) + 3;
    char *grown = realloc(missing_imports, missing_len + n + 1);
    if (!grown)
        return;
    missing_imports = grown;
    snprintf(missing_imports + missing_len, n + 1, "%s%s:%s%s", missing_len ? " " : "", lib, sym, data ? "*" : "");
    missing_len += strlen(missing_imports + missing_len);
    missing_count++;
}

unsigned machine_ppc_missing(const char **names)
{
    *names = missing_imports ? missing_imports : "";
    return (unsigned)missing_count;
}

/* ---------------------------------------------------------------------- */
/* Fragments */

enum { kImportLibraryCFrag = 0, kApplicationCFrag = 1 };
enum { kMemoryCFragLocator = 0, kDataForkCFragLocator = 1, kResourceCFragLocator = 2 };
enum {
    cfragNoSymbolErr = -2802,
    cfragNoLibraryErr = -2804,
    cfragFragmentFormatErr = -2806,
    cfragConnectionIDErr = -2801,
};
enum { kCodeCFragSymbol = 0, kDataCFragSymbol = 1, kTVectorCFragSymbol = 2 };

typedef struct {
    char *name;
    gaddr addr;
    uint8_t cls;
} frag_export;

typedef struct {
    char name[64];       /* the fragment's name (MacRoman), "" for the application */
    char path[1024];     /* the file it came from */
    uint32_t offset, length;
    gaddr main_tv, init_tv, term_tv;
    frag_export *exports;
    uint32_t nexports;
    bool native;         /* a library we provide (InterfaceLib ...): no PEF */
    gaddr memory;        /* a code resource's container, prepared from memory */
    bool loading;
} fragment;

#define MAX_FRAGMENTS 64
static fragment frags[MAX_FRAGMENTS];
static int nfrags;
static char app_dir[1024], app_file[1024];
/* Init routines still to run at launch, in order (fragment indices). */
static int init_queue[MAX_FRAGMENTS];
static int ninit, init_next;

/* Libraries whose exports are ours. */
static const char *const native_libraries[] = {
    "InterfaceLib", "MathLib", "StdCLib", "QuickTimeLib", "DragLib", "ThreadsLib", "AppearanceLib",
    "PrivateInterfaceLib", "ObjectSupportLib", "AppleScriptLib", "TranslationLib", "ControlStripLib",
    "WindowsLib", "MenusLib", "ControlsLib", "DialogsLib", "NavigationLib", "SoundLib", "SpeechLib",
    "InternetConfigLib", "ICAp;InternetConfigLib", "CodeFragmentMgr", "DisplayLib", "DrawSprocketLib", "InputSprocketLib",
};

static bool core_library(const char *name)
{
    return !strcmp(name, "InterfaceLib") || !strcmp(name, "MathLib") || !strcmp(name, "StdCLib");
}

static bool is_native_library(const char *name)
{
    for (unsigned i = 0; i < sizeof native_libraries / sizeof native_libraries[0]; i++)
        if (!strcmp(native_libraries[i], name))
            return true;
    return false;
}

static void pname_to_c(const uint8_t *p, char *out, size_t outsize)
{
    size_t n = p[0] < outsize - 1 ? p[0] : outsize - 1;
    memcpy(out, p + 1, n);
    out[n] = 0;
}

/* Data imports we provide: a block of zeros of their own each (the C
 * library's globals, mostly).  Named, so every importer gets the same. */
static struct { char name[64]; gaddr addr; } data_blocks[128];
static int ndata_blocks;

static gaddr data_import(const char *name)
{
    for (int i = 0; i < ndata_blocks; i++)
        if (!strcmp(data_blocks[i].name, name))
            return data_blocks[i].addr;
    gaddr a = ppc_data_export(name);
    if (!a)
        a = tb_NewPtr(256, false, true);
    if (ndata_blocks < (int)(sizeof data_blocks / sizeof data_blocks[0])) {
        snprintf(data_blocks[ndata_blocks].name, sizeof data_blocks[0].name, "%s", name);
        data_blocks[ndata_blocks++].addr = a;
    }
    return a;
}

static bool frag_find_export(const fragment *f, const char *name, gaddr *addr, uint8_t *cls)
{
    for (uint32_t i = 0; i < f->nexports; i++)
        if (!strcmp(f->exports[i].name, name)) {
            *addr = f->exports[i].addr;
            if (cls)
                *cls = f->exports[i].cls;
            return true;
        }
    return false;
}

static int library_named(const char *name, char *err, size_t errlen);

/* Load a PEF container into fragment f: sections, imports (loading the
 * libraries it needs), relocations, exports. */
static bool pef_load(const uint8_t *pef, size_t len, fragment *f, char *err, size_t errlen)
{
    if (len < 40 || memcmp(pef, "Joy!peff", 8)) {
        snprintf(err, errlen, "not a PEF container");
        return false;
    }
    if (memcmp(pef + 8, "pwpc", 4)) {
        snprintf(err, errlen, "PEF for %.4s, not PowerPC", (const char *)pef + 8);
        return false;
    }
    int nsect = be16(pef + 32), ninst = be16(pef + 34);
    if (40 + (size_t)nsect * 28 > len) {
        snprintf(err, errlen, "truncated PEF");
        return false;
    }
    section *sections = calloc((size_t)(ninst > 0 ? ninst : 1), sizeof *sections);
    const uint8_t *loader = NULL;
    uint32_t loader_len = 0;
    for (int s = 0; s < nsect; s++) {
        const uint8_t *h = pef + 40 + s * 28;
        uint32_t total = be32(h + 8), unpacked = be32(h + 12), packed = be32(h + 16), off = be32(h + 20);
        int kind = h[24];
        if (off + (size_t)packed > len) {
            snprintf(err, errlen, "section %d beyond the container", s);
            free(sections);
            return false;
        }
        if (kind == kLoader) {
            loader = pef + off;
            loader_len = packed;
            continue;
        }
        if (s >= ninst)
            continue;
        gaddr base = tb_NewPtr((int32_t)(total + 16), false, true);
        if (!base) {
            snprintf(err, errlen, "no memory for section %d (%u bytes)", s, total);
            free(sections);
            return false;
        }
        base = (base + 15) & ~15u;
        gmemset(base, 0, total);
        sections[s] = (section){ base, total, kind, base - be32(h + 4) };
        LOGF(LOG_SEG, "cfm: section %d (kind %d) at %08X, %u bytes\n", s, kind, base, total);
        if (kind == kPatternData)
            unpack_pattern(pef + off, pef + off + packed, base, unpacked);
        else
            gput(base, pef + off, packed < total ? packed : total);
    }
    if (!loader || loader_len < 56) {
        snprintf(err, errlen, "no loader section");
        free(sections);
        return false;
    }
    int32_t main_sect = (int32_t)be32(loader), init_sect = (int32_t)be32(loader + 8);
    int32_t term_sect = (int32_t)be32(loader + 16);
    uint32_t main_off = be32(loader + 4), init_off = be32(loader + 12), term_off = be32(loader + 20);
    uint32_t nlibs = be32(loader + 24), nsyms = be32(loader + 28), nreloc = be32(loader + 32);
    uint32_t reloc_off = be32(loader + 36), strings = be32(loader + 40);
    uint32_t hash_off = be32(loader + 44), hash_power = be32(loader + 48), nexports = be32(loader + 52);

    /* Imports: an address each (a transition vector or data), or 0 for
     * missing weak ones. */
    gaddr *imports = calloc(nsyms ? nsyms : 1, sizeof *imports);
    const uint8_t *libs = loader + 56, *syms = libs + nlibs * 24;
    for (uint32_t l = 0; l < nlibs; l++) {
        const uint8_t *lib = libs + l * 24;
        const char *lname = (const char *)loader + strings + be32(lib);
        uint32_t count = be32(lib + 12), first = be32(lib + 16);
        bool weak_lib = (lib[20] & 0x40) != 0;
        char lerr[256];
        int li = library_named(lname, lerr, sizeof lerr);
        const fragment *from = li >= 0 && !frags[li].native ? &frags[li] : NULL;
        if (li < 0)
            LOGF(weak_lib ? LOG_SEG : LOG_UNIMPL, "cfm: library %s: %s\n", lname, lerr);
        for (uint32_t k = first; k < first + count && k < nsyms; k++) {
            uint32_t w = be32(syms + k * 4);
            const char *sname = (const char *)loader + strings + (w & 0xFFFFFF);
            bool weak = (w >> 24) & 0x80;
            int cls = (w >> 24) & 0x0F;
            gaddr addr = 0;
            if (from) {
                if (frag_find_export(from, sname, &addr, NULL)) {
                    imports[k] = addr;
                    continue;
                }
            } else if (li >= 0 || !weak_lib) {
                void (*fn)(ppc_cpu *) = cls == kTVectorCFragSymbol || cls == kCodeCFragSymbol ? export_fn(sname) : NULL;
                if (fn) {
                    imports[k] = native_tvector(fn, sname);
                    continue;
                }
                if (cls == kDataCFragSymbol && li >= 0 && (addr = ppc_data_export(sname)) != 0) {
                    imports[k] = data_import(sname);
                    continue;
                }
            }
            /* A weak symbol, or one from a weakly linked library that isn't
             * there, is 0.  But InterfaceLib, MathLib and StdCLib are always
             * there with everything Mac OS 9 had in them, so what they lack
             * here is ours to add: a stub that says so. */
            if ((weak || weak_lib) && (li < 0 || !core_library(lname))) {
                imports[k] = 0;
                LOGF(LOG_SEG, "cfm: weak %s from %s: not there\n", sname, lname);
            } else if (cls == kDataCFragSymbol) {
                imports[k] = data_import(sname);
                LOGF(LOG_UNIMPL, "cfm: data %s from %s not provided\n", sname, lname);
                note_missing(lname, sname, true);
            } else {
                imports[k] = native_tvector(unimplemented_import, sname);
                LOGF(LOG_UNIMPL, "cfm: %s from %s not provided\n", sname, lname);
                note_missing(lname, sname, false);
            }
        }
    }
    /* Relocations, section by section. */
    const uint8_t *rhead = syms + nsyms * 4;
    for (uint32_t r = 0; r < nreloc; r++) {
        const uint8_t *rh = rhead + r * 12;
        int target = be16(rh);
        uint32_t count = be32(rh + 4), first = be32(rh + 8);
        if (target >= ninst || !sections[target].base)
            continue;
        reloc_state st = {
            .instr = loader + reloc_off + first,
            .sections = sections,
            .nsections = ninst,
            .imports = imports,
            .nimports = nsyms,
            .target_base = sections[target].base,
            .reloc = sections[target].base,
            .sect_c = ninst > 0 ? sections[0].delta : 0,
            .sect_d = ninst > 1 ? sections[1].delta : 0,
            .err = err,
            .errlen = errlen,
        };
        if (!reloc_run(&st, 0, count)) {
            free(imports);
            free(sections);
            return false;
        }
    }
    /* Exports: the key table gives each name's length, the symbol table
     * its class, name and place. */
    const uint8_t *keys = loader + hash_off + (4u << hash_power);
    const uint8_t *esyms = keys + nexports * 4;
    f->exports = calloc(nexports ? nexports : 1, sizeof *f->exports);
    f->nexports = 0;
    for (uint32_t i = 0; i < nexports; i++) {
        uint32_t namelen = be32(keys + i * 4) >> 16;
        const uint8_t *e = esyms + i * 10;
        uint32_t cn = be32(e), value = be32(e + 4);
        int16_t sect = (int16_t)be16(e + 8);
        gaddr addr;
        if (sect >= 0 && sect < ninst)
            addr = sections[sect].base + value;
        else if (sect == -2)
            addr = value; /* absolute */
        else if (sect == -3 && value < nsyms)
            addr = imports[value]; /* re-exported import */
        else
            continue;
        frag_export *x = &f->exports[f->nexports++];
        x->name = strndup((const char *)loader + strings + (cn & 0xFFFFFF), namelen);
        x->addr = addr;
        x->cls = (uint8_t)(cn >> 24) & 0x0F;
    }
    f->main_tv = main_sect >= 0 && main_sect < ninst ? sections[main_sect].base + main_off : 0;
    f->init_tv = init_sect >= 0 && init_sect < ninst ? sections[init_sect].base + init_off : 0;
    f->term_tv = term_sect >= 0 && term_sect < ninst ? sections[term_sect].base + term_off : 0;
    free(imports);
    free(sections);
    return true;
}

/* A cfrg 0 resource's members: calls fn for each PowerPC one until it
 * returns true. */
typedef struct {
    int usage, where;
    uint32_t offset, length;
    uint32_t rtype;
    int16_t rid;
    char name[64];
} cfrg_member;

static bool cfrg_each(const uint8_t *p, size_t len, bool (*fn)(const cfrg_member *, void *), void *ctx)
{
    if (len < 32)
        return false;
    uint32_t n = be32(p + 28);
    size_t at = 32;
    for (uint32_t i = 0; i < n && at + 43 <= len; i++) {
        const uint8_t *m = p + at;
        uint16_t size = be16(m + 40);
        if (be32(m) == 0x70777063u /* pwpc */) {
            /* A fragment in a resource has the resource's type and ID
             * where one in the data fork has its offset and length. */
            cfrg_member cm = { .usage = m[22], .where = m[23], .offset = be32(m + 24), .length = be32(m + 28),
                               .rtype = be32(m + 24), .rid = (int16_t)be32(m + 28) };
            pname_to_c(m + 42, cm.name, sizeof cm.name);
            if (fn(&cm, ctx))
                return true;
        }
        if (!size)
            break;
        at += size;
    }
    return false;
}

/* The bytes of a fragment in a file (host memory, malloc'd). */
static uint8_t *member_bytes(const mac_file *mf, const cfrg_member *m, size_t *len)
{
    const uint8_t *src = NULL;
    size_t avail = 0;
    if (m->where == kDataForkCFragLocator) {
        if (m->offset > mf->data_len)
            return NULL;
        src = mf->data + m->offset;
        avail = mf->data_len - m->offset;
    } else if (m->where == kResourceCFragLocator) {
        rsrc_index idx;
        char err[128];
        if (!mf->rsrc || !rsrc_parse(mf->rsrc, mf->rsrc_len, &idx, err, sizeof err))
            return NULL;
        for (size_t i = 0; i < idx.count && !src; i++)
            if (idx.entries[i].type == m->rtype && idx.entries[i].id == m->rid) {
                src = rsrc_data(mf->rsrc, &idx, &idx.entries[i]);
                avail = idx.entries[i].data_len;
            }
        rsrc_index_free(&idx);
        if (!src)
            return NULL;
        uint8_t *plain;
        size_t plain_len;
        if (rsrc_is_compressed(src, avail) && rsrc_decompress(src, avail, &plain, &plain_len, err, sizeof err)) {
            *len = plain_len;
            return plain;
        }
    } else {
        return NULL;
    }
    size_t take = m->where == kDataForkCFragLocator && m->length && m->length < avail ? m->length : avail;
    uint8_t *bytes = malloc(take ? take : 1);
    memcpy(bytes, src, take);
    *len = take;
    return bytes;
}

typedef struct {
    const char *want; /* library name, or NULL for the application */
    cfrg_member found;
    bool ok;
} member_search;

static bool match_member(const cfrg_member *m, void *ctx)
{
    member_search *s = ctx;
    bool hit = s->want ? (m->usage == kImportLibraryCFrag && !strcmp(m->name, s->want))
                       : m->usage == kApplicationCFrag;
    if (hit) {
        s->found = *m;
        s->ok = true;
    }
    return hit;
}

/* The cfrg 0 resource of a file (host memory, malloc'd). */
static uint8_t *file_cfrg(const mac_file *mf, size_t *len)
{
    rsrc_index idx;
    char err[128];
    if (!mf->rsrc || !rsrc_parse(mf->rsrc, mf->rsrc_len, &idx, err, sizeof err))
        return NULL;
    uint8_t *out = NULL;
    for (size_t i = 0; i < idx.count && !out; i++)
        if (idx.entries[i].type == 0x63667267u /* cfrg */ && idx.entries[i].id == 0) {
            *len = idx.entries[i].data_len;
            out = malloc(*len ? *len : 1);
            memcpy(out, rsrc_data(mf->rsrc, &idx, &idx.entries[i]), *len);
        }
    rsrc_index_free(&idx);
    return out;
}

/* Load the fragment `want` (NULL: the application's) from a file into a
 * new fragment slot; returns its index or -1. */
static int load_from_file(const char *path, const char *want, char *err, size_t errlen)
{
    mac_file mf;
    if (!appledouble_read(path, &mf, err, errlen))
        return -1;
    size_t clen;
    uint8_t *cfrg = file_cfrg(&mf, &clen);
    member_search s = { .want = want };
    if (cfrg)
        cfrg_each(cfrg, clen, match_member, &s);
    free(cfrg);
    if (!s.ok) {
        mac_file_free(&mf);
        snprintf(err, errlen, "no fragment %s in %s", want ? want : "(application)", path);
        return -1;
    }
    size_t len = 0;
    uint8_t *pef = NULL;
    if (s.found.where == kResourceCFragLocator && !strcmp(path, app_file)) {
        /* The application's own resource: through the Resource Manager,
         * which runs the application's own decompressor if it has one. */
        gaddr h = tb_GetResource((int32_t)s.found.rtype, s.found.rid);
        if (h && rd32(h)) {
            len = (size_t)tb_GetHandleSize(h);
            pef = malloc(len ? len : 1);
            gget(pef, rd32(h), (uint32_t)len);
        }
    }
    if (!pef)
        pef = member_bytes(&mf, &s.found, &len);
    mac_file_free(&mf);
    if (!pef || nfrags >= MAX_FRAGMENTS) {
        free(pef);
        snprintf(err, errlen, "can't read fragment %s", s.found.name);
        return -1;
    }
    const char *dump = getenv("BLUEBOX_DUMP_FRAGMENTS"); /* a folder: each fragment's PEF, for study */
    if (dump) {
        char out[1200];
        snprintf(out, sizeof out, "%s/%s.pef", dump, want ? s.found.name : "application");
        FILE *fp = fopen(out, "wb");
        if (fp) {
            fwrite(pef, 1, len, fp);
            fclose(fp);
        }
    }
    int i = nfrags++;
    fragment *f = &frags[i];
    memset(f, 0, sizeof *f);
    snprintf(f->name, sizeof f->name, "%s", want ? s.found.name : "");
    snprintf(f->path, sizeof f->path, "%s", path);
    f->offset = s.found.where == kDataForkCFragLocator ? s.found.offset : 0;
    f->length = (uint32_t)len;
    f->loading = true;
    bool ok = pef_load(pef, len, f, err, errlen);
    free(pef);
    f->loading = false;
    if (!ok) {
        nfrags--;
        return -1;
    }
    if (f->init_tv && ninit < MAX_FRAGMENTS)
        init_queue[ninit++] = i;
    LOGF(LOG_SEG, "cfm: loaded %s from %s (main %08X, init %08X)\n", f->name[0] ? f->name : "application", path,
         f->main_tv, f->init_tv);
    return i;
}

/* Does this file hold an import library of this name? */
static bool file_has_library(const char *path, const char *name)
{
    mac_file mf;
    char err[128];
    if (!appledouble_read(path, &mf, err, sizeof err))
        return false;
    size_t clen;
    uint8_t *cfrg = file_cfrg(&mf, &clen);
    mac_file_free(&mf);
    member_search s = { .want = name };
    if (cfrg)
        cfrg_each(cfrg, clen, match_member, &s);
    free(cfrg);
    return s.ok;
}

/* Look for a library file in a folder (and the folders in it, one level). */
static bool find_library_file(const char *dir, const char *name, int depth, char *out, size_t outsize)
{
    DIR *d = opendir(dir);
    if (!d)
        return false;
    struct dirent *e;
    bool found = false;
    char sub[64][256];
    int nsub = 0;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st))
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (nsub < 64)
                snprintf(sub[nsub++], sizeof sub[0], "%s", e->d_name);
            continue;
        }
        if (file_has_library(path, name)) {
            snprintf(out, outsize, "%s", path);
            found = true;
        }
    }
    closedir(d);
    for (int i = 0; i < nsub && !found && depth > 0; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, sub[i]);
        found = find_library_file(path, name, depth - 1, out, outsize);
    }
    return found;
}

/* The fragment index of a library, loading it if need be: one already
 * loaded, one of ours, one in the application's file, one in its folder. */
static int library_named(const char *name, char *err, size_t errlen)
{
    for (int i = 0; i < nfrags; i++)
        if (!strcmp(frags[i].name, name))
            return i;
    if (is_native_library(name)) {
        if (nfrags >= MAX_FRAGMENTS)
            return -1;
        fragment *f = &frags[nfrags];
        memset(f, 0, sizeof *f);
        snprintf(f->name, sizeof f->name, "%s", name);
        f->native = true;
        return nfrags++;
    }
    if (frags[0].path[0] && file_has_library(frags[0].path, name))
        return load_from_file(frags[0].path, name, err, errlen);
    char path[1024];
    if (app_dir[0] && find_library_file(app_dir, name, 2, path, sizeof path))
        return load_from_file(path, name, err, errlen);
    snprintf(err, errlen, "library %s not found", name);
    return -1;
}

/* ---------------------------------------------------------------------- */
/* Launching */

bool cfm_has_ppc(void)
{
    gaddr h = tb_Get1Resource((int32_t)fourcc("cfrg"), 0);
    if (!h || !rd32(h) || tb_GetHandleSize(h) < 32)
        return false;
    gaddr p = rd32(h);
    uint32_t n = rd32(p + 28);
    gaddr m = p + 32;
    for (uint32_t i = 0; i < n; i++) {
        if (rd32(m) == 0x70777063u && rd8(m + 22) == kApplicationCFrag)
            return true;
        uint16_t size = rd16(m + 40);
        if (!size)
            break;
        m += size;
    }
    return false;
}

/* The CFragInitBlock for a fragment: its locator (an FSSpec of its file)
 * and name. */
static gaddr init_block(const fragment *f)
{
    gaddr b = tb_NewPtr(64, false, true);
    gaddr spec = tb_NewPtr(80, false, true);
    gaddr name = tb_NewPtr(64, false, true);
    if (!b || !spec || !name)
        return b;
    if (f->path[0])
        fm_spec_for_path(f->path, spec);
    size_t n = strlen(f->name);
    wr8(name, (uint8_t)n);
    gput(name + 1, f->name, (uint32_t)n);
    wr32(b + 12, kDataForkCFragLocator);
    wr32(b + 16, spec);
    wr32(b + 20, f->offset);
    wr32(b + 24, f->length);
    wr32(b + 28, name);
    return b;
}

static gaddr main_tv, done_code, chain_code;

/* The init routines, one after another (each returns here), then main. */
static void run_next_init(ppc_cpu *c)
{
    if (init_next < ninit) {
        const fragment *f = &frags[init_queue[init_next++]];
        c->pc = rd32(f->init_tv);
        c->gpr[2] = rd32(f->init_tv + 4);
        c->gpr[3] = init_block(f);
        c->lr = chain_code;
        return;
    }
    c->pc = rd32(main_tv);
    c->gpr[2] = rd32(main_tv + 4);
    c->lr = done_code;
}

/* main returned: the application is done. */
static void main_returned(ppc_cpu *c)
{
    rm_update_all();
    cpu_halt(g_cpu, "ExitToShell (main returned)");
    ppc_halt(c, "ExitToShell (main returned)");
}

/* BLUEBOX_PPC_TRACE: each PowerPC instruction, for study. */
static void trace_instruction(ppc_cpu *c)
{
    fprintf(stderr, "%08X %08X  r3 %08X r4 %08X r5 %08X lr %08X\n", c->pc, rd32(c->pc), c->gpr[3], c->gpr[4],
            c->gpr[5], c->lr);
}

bool cfm_launch(const char *app_path, char *err, size_t errlen)
{
    nnatives = 0;
    glue_area = 0;
    missing_len = missing_count = 0;
    nfrags = ninit = init_next = ndata_blocks = 0;
    inputsprocket_reset();
    snprintf(app_dir, sizeof app_dir, "%s", app_path);
    snprintf(app_file, sizeof app_file, "%s", app_path);
    char *slash = strrchr(app_dir, '/');
    if (slash)
        *slash = 0;
    else
        app_dir[0] = 0;
    gestalt_powerpc();
    /* The application is fragment 0 (named "", so no import finds it). */
    frags[0].path[0] = 0;
    int a = load_from_file(app_path, NULL, err, errlen);
    if (a < 0)
        return false;
    if (!frags[a].main_tv) {
        snprintf(err, errlen, "the fragment has no main routine");
        return false;
    }
    main_tv = frags[a].main_tv;

    /* An A5 world for the QuickDraw globals' address (InitGraf keeps it at
     * 0(A5)), where a 68k application's would be, with the 68k stack (for
     * the traps and 68k callbacks PowerPC code calls) below it. */
    gaddr a5 = (GA_A5WORLD_TOP - 1024) & ~15u;
    gmemset(a5 - 1024, 0, 2048);
    wr32(LM_CurrentA5, a5);
    wr32(LM_CurStackBase, a5);
    g_cpu->a[5] = a5;
    g_cpu->a[7] = a5 - 64;

    /* A stack of its own: 1 MB, the back chain ending in 0. */
    const uint32_t stack_size = 1024 * 1024;
    gaddr stack = tb_NewPtr((int32_t)stack_size, false, true);
    if (!stack) {
        snprintf(err, errlen, "no memory for the PowerPC stack");
        return false;
    }
    ppc_init(&g_ppc);
    g_ppc.native = dispatch_native;
    if (getenv("BLUEBOX_PPC_TRACE"))
        g_ppc.trace = trace_instruction;
    g_ppc.gpr[1] = (stack + stack_size - 64) & ~15u;
    wr32(g_ppc.gpr[1], 0);
    done_code = rd32(native_tvector(main_returned, "main returned"));
    chain_code = rd32(native_tvector(run_next_init, "next init"));
    run_next_init(&g_ppc);
    g_ppc_mode = true;
    LOGF(LOG_SEG, "cfm: PowerPC fragment, main %08X (code %08X, TOC %08X), %d fragments\n", main_tv,
         rd32(main_tv), rd32(main_tv + 4), nfrags);
    return true;
}

/* ---------------------------------------------------------------------- */
/* The Code Fragment Manager's calls (connection IDs are fragment index + 1) */

static fragment *connection(uint32_t id) { return id >= 1 && id <= (uint32_t)nfrags ? &frags[id - 1] : NULL; }

/* Run the init routines of fragments loaded since `from` (now, nested). */
static void run_new_inits(int from)
{
    for (int q = from; q < ninit; q++) {
        const fragment *f = &frags[init_queue[q]];
        uint32_t block = init_block(f);
        ppc_call(f->init_tv, &block, 1);
    }
    init_next = ninit;
}

static void connect_result(ppc_cpu *c, int i, gaddr conn_p, gaddr main_p, gaddr err_p, const char *err)
{
    if (i < 0) {
        if (err_p) {
            size_t n = strlen(err) > 255 ? 255 : strlen(err);
            wr8(err_p, (uint8_t)n);
            gput(err_p + 1, err, (uint32_t)n);
        }
        c->gpr[3] = (uint32_t)(int32_t)cfragNoLibraryErr;
        return;
    }
    if (conn_p)
        wr32(conn_p, (uint32_t)i + 1);
    if (main_p)
        wr32(main_p, frags[i].main_tv);
    if (err_p)
        wr8(err_p, 0);
    c->gpr[3] = 0;
}

/* GetSharedLibrary(libName, archType, loadFlags, *connID, *mainAddr, errMessage) */
static void ppc_GetSharedLibrary(ppc_cpu *c)
{
    char name[64], err[256];
    pname_to_c(gptr(c->gpr[3]), name, sizeof name);
    int before = ninit;
    int i = library_named(name, err, sizeof err);
    run_new_inits(before);
    connect_result(c, i, c->gpr[6], c->gpr[7], c->gpr[8], err);
    c->pc = c->lr;
}

/* GetDiskFragment(fileSpec, offset, length, fragName, loadFlags, *connID, *mainAddr, errMessage) */
static void ppc_GetDiskFragment(ppc_cpu *c)
{
    gaddr spec = c->gpr[3];
    char path[1024], err[256] = "file not found";
    int i = -1;
    if (fm_host_path((int16_t)rd16(spec), (int32_t)rd32(spec + 2), spec + 6, true, path, sizeof path)) {
        mac_file mf;
        if (appledouble_read(path, &mf, err, sizeof err)) {
            /* The fragment at offset (the whole data fork if none). */
            uint32_t off = c->gpr[4], len = c->gpr[5];
            size_t avail = off <= mf.data_len ? mf.data_len - off : 0;
            size_t take = len && len < avail ? len : avail;
            if (take >= 40 && nfrags < MAX_FRAGMENTS) {
                fragment *f = &frags[nfrags];
                memset(f, 0, sizeof *f);
                if (c->gpr[6])
                    pname_to_c(gptr(c->gpr[6]), f->name, sizeof f->name);
                if (!f->name[0])
                    snprintf(f->name, sizeof f->name, "%s#%d", path, nfrags);
                snprintf(f->path, sizeof f->path, "%s", path);
                f->offset = off;
                f->length = (uint32_t)take;
                int before = ninit, idx = nfrags++;
                if (pef_load(mf.data + off, take, f, err, sizeof err)) {
                    if (f->init_tv && ninit < MAX_FRAGMENTS)
                        init_queue[ninit++] = idx;
                    i = idx;
                } else {
                    nfrags--;
                }
                run_new_inits(before);
            } else {
                snprintf(err, sizeof err, "no fragment there");
            }
            mac_file_free(&mf);
        }
    }
    connect_result(c, i, c->gpr[8], c->gpr[9], c->gpr[10], err);
    if (i < 0)
        c->gpr[3] = (uint32_t)(int32_t)cfragFragmentFormatErr;
    c->pc = c->lr;
}

/* GetMemFragment(memAddr, length, fragName, loadFlags, *connID, *mainAddr, errMessage) */
static void ppc_GetMemFragment(ppc_cpu *c)
{
    char err[256] = "bad fragment";
    int i = -1;
    uint32_t len = c->gpr[4];
    if (len >= 40 && nfrags < MAX_FRAGMENTS) {
        uint8_t *bytes = malloc(len);
        gget(bytes, c->gpr[3], len);
        fragment *f = &frags[nfrags];
        memset(f, 0, sizeof *f);
        if (c->gpr[5])
            pname_to_c(gptr(c->gpr[5]), f->name, sizeof f->name);
        int before = ninit, idx = nfrags++;
        if (pef_load(bytes, len, f, err, sizeof err)) {
            if (f->init_tv && ninit < MAX_FRAGMENTS)
                init_queue[ninit++] = idx;
            i = idx;
        } else {
            nfrags--;
        }
        free(bytes);
        run_new_inits(before);
    }
    connect_result(c, i, c->gpr[7], c->gpr[8], c->gpr[9], err);
    if (i < 0)
        c->gpr[3] = (uint32_t)(int32_t)cfragFragmentFormatErr;
    c->pc = c->lr;
}

/* The length of a PEF container from its section headers. */
static uint32_t container_length(gaddr pef)
{
    uint32_t end = 40 + 28u * rd16(pef + 32);
    for (int s = 0; s < rd16(pef + 32); s++) {
        gaddr h = pef + 40 + 28 * (gaddr)s;
        uint32_t e = rd32(h + 20) + rd32(h + 16);
        if (e > end)
            end = e;
    }
    return end;
}

/* Prepare a fragment in memory (an accelerated code resource's): load it
 * once, run its init routine, and return its main's transition vector. */
gaddr cfm_prepare_container(gaddr pef)
{
    for (int i = 0; i < nfrags; i++)
        if (frags[i].memory == pef)
            return frags[i].main_tv;
    if (rd32(pef) != 0x4A6F7921u /* Joy! */ || nfrags >= MAX_FRAGMENTS)
        return 0;
    uint32_t len = container_length(pef);
    uint8_t *bytes = malloc(len);
    gget(bytes, pef, len);
    fragment *f = &frags[nfrags];
    memset(f, 0, sizeof *f);
    snprintf(f->name, sizeof f->name, "(code resource at %08X)", pef);
    f->memory = pef;
    char err[256];
    int before = ninit, idx = nfrags++;
    bool ok = pef_load(bytes, len, f, err, sizeof err);
    free(bytes);
    if (!ok) {
        LOGF(LOG_UNIMPL, "cfm: code resource at %08X: %s\n", pef, err);
        nfrags--;
        return 0;
    }
    if (f->init_tv && ninit < MAX_FRAGMENTS)
        init_queue[ninit++] = idx;
    run_new_inits(before);
    LOGF(LOG_SEG, "cfm: prepared the code resource at %08X: main %08X\n", pef, f->main_tv);
    return f->main_tv;
}

/* FindSymbol(connID, symName, *symAddr, *symClass) */
static void ppc_FindSymbol(ppc_cpu *c)
{
    fragment *f = connection(c->gpr[3]);
    char name[256];
    pname_to_c(gptr(c->gpr[4]), name, sizeof name);
    gaddr addr = 0;
    uint8_t cls = kTVectorCFragSymbol;
    bool ok = false;
    if (f && !f->native) {
        ok = frag_find_export(f, name, &addr, &cls);
    } else if (f) {
        void (*fn)(ppc_cpu *) = export_fn(name);
        if (fn) {
            addr = native_tvector(fn, name);
            ok = addr != 0;
        }
    }
    if (c->gpr[5])
        wr32(c->gpr[5], ok ? addr : 0);
    if (c->gpr[6])
        wr8(c->gpr[6], cls);
    c->gpr[3] = !f ? (uint32_t)(int32_t)cfragConnectionIDErr : ok ? 0 : (uint32_t)(int32_t)cfragNoSymbolErr;
    c->pc = c->lr;
}

/* CountSymbols(connID, *symCount) */
static void ppc_CountSymbols(ppc_cpu *c)
{
    fragment *f = connection(c->gpr[3]);
    if (c->gpr[4])
        wr32(c->gpr[4], f ? f->nexports : 0);
    c->gpr[3] = f ? 0 : (uint32_t)(int32_t)cfragConnectionIDErr;
    c->pc = c->lr;
}

/* GetIndSymbol(connID, symIndex (1-based), symName, *symAddr, *symClass) */
static void ppc_GetIndSymbol(ppc_cpu *c)
{
    fragment *f = connection(c->gpr[3]);
    uint32_t i = c->gpr[4];
    if (!f || i < 1 || i > f->nexports) {
        c->gpr[3] = (uint32_t)(int32_t)(f ? cfragNoSymbolErr : cfragConnectionIDErr);
        c->pc = c->lr;
        return;
    }
    const frag_export *x = &f->exports[i - 1];
    if (c->gpr[5]) {
        size_t n = strlen(x->name) > 255 ? 255 : strlen(x->name);
        wr8(c->gpr[5], (uint8_t)n);
        gput(c->gpr[5] + 1, x->name, (uint32_t)n);
    }
    if (c->gpr[6])
        wr32(c->gpr[6], x->addr);
    if (c->gpr[7])
        wr8(c->gpr[7], x->cls);
    c->gpr[3] = 0;
    c->pc = c->lr;
}

/* CloseConnection(*connID): the fragment stays. */
static void ppc_CloseConnection(ppc_cpu *c)
{
    c->gpr[3] = 0;
    c->pc = c->lr;
}

const ppc_export cfm_exports[] = {
    { "CloseConnection", ppc_CloseConnection },
    { "CountSymbols", ppc_CountSymbols },
    { "FindSymbol", ppc_FindSymbol },
    { "GetDiskFragment", ppc_GetDiskFragment },
    { "GetIndSymbol", ppc_GetIndSymbol },
    { "GetMemFragment", ppc_GetMemFragment },
    { "GetSharedLibrary", ppc_GetSharedLibrary },
};
const unsigned cfm_export_count = sizeof cfm_exports / sizeof cfm_exports[0];
