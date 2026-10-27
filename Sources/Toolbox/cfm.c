/*
 * Code Fragment Manager: PowerPC applications (PEF containers, and the
 * first tools' XCOFF).
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

#include "carbon_events.h"
#include "forks.h"
#include "hostgl.h"
#include "machine.h"
#include "os9svc.h"
#include "carbon_hi.h"
#include "os9_toolbox.h"
#include "ppc_bridge.h"
#include "tb_internal.h"
#include "trap.h"

ppc_cpu g_ppc;
gaddr cfm_ppc_stack_lo;
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
static void missing_library_import(ppc_cpu *c);

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
    if (natives[index].fn == missing_library_import) {
        ppc_halt(c, "library not found: %s", natives[index].name);
        return;
    }
    trap_set_current_word(0); /* not a trap: no HFS or async bits */
    LOGF(LOG_TRAPS, "%08X PowerPC %s\n", c->lr, natives[index].name);
    if (!mm_debug_before_call(natives[index].name)) {
        ppc_halt(c, "heap damaged before %s", natives[index].name);
        return;
    }
    if (!code_check_before_call(natives[index].name)) {
        ppc_halt(c, "code changed before %s", natives[index].name);
        return;
    }
    natives[index].fn(c);
    if (!mm_debug_after_call(natives[index].name))
        ppc_halt(c, "heap damaged by %s", natives[index].name);
    else if (!code_check_after_call(natives[index].name))
        ppc_halt(c, "code changed by %s", natives[index].name);
}

static gaddr native_tvector(void (*fn)(ppc_cpu *), const char *name);

/* A transition vector (code, TOC) whose code calls fn. */
gaddr cfm_native_tvector(void (*fn)(ppc_cpu *), const char *name) { return native_tvector(fn, name); }

static gaddr native_tvector(void (*fn)(ppc_cpu *), const char *name)
{
    for (int i = 0; i < nnatives; i++)
        if (natives[i].fn == fn && fn != unimplemented_import && fn != missing_library_import && natives[i].tvector)
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
static void missing_library_import(ppc_cpu *c) { (void)c; }

/* ---- Hand-written exports: calls whose 68k forms are raw traps ---- */

static void ppc_ExitToShell(ppc_cpu *c)
{
    stdclib_terminate();
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
    for (unsigned i = 0; i < hfs_export_count; i++)
        if (!strcmp(hfs_exports[i].name, name))
            return hfs_exports[i].fn;
    for (unsigned i = 0; i < isp_export_count; i++)
        if (!strcmp(isp_exports[i].name, name))
            return isp_exports[i].fn;
    for (unsigned i = 0; i < carbon_export_count; i++)
        if (!strcmp(carbon_exports[i].name, name))
            return carbon_exports[i].fn;
    for (unsigned i = 0; i < ce_export_count; i++)
        if (!strcmp(ce_exports[i].name, name))
            return ce_exports[i].fn;
    for (unsigned i = 0; i < mp_export_count; i++)
        if (!strcmp(mp_exports[i].name, name))
            return mp_exports[i].fn;
    void (*carbonhi)(ppc_cpu *) = carbonhi_export_fn(name); /* CarbonLib's drawing, windows and menus */
    if (carbonhi)
        return carbonhi;
    for (unsigned i = 0; i < carbon_ctl_export_count; i++)
        if (!strcmp(carbon_ctl_exports[i].name, name))
            return carbon_ctl_exports[i].fn;
    for (unsigned i = 0; i < carbonupps_export_count; i++) /* CarbonLib's remaining UPPs */
        if (!strcmp(carbonupps_exports[i].name, name))
            return carbonupps_exports[i].fn;
    for (unsigned i = 0; i < lmacc_export_count; i++) /* the rest of LowMem.h's accessors */
        if (!strcmp(lmacc_exports[i].name, name))
            return lmacc_exports[i].fn;
    for (unsigned i = 0; i < carbongaps_export_count; i++) /* CarbonLib's small additions */
        if (!strcmp(carbongaps_exports[i].name, name))
            return carbongaps_exports[i].fn;
    for (unsigned i = 0; i < gimport_export_count; i++) /* QuickTime's graphics importers */
        if (!strcmp(gimport_exports[i].name, name))
            return gimport_exports[i].fn;
    for (unsigned i = 0; i < walk_export_count; i++) /* weak imports the archive walk's programs called */
        if (!strcmp(walk_exports[i].name, name))
            return walk_exports[i].fn;
    for (unsigned i = 0; i < qtcapture_export_count; i++) /* QuickTime's capture and import calls */
        if (!strcmp(qtcapture_exports[i].name, name))
            return qtcapture_exports[i].fn;
    for (unsigned i = 0; i < vam_export_count; i++) /* found missing by the archive walk */
        if (!strcmp(vam_exports[i].name, name))
            return vam_exports[i].fn;
    for (unsigned i = 0; i < priv_os9_export_count; i++) /* Mac OS 9's private names */
        if (!strcmp(priv_os9_exports[i].name, name))
            return priv_os9_exports[i].fn;
    for (unsigned i = 0; i < carbonuser_export_count; i++) /* CarbonLib's user and machine names */
        if (!strcmp(carbonuser_exports[i].name, name))
            return carbonuser_exports[i].fn;
    void (*svc)(ppc_cpu *) = carbonsvc_export_fn(name);
    if (svc)
        return svc;
    void (*os9)(ppc_cpu *) = os9svc_export_fn(name); /* Mac OS 9's service libraries */
    if (os9)
        return os9;
    void (*cf)(ppc_cpu *) = carbon_cf_export_fn(name);
    if (cf)
        return cf;
    for (unsigned i = 0; i < snd_export_count; i++)
        if (!strcmp(snd_exports[i].name, name))
            return snd_exports[i].fn;
    for (unsigned i = 0; i < dsp_export_count; i++)
        if (!strcmp(dsp_exports[i].name, name))
            return dsp_exports[i].fn;
    for (unsigned i = 0; i < ot_export_count; i++)
        if (!strcmp(ot_exports[i].name, name))
            return ot_exports[i].fn;
    for (unsigned i = 0; i < ota_export_count; i++)
        if (!strcmp(ota_exports[i].name, name))
            return ota_exports[i].fn;
    for (unsigned i = 0; i < ots_export_count; i++)
        if (!strcmp(ots_exports[i].name, name))
            return ots_exports[i].fn;
    for (unsigned i = 0; i < ctb_export_count; i++)
        if (!strcmp(ctb_exports[i].name, name))
            return ctb_exports[i].fn;
    for (unsigned i = 0; i < ml_export_count; i++)
        if (!strcmp(ml_exports[i].name, name))
            return ml_exports[i].fn;
    void (*stdc)(ppc_cpu *) = stdclib_export_fn(name);
    if (stdc)
        return stdc;
    for (unsigned i = 0; i < lib_export_count; i++)
        if (!strcmp(lib_exports[i].name, name))
            return lib_exports[i].fn;
    for (unsigned i = 0; i < cfm_export_count; i++)
        if (!strcmp(cfm_exports[i].name, name))
            return cfm_exports[i].fn;
    for (unsigned i = 0; i < mm_export_count; i++)
        if (!strcmp(mm_exports[i].name, name))
            return mm_exports[i].fn;
    for (unsigned i = 0; i < tec_export_count; i++)
        if (!strcmp(tec_exports[i].name, name))
            return tec_exports[i].fn;
    for (unsigned i = 0; i < ppc_glue_export_count; i++)
        if (!strcmp(ppc_glue_exports[i].name, name))
            return ppc_glue_exports[i].fn;
    void (*util)(ppc_cpu *) = ilutil_export_fn(name);
    if (util)
        return util;
    if (name[0] == 'a' && name[1] == 'g' && name[2] == 'l')
        for (unsigned i = 0; i < agl_export_count; i++)
            if (!strcmp(agl_exports[i].name, name))
                return agl_exports[i].fn;
    if (name[0] == 'g' && name[1] == 'l')
        for (unsigned i = 0; i < gl_export_count; i++)
            if (!strcmp(gl_exports[i].name, name))
                return gl_exports[i].fn;
    if (os9tbx_export_fn(name)) /* WindowsLib, ControlsLib, MenusLib and AppearanceLib's own */
        return os9tbx_export_fn(name);
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

/* Whether a library's export is ours (bluebox exports), as an import of it
 * is resolved: a routine by name, data by the data exports. */
bool machine_provides_export(const char *name, bool data)
{
    return (!data && export_fn(name)) || ppc_data_export_known(name);
}

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
    const char *const *absent; /* each missing weak import's "symbol from library" */
    uint32_t nimports;
    gaddr target_base, reloc, sect_c, sect_d;
    uint32_t import_index;
    char *err;
    size_t errlen;
    int depth;
} reloc_state;

/* Where the missing weak imports went: a call through one of them (an
 * illegal instruction at 0) is then named. */
typedef struct {
    gaddr slot;
    char *name;
} absent_slot;
static absent_slot *absent_slots;
static size_t absent_count, absent_cap;

static void note_absent_slot(gaddr slot, const char *name)
{
    if (absent_count == absent_cap) {
        size_t cap = absent_cap ? absent_cap * 2 : 64;
        absent_slot *grown = realloc(absent_slots, cap * sizeof *grown);
        if (!grown)
            return;
        absent_slots = grown;
        absent_cap = cap;
    }
    absent_slots[absent_count++] = (absent_slot){ slot, strdup(name) };
}

static void free_absent(char **absent, uint32_t n)
{
    for (uint32_t i = 0; absent && i < n; i++)
        free(absent[i]);
    free(absent);
}

/* The missing weak import a call to 0 came through: the cross-TOC glue
 * (lwz r12,d(r2); stw r2,20(r1); lwz r0,0(r12); ...; bctr) saved the
 * caller's TOC at 20(r1), and its first instruction gives the slot. */
static void name_nil_call(ppc_cpu *c)
{
    if (c->op_pc != 0 || !absent_count)
        return;
    gaddr toc = rd32(c->gpr[1] + 20);
    for (int back = 1; back <= 8; back++) {
        uint32_t op = rd32(c->prev_pc - 4u * back);
        if ((op & 0xFFFF0000u) != 0x81820000u) /* lwz r12,d(r2) */
            continue;
        gaddr slot = toc + (uint32_t)(int16_t)(op & 0xFFFF);
        for (size_t i = absent_count; i-- > 0;)
            if (absent_slots[i].slot == slot) {
                size_t n = strlen(c->halt_reason);
                snprintf(c->halt_reason + n, sizeof c->halt_reason - n, ": weak %s, not there",
                         absent_slots[i].name);
                return;
            }
        return;
    }
}

static gaddr import_at(reloc_state *s, uint32_t index)
{
    if (index >= s->nimports)
        return 0;
    if (!s->imports[index] && s->absent && s->absent[index])
        note_absent_slot(s->reloc, s->absent[index]);
    return s->imports[index];
}
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
    char app_name[64];   /* the application's name in its 'cfrg', which libraries may import from */
    char path[1024];     /* the file it came from */
    uint32_t offset, length;
    gaddr main_tv, init_tv, term_tv;
    frag_export *exports;
    uint32_t nexports;
    bool native;         /* a library we provide (InterfaceLib ...): no PEF */
    gaddr memory;        /* a code resource's container, prepared from memory */
    bool loading;
    gaddr code, data;    /* its first code and data sections */
    uint32_t code_size, data_size;
    gaddr container;     /* the container in guest memory, when it has exception tables */
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
    "OTClientLib", "OTClientUtilLib", "OTInetClientLib", "OTUtilityLib", "OTXTILib", "OpenTransportLib",
    "OpenTptInternetLib", "OpenTptClientLib", "OpenTptUtilityLib", "OTATalkClientLib", "OTATalkSharedLib",
    "OpenTptAppleTalkLib", "OTNtvUtilLib", "QuickDrawGXLib", "CollectionsLib", "ContextualMenu", "IconServicesLib",
    "TextCommon", "UnicodeConverter", "TextEncodingConverter", "CarbonLib",
    "FontManager", "MPLibrary", "DriverServicesLib", "PowerMgrLib", "URLAccessLib", "NameRegistryLib",
    "DriverLoaderLib", "KeychainLib", "FoldersLib", "FileSigningLib", "UTCUtils",
    "Apple;Carbon;Multimedia", "Apple;Carbon;Networking", /* CarbonLib's other names */
};

/* Apple libraries left unloaded even where an installer put them: the
 * Text Encoding Converter's globals (Internet Explorer 4's installer adds
 * it), which its own libraries found through a table its boot-time code
 * made.  The converter is ours (tec.c), and has no use for them. */
static bool unloaded_library(const char *name) { return !strcmp(name, "TextCommonGlobals"); }

static bool core_library(const char *name)
{
    return !strcmp(name, "InterfaceLib") || !strcmp(name, "MathLib") || !strcmp(name, "StdCLib");
}

static bool is_native_library(const char *name)
{
    /* Apple's OpenGL, where the host has one to draw with (opengl.c). */
    if (!strcmp(name, "OpenGLLibrary") || !strcmp(name, "OpenGLUtility"))
        return hostgl_available();
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

/* Export i of a loader section, at addr. */
static void add_export(fragment *f, const uint8_t *loader, uint32_t strings, const uint8_t *keys,
                       const uint8_t *esyms, uint32_t i, gaddr addr)
{
    uint32_t namelen = be32(keys + i * 4) >> 16;
    uint32_t cn = be32(esyms + i * 10);
    frag_export *x = &f->exports[f->nexports++];
    x->name = strndup((const char *)loader + strings + (cn & 0xFFFFFF), namelen);
    x->addr = addr;
    x->cls = (uint8_t)(cn >> 24) & 0x0F;
}

static int library_named(const char *name, char *err, size_t errlen);

/* What an import of sname (class cls) from library lname binds to: an
 * address (a transition vector or data), or 0 for a missing weak one.
 * li is the library's fragment, or -1 where it wasn't found. */
static gaddr bind_import(const char *lname, int li, const char *sname, int cls, bool weak, bool weak_lib)
{
    const fragment *from = li >= 0 && !frags[li].native ? &frags[li] : NULL;
    gaddr addr = 0;
    if (from) {
        if (frag_find_export(from, sname, &addr, NULL))
            return addr;
    } else if (li >= 0 || !weak_lib) {
        /* By name, as CFM binds: a routine imported as data
         * (Hardwood Solitaire II's GetCurrentProcess) is still its
         * transition vector, unless we have data of that name. */
        void (*fn)(ppc_cpu *) = cls == kDataCFragSymbol && ppc_data_export(sname) ? NULL : export_fn(sname);
        if (fn)
            return native_tvector(fn, sname);
        /* Our data exports, whatever class the importer gave them
         * (CallComponentUPP comes as a transition vector in some). */
        if (li >= 0 && ppc_data_export(sname) != 0)
            return data_import(sname);
    }
    /* A weak symbol, or one from a weakly linked library that isn't
     * there, is 0.  But InterfaceLib, MathLib and StdCLib are always
     * there with everything Mac OS 9 had in them, so what they lack
     * here is ours to add: a stub that says so. */
    if (li >= 0 && !strncmp(lname, "OT", 2)) {
        /* Open Transport's other calls say they're not supported. */
        LOGF(LOG_UNIMPL, "cfm: %s from %s: not supported\n", sname, lname);
        return native_tvector(ot_unsupported, sname);
    }
    if ((weak || weak_lib) && (li < 0 || !core_library(lname))) {
        LOGF(LOG_SEG, "cfm: weak %s from %s: not there\n", sname, lname);
        return 0;
    }
    if (cls == kDataCFragSymbol) {
        LOGF(LOG_UNIMPL, "cfm: data %s from %s not provided\n", sname, lname);
        note_missing(lname, sname, true);
        return data_import(sname);
    }
    if (li < 0) {
        /* A library Mac OS didn't have either: a plug-in's host
         * (NOVA's saverlets import from a fragment inside NOVA, found
         * only once NOVA has loaded it), or one the program needs
         * installed.  Called, it stops naming the library. */
        char both[64];
        snprintf(both, sizeof both, "%s (for %s)", lname, sname);
        return native_tvector(missing_library_import, both);
    }
    LOGF(LOG_UNIMPL, "cfm: %s from %s not provided\n", sname, lname);
    note_missing(lname, sname, false);
    return native_tvector(unimplemented_import, sname);
}

/* An XCOFF container, as the first PowerPC tools wrote them (Anvil's,
 * IBM's) and the Code Fragment Manager loaded too: Boingo Electro and
 * Bumbler Bee-Luxe have one in the data fork.  Its sections (.text,
 * .data, .bss) go where PEF's would; the .loader section names the
 * imports by library, relocates the words that hold addresses and lists
 * the exports, and the auxiliary header gives main's descriptor. */
enum { kXCOFFMagic = 0x01DF, kSTYP_TEXT = 0x20, kSTYP_DATA = 0x40, kSTYP_BSS = 0x80, kSTYP_LOADER = 0x1000 };
enum { kXMC_PR = 0, kXMC_DS = 10, kL_IMPORT = 0x40, kL_EXPORT = 0x10 };

static bool xcoff_load(const uint8_t *x, size_t len, fragment *f, char *err, size_t errlen)
{
    uint16_t nscns = be16(x + 2), opthdr = be16(x + 16);
    if (20 + (size_t)opthdr + nscns * 40u > len || nscns > 32) {
        snprintf(err, errlen, "truncated XCOFF");
        return false;
    }
    /* Section i (1-based, as the loader counts them): where it went, and
     * the address the linker gave it. */
    gaddr base[33] = { 0 };
    uint32_t vaddr[33] = { 0 }, flags[33] = { 0 };
    const uint8_t *loader = NULL;
    uint32_t loader_len = 0;
    int text = 0, data = 0, bss = 0;
    for (int i = 1; i <= nscns; i++) {
        const uint8_t *h = x + 20 + opthdr + (i - 1) * 40;
        uint32_t size = be32(h + 16), ptr = be32(h + 20);
        vaddr[i] = be32(h + 12);
        flags[i] = be32(h + 36) & 0xFFFF;
        if (flags[i] != kSTYP_BSS && (size_t)ptr + size > len) {
            snprintf(err, errlen, "XCOFF section %d beyond the container", i);
            return false;
        }
        if (flags[i] == kSTYP_LOADER) {
            loader = x + ptr;
            loader_len = size;
            continue;
        }
        if (flags[i] != kSTYP_TEXT && flags[i] != kSTYP_DATA && flags[i] != kSTYP_BSS)
            continue;
        gaddr b = tb_NewPtr((int32_t)(size + 16), false, true);
        if (!b) {
            snprintf(err, errlen, "no memory for XCOFF section %d (%u bytes)", i, size);
            return false;
        }
        base[i] = (b + 15) & ~15u;
        gmemset(base[i], 0, size);
        if (flags[i] != kSTYP_BSS)
            gput(base[i], x + ptr, size);
        if (flags[i] == kSTYP_TEXT && !text) {
            text = i;
            f->code = base[i], f->code_size = size;
            code_check_add(base[i], size, f->name[0] ? f->name : "the application's code");
        } else if (flags[i] == kSTYP_DATA && !data) {
            data = i;
            f->data = base[i], f->data_size = size;
        } else if (flags[i] == kSTYP_BSS && !bss) {
            bss = i;
        }
        LOGF(LOG_SEG, "cfm: XCOFF section %d (%04X) at %08X, %u bytes\n", i, flags[i], base[i], size);
    }
    if (!loader || loader_len < 32) {
        snprintf(err, errlen, "no loader section");
        return false;
    }
    uint32_t nsyms = be32(loader + 4), nreloc = be32(loader + 8), nimpid = be32(loader + 16);
    uint32_t impoff = be32(loader + 20), stoff = be32(loader + 28);
    if (32 + (size_t)nsyms * 24 + (size_t)nreloc * 12 > loader_len || impoff > loader_len || stoff > loader_len) {
        snprintf(err, errlen, "truncated XCOFF loader section");
        return false;
    }
    /* The libraries: each import file ID is a path, a base name and an
     * archive member, NUL-ended; the first is the default search path. */
    const char *libname[64] = { 0 };
    const char *p = (const char *)loader + impoff, *end = (const char *)loader + loader_len;
    for (uint32_t i = 0; i < nimpid && i < 64; i++) {
        const char *path = p, *basename;
        p += strnlen(p, (size_t)(end - p)) + 1;
        basename = p < end ? p : "";
        p += strnlen(p, (size_t)(end - p)) + 1;
        p += strnlen(p, (size_t)(end - p)) + 1;
        if (p > end)
            break;
        libname[i] = basename[0] ? basename : path;
    }
    int libfrag[64];
    for (int i = 0; i < 64; i++)
        libfrag[i] = -2; /* not looked up yet */

    /* Symbols: imports bound, defined ones at their sections' places. */
    gaddr *sym = calloc(nsyms ? nsyms : 1, sizeof *sym);
    char **names = calloc(nsyms ? nsyms : 1, sizeof *names);
    f->exports = calloc(nsyms ? nsyms : 1, sizeof *f->exports);
    f->nexports = 0;
    for (uint32_t i = 0; i < nsyms; i++) {
        const uint8_t *e = loader + 32 + i * 24;
        if (be32(e) == 0) {
            uint32_t o = stoff + be32(e + 4);
            names[i] = o < loader_len ? strndup((const char *)loader + o, loader_len - o) : strdup("");
        } else {
            names[i] = strndup((const char *)e, 8);
        }
        uint32_t value = be32(e + 8);
        int16_t scn = (int16_t)be16(e + 12);
        uint8_t type = e[14], smclas = e[15];
        uint32_t ifile = be32(e + 16);
        int cls = smclas == kXMC_DS ? kTVectorCFragSymbol : smclas == kXMC_PR ? kCodeCFragSymbol : kDataCFragSymbol;
        if (type & kL_IMPORT) {
            const char *lname = ifile < 64 && libname[ifile] ? libname[ifile] : "InterfaceLib";
            if (ifile < 64 && libfrag[ifile] == -2) {
                char lerr[256];
                libfrag[ifile] = library_named(lname, lerr, sizeof lerr);
                if (libfrag[ifile] < 0)
                    LOGF(LOG_UNIMPL, "cfm: library %s: %s\n", lname, lerr);
            }
            sym[i] = bind_import(lname, ifile < 64 ? libfrag[ifile] : -1, names[i], cls, false, false);
        } else if (scn >= 1 && scn <= nscns && base[scn]) {
            sym[i] = base[scn] + (value - vaddr[scn]);
            if (type & kL_EXPORT) {
                frag_export *xp = &f->exports[f->nexports++];
                xp->name = strdup(names[i]);
                xp->addr = sym[i];
                xp->cls = (uint8_t)cls;
            }
        }
    }
    /* Relocations: each adds to a word what its symbol moved by (a
     * section's own symbols 0-2, or a defined symbol), or an import's
     * address.  Only whole words (R_POS and its load forms) are left for
     * a loader to do. */
    bool ok = true;
    const uint8_t *rel = loader + 32 + nsyms * 24;
    for (uint32_t r = 0; r < nreloc && ok; r++) {
        const uint8_t *e = rel + r * 12;
        uint32_t at = be32(e), ndx = be32(e + 4);
        uint8_t rsize = e[8], rtype = e[9];
        int16_t secnm = (int16_t)be16(e + 10);
        if (secnm < 1 || secnm > nscns || !base[secnm] || (rsize & 0x3F) != 31 ||
            (rtype != 0x00 && rtype != 0x0C && rtype != 0x0D)) {
            snprintf(err, errlen, "XCOFF relocation %u: type %02X size %u in section %d", r, rtype, rsize, secnm);
            ok = false;
            break;
        }
        uint32_t add;
        int implicit = ndx == 0 ? text : ndx == 1 ? data : ndx == 2 ? bss : 0;
        if (ndx < 3) {
            add = implicit ? base[implicit] - vaddr[implicit] : 0;
        } else if (ndx - 3 < nsyms) {
            const uint8_t *s = loader + 32 + (ndx - 3) * 24;
            int16_t scn = (int16_t)be16(s + 12);
            if (s[14] & kL_IMPORT)
                add = sym[ndx - 3];
            else
                add = scn >= 1 && scn <= nscns && base[scn] ? base[scn] - vaddr[scn] : 0;
        } else {
            snprintf(err, errlen, "XCOFF relocation %u: symbol %u of %u", r, ndx, nsyms);
            ok = false;
            break;
        }
        gaddr where = base[secnm] + (at - vaddr[secnm]);
        wr32(where, rd32(where) + add);
    }
    /* main: the descriptor the auxiliary header names. */
    if (ok && opthdr >= 72) {
        const uint8_t *a = x + 20;
        uint32_t entry = be32(a + 16);
        uint16_t snentry = be16(a + 32);
        if (snentry >= 1 && snentry <= nscns && base[snentry])
            f->main_tv = base[snentry] + (entry - vaddr[snentry]);
    }
    for (uint32_t i = 0; i < nsyms; i++)
        free(names[i]);
    free(names);
    free(sym);
    return ok;
}

/* Load a PEF container into fragment f: sections, imports (loading the
 * libraries it needs), relocations, exports.  With an image of the
 * container in guest memory (at, else 0), its code and unpacked data are
 * used where they are in it, as CFM uses a container in memory:
 * FutureBASIC^3's runtime keeps glue and variables between its code and
 * data sections and reaches them from both. */
static bool pef_load(const uint8_t *pef, size_t len, gaddr at, fragment *f, char *err, size_t errlen)
{
    if (len >= 20 && be16(pef) == kXCOFFMagic)
        return xcoff_load(pef, len, f, err, errlen);
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
        if (at && (kind == kCode || kind == kUnpackedData) && packed == total) {
            sections[s] = (section){ at + off, total, kind, at + off - be32(h + 4) };
            LOGF(LOG_SEG, "cfm: section %d (kind %d) in place at %08X, %u bytes\n", s, kind, at + off, total);
            continue;
        }
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

    /* Exports first, before the libraries this one needs are loaded: one
     * of them may need this one in turn (Internet Explorer's C++ library
     * and its MS Memory Library each import the other).  The key table
     * gives each name's length, the symbol table its class, name and
     * place. */
    const uint8_t *keys = loader + hash_off + (4u << hash_power);
    const uint8_t *esyms = keys + nexports * 4;
    f->exports = calloc(nexports ? nexports : 1, sizeof *f->exports);
    f->nexports = 0;
    for (uint32_t i = 0; i < nexports; i++) {
        const uint8_t *e = esyms + i * 10;
        uint32_t value = be32(e + 4);
        int16_t sect = (int16_t)be16(e + 8);
        if (sect >= 0 && sect < ninst)
            add_export(f, loader, strings, keys, esyms, i, sections[sect].base + value);
        else if (sect == -2)
            add_export(f, loader, strings, keys, esyms, i, value); /* absolute */
    }

    /* Imports: an address each (a transition vector or data), or 0 for
     * missing weak ones. */
    gaddr *imports = calloc(nsyms ? nsyms : 1, sizeof *imports);
    char **absent = calloc(nsyms ? nsyms : 1, sizeof *absent);
    const uint8_t *libs = loader + 56, *syms = libs + nlibs * 24;
    uint32_t next = 0; /* where the libraries so far end */
    for (uint32_t l = 0; l < nlibs; l++) {
        const uint8_t *lib = libs + l * 24;
        const char *lname = (const char *)loader + strings + be32(lib);
        uint32_t count = be32(lib + 12), first = be32(lib + 16);
        /* Each library's symbols follow the one before's: SmalltalkAgents'
         * linker gave its runtime library first 0 after InterfaceLib's one
         * symbol, so its last (RuntimeInit) was never bound.  Libraries
         * whose symbols are merely in another order keep theirs. */
        if (first < next && next + count <= nsyms)
            first = next;
        if (first + count > next)
            next = first + count;
        bool weak_lib = (lib[20] & 0x40) != 0;
        char lerr[256];
        int li = library_named(lname, lerr, sizeof lerr);
        if (li < 0)
            LOGF(weak_lib ? LOG_SEG : LOG_UNIMPL, "cfm: library %s: %s\n", lname, lerr);
        for (uint32_t k = first; k < first + count && k < nsyms; k++) {
            uint32_t w = be32(syms + k * 4);
            const char *sname = (const char *)loader + strings + (w & 0xFFFFFF);
            imports[k] = bind_import(lname, li, sname, (w >> 24) & 0x0F, (w >> 24) & 0x80, weak_lib);
            if (!imports[k] && absent) { /* a weak import not there: named if it's called */
                char both[256];
                snprintf(both, sizeof both, "%s from %s", sname, lname);
                absent[k] = strdup(both);
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
            .absent = (const char *const *)absent,
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
            free_absent(absent, nsyms);
            free(sections);
            return false;
        }
    }
    /* Re-exported imports, now that the imports are known. */
    for (uint32_t i = 0; i < nexports; i++) {
        const uint8_t *e = esyms + i * 10;
        uint32_t value = be32(e + 4);
        if ((int16_t)be16(e + 8) == -3 && value < nsyms)
            add_export(f, loader, strings, keys, esyms, i, imports[value]);
    }
    for (int i = ninst - 1; i >= 0; i--) {
        if (sections[i].kind == kCode)
            f->code = sections[i].base, f->code_size = sections[i].size;
        else if (sections[i].base && sections[i].kind != kConstant)
            f->data = sections[i].base, f->data_size = sections[i].size;
    }
    for (int i = 0; i < ninst; i++)
        if (sections[i].kind == kCode && sections[i].base)
            code_check_add(sections[i].base, sections[i].size, f->name[0] ? f->name : "the application's code");
    /* C++ exception handling (MrCExceptionsLib) reads the exception
     * section from the container itself (FragFindOwnerOfPC). */
    f->container = 0;
    for (int i = 0; i < nsect; i++)
        if (pef[40 + i * 28 + 24] == kException && (f->container = tb_NewPtr((int32_t)len, false, false)) != 0) {
            gput(f->container, pef, (uint32_t)len);
            break;
        }
    f->main_tv = main_sect >= 0 && main_sect < ninst ? sections[main_sect].base + main_off : 0;
    f->init_tv = init_sect >= 0 && init_sect < ninst ? sections[init_sect].base + init_off : 0;
    f->term_tv = term_sect >= 0 && term_sect < ninst ? sections[term_sect].base + term_off : 0;
    free(imports);
    free_absent(absent, nsyms);
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
            const uint8_t *data = rsrc_data(mf->rsrc, &idx, &idx.entries[i]);
            size_t n = idx.entries[i].data_len;
            /* A compressed one (Graphing Calculator's) comes out plain. */
            if (rsrc_is_compressed(data, n) && rsrc_decompress(data, n, &out, len, err, sizeof err))
                continue;
            *len = n;
            out = malloc(n ? n : 1);
            memcpy(out, data, n);
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
    snprintf(f->app_name, sizeof f->app_name, "%s", want ? "" : s.found.name);
    snprintf(f->path, sizeof f->path, "%s", path);
    f->offset = s.found.where == kDataForkCFragLocator ? s.found.offset : 0;
    f->length = (uint32_t)len;
    f->loading = true;
    bool ok = pef_load(pef, len, 0, f, err, errlen);
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

/* Whether an application's PowerPC fragment is a container we can load
 * (PEF or XCOFF), as far as its first bytes tell.  One in a resource is
 * taken on trust: the application may decompress it itself. */
bool cfm_app_fragment_loadable(const char *path)
{
    mac_file mf;
    char err[256];
    if (!appledouble_read(path, &mf, err, sizeof err))
        return false;
    size_t clen, len = 0;
    uint8_t *cfrg = file_cfrg(&mf, &clen);
    member_search s = { .want = NULL };
    if (cfrg)
        cfrg_each(cfrg, clen, match_member, &s);
    free(cfrg);
    bool ok = s.ok;
    if (ok && s.found.where == kDataForkCFragLocator) {
        uint8_t *bytes = member_bytes(&mf, &s.found, &len);
        ok = bytes && ((len >= 40 && !memcmp(bytes, "Joy!peff", 8)) || (len >= 20 && be16(bytes) == kXCOFFMagic));
        free(bytes);
    }
    mac_file_free(&mf);
    return ok;
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
    /* The application by its fragment's name: MacPerl's is "Perl", whose
     * routines its extensions (shared libraries it loads) import. */
    for (int i = 0; i < nfrags; i++)
        if (frags[i].app_name[0] && !strcmp(frags[i].app_name, name))
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
    if (unloaded_library(name)) {
        snprintf(err, errlen, "library %s left out", name);
        return -1;
    }
    if (frags[0].path[0] && file_has_library(frags[0].path, name))
        return load_from_file(frags[0].path, name, err, errlen);
    char path[1024];
    if (app_dir[0] && find_library_file(app_dir, name, 2, path, sizeof path))
        return load_from_file(path, name, err, errlen);
    /* Then the Extensions folder, as the Code Fragment Manager searched it
     * (with folders in it: Microsoft's "MS Library Folder"). */
    char ext[1100];
    snprintf(ext, sizeof ext, "%s/Extensions", fm_system_folder_host());
    if (find_library_file(ext, name, 2, path, sizeof path))
        return load_from_file(path, name, err, errlen);
    snprintf(err, errlen, "library %s not found", name);
    return -1;
}

/* Whether the program runs on CarbonLib (imports from it). */
bool cfm_imports_carbonlib(void)
{
    for (int i = 0; i < nfrags; i++)
        if (!strcmp(frags[i].name, "CarbonLib"))
            return true;
    return false;
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
    stdclib_terminate();
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

static bool ppc_cpu_ready;

/* A launch starts the Code Fragment Manager afresh: no fragments, no
 * native glue, no PowerPC processor set up yet. */
void cfm_reset(void)
{
    nnatives = 0;
    glue_area = 0;
    missing_len = missing_count = 0;
    nfrags = ninit = init_next = ndata_blocks = 0;
    for (size_t i = 0; i < absent_count; i++)
        free(absent_slots[i].name);
    absent_count = 0;
    stdclib_reset(); /* the last program's library state went with it */
    carbon_cf_reset();
    ppc_cpu_ready = false;
    cfm_ppc_stack_lo = 0;
    code_check_reset(); /* the last program's code */
}

/* The PowerPC processor, set up once per launch: a stack of its own (1 MB,
 * the back chain ending in 0) and our native calls.  A 68k program gets it
 * when it first calls PowerPC code through a routine descriptor, as on a
 * Power Mac (Installer VISE's own PowerPC routines). */
bool cfm_ppc_cpu(void)
{
    if (ppc_cpu_ready)
        return true;
    const uint32_t stack_size = 1024 * 1024;
    gaddr stack = tb_NewPtr((int32_t)stack_size, false, true);
    if (!stack)
        return false;
    cfm_ppc_stack_lo = stack;
    ppc_init(&g_ppc);
    g_ppc.native = dispatch_native;
    g_ppc.illegal = name_nil_call;
    if (getenv("BLUEBOX_PPC_TRACE"))
        g_ppc.trace = trace_instruction;
    g_ppc.gpr[1] = (stack + stack_size - 64) & ~15u;
    wr32(g_ppc.gpr[1], 0);
    ppc_cpu_ready = true;
    return true;
}

bool cfm_launch(const char *app_path, char *err, size_t errlen)
{
    cfm_reset();
    inputsprocket_reset();
    snprintf(app_dir, sizeof app_dir, "%s", app_path);
    snprintf(app_file, sizeof app_file, "%s", app_path);
    char *slash = strrchr(app_dir, '/');
    if (slash)
        *slash = 0;
    else
        app_dir[0] = 0;
    gestalt_powerpc();
    /* The application is fragment 0 (named "": imports find it only by
     * its name in the 'cfrg', as MacPerl's extensions do). */
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

    if (!cfm_ppc_cpu()) {
        snprintf(err, errlen, "no memory for the PowerPC stack");
        return false;
    }
    /* CarbonLib set up the Toolbox for its programs as it loaded. */
    for (int i = 0; i < nfrags; i++)
        if (frags[i].native && !strcmp(frags[i].name, "CarbonLib"))
            carbonlib_init();
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
    if (i < 0)
        LOGF(LOG_SEG, "cfm: GetSharedLibrary %s: %s\n", name, err);
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
                if (pef_load(mf.data + off, take, 0, f, err, sizeof err)) {
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

/* A container from memory, copied whole into memory of its own: the
 * fragment's code and data are used there (pef_load), and the original
 * (a code resource, which may move) is left as it was. */
static gaddr memory_image(const uint8_t *bytes, uint32_t len)
{
    gaddr p = tb_NewPtr((int32_t)len, false, false);
    if (p)
        gput(p, bytes, len);
    return p;
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
        gaddr image = memory_image(bytes, len);
        if (pef_load(bytes, len, image, f, err, sizeof err)) {
            if (f->init_tv && ninit < MAX_FRAGMENTS)
                init_queue[ninit++] = idx;
            i = idx;
        } else {
            nfrags--;
            if (image)
                tb_DisposePtr(image);
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
    gaddr image = memory_image(bytes, len);
    bool ok = pef_load(bytes, len, image, f, err, sizeof err);
    free(bytes);
    if (!ok && image)
        tb_DisposePtr(image);
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
    if (!ok)
        LOGF(LOG_UNIMPL, "cfm: FindSymbol %s: not there\n", name);
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

/* GetIndSymbol(connID, symIndex, symName, *symAddr, *symClass): the index
 * counts from 0 to CountSymbols - 1 (mTropolis asks each plug-in for its
 * symbol 0). */
static void ppc_GetIndSymbol(ppc_cpu *c)
{
    fragment *f = connection(c->gpr[3]);
    uint32_t i = c->gpr[4];
    if (!f || i >= f->nexports) {
        c->gpr[3] = (uint32_t)(int32_t)(f ? cfragNoSymbolErr : cfragConnectionIDErr);
        c->pc = c->lr;
        return;
    }
    const frag_export *x = &f->exports[i];
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

/* FragFindOwnerOfPC(pc, *info) (PrivateInterfaceLib, undocumented): the
 * fragment whose code holds pc.  MrCExceptionsLib reads the container at
 * 0 (its section headers lead to the exception tables) and the code
 * section and its size at 16 and 20; what lies between is our guess (the
 * container's length, the data section and its size). */
static void ppc_FragFindOwnerOfPC(ppc_cpu *c)
{
    gaddr pc = c->gpr[3], info = c->gpr[4];
    c->pc = c->lr;
    for (int i = 0; i < nfrags; i++) {
        const fragment *f = &frags[i];
        if (!f->code || pc < f->code || pc - f->code >= f->code_size)
            continue;
        if (info) {
            gaddr container = f->memory ? f->memory : f->container;
            wr32(info, container);
            wr32(info + 4, container ? f->length : 0);
            wr32(info + 8, f->data);
            wr32(info + 12, f->data_size);
            wr32(info + 16, f->code);
            wr32(info + 20, f->code_size);
        }
        c->gpr[3] = 0;
        return;
    }
    c->gpr[3] = (uint32_t)(int32_t)paramErr;
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
    { "FragFindOwnerOfPC", ppc_FragFindOwnerOfPC },
    { "GetDiskFragment", ppc_GetDiskFragment },
    { "GetIndSymbol", ppc_GetIndSymbol },
    { "GetMemFragment", ppc_GetMemFragment },
    { "GetSharedLibrary", ppc_GetSharedLibrary },
};
const unsigned cfm_export_count = sizeof cfm_exports / sizeof cfm_exports[0];
