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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    natives[index].fn(c);
}

/* A transition vector (code, TOC) whose code calls fn. */
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

/* ---------------------------------------------------------------------- */
/* PEF */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

enum { kCode = 0, kUnpackedData, kPatternData, kConstant, kLoader, kDebug, kExecData, kException, kTraceback };

typedef struct {
    gaddr base;
    uint32_t size;
    int kind;
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

/* Run a section's relocation instructions. */
static bool relocate(const uint8_t *instr, uint32_t count, const section *sections, int nsections, int target,
                     const gaddr *imports, uint32_t nimports, char *err, size_t errlen)
{
    gaddr reloc = sections[target].base;
    gaddr sect_c = nsections > 0 ? sections[0].base : 0, sect_d = nsections > 1 ? sections[1].base : 0;
    uint32_t import_index = 0;
    uint32_t i = 0;
    /* For repeats: where each instruction started. */
    uint32_t *starts = malloc(sizeof *starts * (count + 1));
#define ADD(v) do { wr32(reloc, rd32(reloc) + (v)); reloc += 4; } while (0)
    while (i < count) {
        starts[i] = i;
        uint16_t w = be16(instr + i * 2);
        uint32_t here = i;
        i++;
        if ((w & 0xC000) == 0x0000) { /* RelocBySectDWithSkip */
            uint32_t skip = (w >> 6) & 0xFF, n = w & 0x3F;
            reloc += skip * 4;
            for (uint32_t k = 0; k < n; k++)
                ADD(sect_d);
        } else if ((w & 0xE000) == 0x4000) { /* the run group */
            uint32_t sub = (w >> 9) & 0xF, run = (w & 0x1FF) + 1;
            for (uint32_t k = 0; k < run; k++) {
                switch (sub) {
                case 0: ADD(sect_c); break;                          /* RelocBySectC */
                case 1: ADD(sect_d); break;                          /* RelocBySectD */
                case 2: ADD(sect_c); ADD(sect_d); reloc += 4; break; /* RelocTVector12 */
                case 3: ADD(sect_c); ADD(sect_d); break;             /* RelocTVector8 */
                case 4: ADD(sect_d); reloc += 4; break;              /* RelocVTable8 */
                case 5:                                              /* RelocImportRun */
                    ADD(import_index < nimports ? imports[import_index] : 0);
                    import_index++;
                    break;
                default:
                    snprintf(err, errlen, "relocation run subopcode %u", sub);
                    free(starts);
                    return false;
                }
            }
        } else if ((w & 0xE000) == 0x6000) { /* the small index group */
            uint32_t sub = (w >> 9) & 0xF, index = w & 0x1FF;
            switch (sub) {
            case 0: /* RelocSmByImport */
                ADD(index < nimports ? imports[index] : 0);
                import_index = index + 1;
                break;
            case 1: sect_c = (int)index < nsections ? sections[index].base : 0; break; /* RelocSmSetSectC */
            case 2: sect_d = (int)index < nsections ? sections[index].base : 0; break; /* RelocSmSetSectD */
            case 3: ADD((int)index < nsections ? sections[index].base : 0); break;     /* RelocSmBySection */
            default:
                snprintf(err, errlen, "relocation index subopcode %u", sub);
                free(starts);
                return false;
            }
        } else if ((w & 0xF000) == 0x8000) { /* RelocIncrPosition */
            reloc += (w & 0x0FFF) + 1;
        } else if ((w & 0xF000) == 0x9000) { /* RelocSmRepeat */
            uint32_t blocks = ((w >> 8) & 0xF) + 1, times = (w & 0xFF) + 1;
            /* Repeat the previous `blocks` instructions `times` times. */
            uint32_t from = here >= blocks ? here - blocks : 0;
            for (uint32_t t = 0; t < times; t++) {
                for (uint32_t k = from; k < here; k++) {
                    uint16_t rw = be16(instr + k * 2);
                    /* Only the simple forms are repeated in practice. */
                    if ((rw & 0xC000) == 0x0000) {
                        uint32_t skip = (rw >> 6) & 0xFF, n = rw & 0x3F;
                        reloc += skip * 4;
                        for (uint32_t q = 0; q < n; q++)
                            ADD(sect_d);
                    } else if ((rw & 0xE000) == 0x4000) {
                        uint32_t sub = (rw >> 9) & 0xF, run = (rw & 0x1FF) + 1;
                        for (uint32_t q = 0; q < run; q++) {
                            if (sub == 0) ADD(sect_c);
                            else if (sub == 1) ADD(sect_d);
                            else if (sub == 2) { ADD(sect_c); ADD(sect_d); reloc += 4; }
                            else if (sub == 3) { ADD(sect_c); ADD(sect_d); }
                            else if (sub == 4) { ADD(sect_d); reloc += 4; }
                            else { ADD(import_index < nimports ? imports[import_index] : 0); import_index++; }
                        }
                    } else if ((rw & 0xF000) == 0x8000) {
                        reloc += (rw & 0x0FFF) + 1;
                    } else if ((rw & 0xE000) == 0x6000 && ((rw >> 9) & 0xF) == 0) {
                        uint32_t idx = rw & 0x1FF;
                        ADD(idx < nimports ? imports[idx] : 0);
                        import_index = idx + 1;
                    }
                }
            }
        } else if ((w & 0xFC00) == 0xA000) { /* RelocSetPosition */
            uint32_t off = ((uint32_t)(w & 0x3FF) << 16) | be16(instr + i * 2);
            i++;
            reloc = sections[target].base + off;
        } else if ((w & 0xFC00) == 0xA400) { /* RelocLgByImport */
            uint32_t index = ((uint32_t)(w & 0x3FF) << 16) | be16(instr + i * 2);
            i++;
            ADD(index < nimports ? imports[index] : 0);
            import_index = index + 1;
        } else if ((w & 0xFC00) == 0xB000) { /* RelocLgRepeat */
            uint32_t blocks = ((w >> 6) & 0xF) + 1;
            uint32_t times = ((uint32_t)(w & 0x3F) << 16) | be16(instr + i * 2);
            i++;
            (void)blocks;
            (void)times;
            snprintf(err, errlen, "RelocLgRepeat is not handled yet");
            free(starts);
            return false;
        } else if ((w & 0xFC00) == 0xB400) { /* RelocLgSetOrBySection */
            uint32_t sub = (w >> 6) & 0xF;
            uint32_t index = ((uint32_t)(w & 0x3F) << 16) | be16(instr + i * 2);
            i++;
            gaddr base = (int)index < nsections ? sections[index].base : 0;
            if (sub == 0)
                ADD(base);
            else if (sub == 1)
                sect_c = base;
            else if (sub == 2)
                sect_d = base;
        } else {
            snprintf(err, errlen, "relocation opcode %04X", w);
            free(starts);
            return false;
        }
    }
#undef ADD
    free(starts);
    return true;
}

typedef struct {
    gaddr main_tv, init_tv;
} pef_result;

static bool pef_load(const uint8_t *pef, size_t len, pef_result *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof *out);
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
        sections[s] = (section){ base, total, kind };
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
    uint32_t main_off = be32(loader + 4), init_off = be32(loader + 12);
    uint32_t nlibs = be32(loader + 24), nsyms = be32(loader + 28), nreloc = be32(loader + 32);
    uint32_t reloc_off = be32(loader + 36), strings = be32(loader + 40);

    /* Imports: a transition vector (or 0, for missing weak ones) each. */
    gaddr *imports = calloc(nsyms ? nsyms : 1, sizeof *imports);
    const uint8_t *libs = loader + 56, *syms = libs + nlibs * 24;
    for (uint32_t l = 0; l < nlibs; l++) {
        const uint8_t *lib = libs + l * 24;
        const char *lname = (const char *)loader + strings + be32(lib);
        uint32_t count = be32(lib + 12), first = be32(lib + 16);
        bool weak_lib = (lib[20] & 0x40) != 0;
        for (uint32_t k = first; k < first + count && k < nsyms; k++) {
            uint32_t w = be32(syms + k * 4);
            const char *sname = (const char *)loader + strings + (w & 0xFFFFFF);
            bool weak = (w >> 24) & 0x80;
            int cls = (w >> 24) & 0x0F;
            void (*fn)(ppc_cpu *) = cls == 2 ? export_fn(sname) : NULL;
            if (fn) {
                imports[k] = native_tvector(fn, sname);
            } else if (weak || weak_lib) {
                imports[k] = 0;
            } else {
                imports[k] = native_tvector(unimplemented_import, sname);
                LOGF(LOG_UNIMPL, "cfm: %s from %s not provided\n", sname, lname);
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
        if (!relocate(loader + reloc_off + first, count, sections, ninst, target, imports, nsyms, err, errlen)) {
            free(imports);
            free(sections);
            return false;
        }
    }
    if (main_sect >= 0 && main_sect < ninst)
        out->main_tv = sections[main_sect].base + main_off;
    if (init_sect >= 0 && init_sect < ninst)
        out->init_tv = sections[init_sect].base + init_off;
    free(imports);
    free(sections);
    return true;
}

/* ---------------------------------------------------------------------- */
/* Launching */

/* The PowerPC fragment of an application, from its 'cfrg' 0: the bytes of
 * the data fork it names.  NULL if there is none. */
static uint8_t *ppc_fragment(const char *app_path, size_t *len)
{
    gaddr h = tb_Get1Resource((int32_t)fourcc("cfrg"), 0);
    if (!h || !rd32(h) || tb_GetHandleSize(h) < 32)
        return NULL;
    gaddr p = rd32(h);
    uint32_t n = rd32(p + 28);
    gaddr m = p + 32;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t arch = rd32(m);
        uint8_t usage = rd8(m + 22), where = rd8(m + 23);
        uint32_t off = rd32(m + 24), flen = rd32(m + 28);
        uint16_t size = rd16(m + 40);
        if (arch == 0x70777063u /* pwpc */ && usage == 1 /* application */ && where == 1 /* data fork */) {
            mac_file mf;
            char err[256];
            if (!appledouble_read(app_path, &mf, err, sizeof err))
                return NULL;
            if (off > mf.data_len) {
                mac_file_free(&mf);
                return NULL;
            }
            size_t avail = mf.data_len - off;
            size_t take = flen && flen < avail ? flen : avail;
            uint8_t *bytes = malloc(take ? take : 1);
            memcpy(bytes, mf.data + off, take);
            mac_file_free(&mf);
            *len = take;
            return bytes;
        }
        if (!size)
            break;
        m += size;
    }
    return NULL;
}

bool cfm_has_ppc(void)
{
    gaddr h = tb_Get1Resource((int32_t)fourcc("cfrg"), 0);
    if (!h || !rd32(h) || tb_GetHandleSize(h) < 32)
        return false;
    gaddr p = rd32(h);
    uint32_t n = rd32(p + 28);
    gaddr m = p + 32;
    for (uint32_t i = 0; i < n; i++) {
        if (rd32(m) == 0x70777063u && rd8(m + 22) == 1)
            return true;
        uint16_t size = rd16(m + 40);
        if (!size)
            break;
        m += size;
    }
    return false;
}

static gaddr main_tv, done_code;

/* After the init routine: main, returning to main_returned. */
static void start_main(ppc_cpu *c)
{
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

bool cfm_launch(const char *app_path, char *err, size_t errlen)
{
    size_t len;
    uint8_t *pef = ppc_fragment(app_path, &len);
    if (!pef) {
        snprintf(err, errlen, "no PowerPC fragment in the data fork");
        return false;
    }
    nnatives = 0;
    glue_area = 0;
    pef_result r;
    bool ok = pef_load(pef, len, &r, err, errlen);
    free(pef);
    if (!ok)
        return false;
    if (!r.main_tv) {
        snprintf(err, errlen, "the fragment has no main routine");
        return false;
    }
    main_tv = r.main_tv;

    /* An A5 world for the QuickDraw globals' address (InitGraf keeps it at
     * 0(A5)) and the 68k side's application globals. */
    gaddr a5world = tb_NewPtr(1024, false, true);
    gaddr a5 = a5world + 512;
    wr32(LM_CurrentA5, a5);
    g_cpu->a[5] = a5;

    /* A stack of its own: 1 MB, the back chain ending in 0. */
    const uint32_t stack_size = 1024 * 1024;
    gaddr stack = tb_NewPtr((int32_t)stack_size, false, true);
    if (!stack) {
        snprintf(err, errlen, "no memory for the PowerPC stack");
        return false;
    }
    ppc_init(&g_ppc);
    g_ppc.native = dispatch_native;
    g_ppc.gpr[1] = (stack + stack_size - 64) & ~15u;
    wr32(g_ppc.gpr[1], 0);
    gaddr done = native_tvector(main_returned, "main returned");
    done_code = rd32(done);
    g_ppc.lr = done_code;
    if (r.init_tv) {
        /* init(CFragInitBlock *): the block is left empty for now. */
        gaddr block = tb_NewPtr(64, false, true);
        gaddr then = native_tvector(start_main, "start main");
        g_ppc.pc = rd32(r.init_tv);
        g_ppc.gpr[2] = rd32(r.init_tv + 4);
        g_ppc.gpr[3] = block;
        g_ppc.lr = rd32(then);
    } else {
        g_ppc.pc = rd32(main_tv);
        g_ppc.gpr[2] = rd32(main_tv + 4);
    }
    g_ppc_mode = true;
    LOGF(LOG_SEG, "cfm: PowerPC fragment, main %08X (code %08X, TOC %08X)\n", main_tv, rd32(main_tv),
         rd32(main_tv + 4));
    return true;
}
