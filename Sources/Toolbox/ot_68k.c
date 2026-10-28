/*
 * Open Transport for 68k programs, and for the 68k code in PowerPC ones.
 *
 * A 68k program links Apple's static glue (OpenTransportApp.o, OpenTransport.o,
 * OpenTptInet.o, ...).  Its calls go through stubs that jump to a function
 * pointer they cache, or the first time to the Shared Library Manager's
 * dispatcher, with A0 at the stub's record (the library class to load, as a
 * string, and the function's index in it).  Its start-up asks Gestalt for
 * Open Transport, finds the manager through the Expand Memory record (a
 * pointer at offset $10C to the manager's global, which holds the manager
 * object at $10 and the dispatcher's address at $88) and has the manager
 * initialise itself and load the libraries.
 *
 * Here the manager is ours.  Gestalt's answer to 68k code sets up the
 * Expand Memory record and the global, so the glue finds a manager already
 * loaded.  The dispatcher looks the function up in the table the glue's own
 * declarations give (ot_68k_table.c), binds the stub to a native routine,
 * and jumps to it with the caller's stack as it was.  That routine takes
 * the arguments off the stack by the function's convention (the Open
 * Transport calls are Pascal, or C for the lower-case names), runs the same
 * implementation a PowerPC caller gets, and returns the result in its slot
 * or D0.  Notifiers and other callbacks the program gives are called as 68k
 * code.
 */
#include <stdio.h>
#include <string.h>

#include "machine.h"
#include "ot_68k.h"
#include "ot_internal.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define LM_EXPAND_MEM 0x02B6
enum { kSLMGlobalOffset = 0x10C, kExpandMemMin = 0x200, kSLMGlobalSize = 0x100, kVtableSize = 0x100 };

static gaddr slm_global, lib_mgr, vtable;
static gaddr dispatch_stub, unsupported_stub;
static gaddr stub_of[512];                 /* the native routine bound to each table entry */
static const ot68k_fn *bound[8192];        /* a native's table entry, by native index */
static void (*impl[512])(ppc_cpu *);

/* Return from a routine the 68k code called with a C stack frame: the return
 * address popped, the result in D0. */
static void return_c(cpu68k *c, uint32_t result)
{
    c->d[0] = result;
    uint32_t sp = c->a[7];
    c->pc = rd32(sp);
    c->a[7] = sp + 4;
}

/* ---- The manager object ---- */

/* A call on the manager that does nothing and says it went well (loading a
 * library, releasing the manager). */
static void slm_ok(cpu68k *c, uint16_t tw)
{
    (void)tw;
    return_c(c, 0);
}

/* InitLibraryManager(this, &gLibraryManager, ...): the manager is there. */
static void slm_init(cpu68k *c, uint16_t tw)
{
    (void)tw;
    gaddr var = rd32(c->a[7] + 8);
    if (var)
        wr32(var, lib_mgr);
    return_c(c, 0);
}

/* CleanupLibraryManager(this, &gLibraryManager, flag): false, the manager
 * stays loaded. */
static void slm_cleanup(cpu68k *c, uint16_t tw)
{
    (void)tw;
    gaddr var = rd32(c->a[7] + 8);
    if (var)
        wr32(var, 0);
    return_c(c, 0);
}

/* ---- The dispatcher ---- */

static const ot68k_fn *lookup(const char *lib, int index)
{
    for (unsigned i = 0; i < ot68k_function_count; i++)
        if (ot68k_functions[i].index == index && !strcmp(ot68k_functions[i].lib, lib))
            return &ot68k_functions[i];
    return NULL;
}

static void adapter(cpu68k *c, uint16_t tw);

static gaddr bind_function(const ot68k_fn *f)
{
    unsigned i = (unsigned)(f - ot68k_functions);
    if (!stub_of[i]) {
        stub_of[i] = native_register(adapter, f->name, 0);
        int ni = native_index_for(stub_of[i]);
        if (ni >= 0 && (unsigned)ni < sizeof bound / sizeof bound[0])
            bound[ni] = f;
        impl[i] = cfm_export_fn(f->name);
    }
    return stub_of[i];
}

/* The unsupported call: C, nothing popped, kOTNotSupportedErr in D0. */
static void unsupported(cpu68k *c, uint16_t tw)
{
    (void)tw;
    return_c(c, (uint32_t)kOTNotSupportedErr);
}

/* A0 is the stub's record: +0 the cached routine, +4 the library record (its
 * class string at +16), +12 the function's index. */
static void dispatch(cpu68k *c, uint16_t tw)
{
    (void)tw;
    gaddr rec = c->a[0], lib = rd32(rec + 4);
    char cls[64];
    uint32_t i = 0;
    for (; i < sizeof cls - 1 && rd8(lib + 16 + i); i++)
        cls[i] = (char)rd8(lib + 16 + i);
    cls[i] = 0;
    int index = rd16(rec + 12);
    const ot68k_fn *f = lookup(cls, index);
    gaddr target;
    if (f && f->conv != '?') {
        target = bind_function(f);
    } else {
        LOGF(LOG_UNIMPL, "OT 68k: %s #%d%s%s not supported\n", cls, index, f ? " " : "", f ? f->name : "");
        target = unsupported_stub;
    }
    wr32(rec, target);
    c->pc = target; /* on to it with the caller's stack */
}

/* The routine for a table entry: its arguments off the stack, the
 * implementation run, the result and the return. */
static void adapter(cpu68k *c, uint16_t tw)
{
    (void)tw;
    int ni = native_index_for(c->op_pc);
    const ot68k_fn *f = ni >= 0 ? bound[ni] : NULL;
    if (!f) {
        unsupported(c, 0);
        return;
    }
    unsigned entry = (unsigned)(f - ot68k_functions);
    uint32_t sp = c->a[7], ret = rd32(sp);
    LOGF(LOG_TRAPS, "OT 68k: %s (from %08X)\n", f->name, ret);
    size_t n = strlen(f->sig);
    uint32_t args[24] = { 0 };
    uint32_t popped = 0;
    if (n > 24)
        n = 24;
    if (f->conv == 'P') {
        /* Pascal: the last argument was pushed last; a byte sits in the high
         * half of a word, and everything is at least a word */
        uint32_t off = 4;
        for (size_t i = n; i-- > 0;) {
            switch (f->sig[i]) {
            case 'l': args[i] = rd32(sp + off); off += 4; break;
            case 'w': args[i] = rd16(sp + off); off += 2; break;
            default: args[i] = rd8(sp + off); off += 2; break;
            }
        }
        popped = off - 4;
    } else {
        for (size_t i = 0; i < n; i++)
            args[i] = rd32(sp + 4 + 4 * (uint32_t)i);
    }
    static ppc_cpu regs;
    ppc_cpu saved = regs;
    memset(&regs, 0, sizeof regs);
    for (size_t i = 0; i < n; i++)
        regs.gpr[3 + i] = args[i];
    bool was = ot_from_68k;
    ot_from_68k = true;
    if (impl[entry]) {
        impl[entry](&regs);
        const char *nm = f->name;
        if (!strncmp(nm, "OTCreateDeferredTask", 20) || !strncmp(nm, "OTCreateSystemTask", 18) ||
            !strncmp(nm, "OTCreateTimerTask", 17))
            ot_note_68k_proc(args[0]); /* the task's routine is 68k code */
    } else {
        regs.gpr[3] = (uint32_t)kOTNotSupportedErr;
    }
    ot_from_68k = was;
    uint32_t result = regs.gpr[3];
    regs = saved;
    if (f->conv == 'P') {
        gaddr slot = sp + 4 + popped;
        switch (f->ret) {
        case 'l': wr32(slot, result); break;
        case 'w': wr16(slot, (uint16_t)result); break;
        case 'b': wr8(slot, (uint8_t)result); break;
        default: break;
        }
        c->d[0] = result;
        c->a[7] = slot;
        c->pc = ret;
    } else {
        return_c(c, f->ret == 'v' ? c->d[0] : result);
    }
}

/* ---- Setting the manager up ---- */

/* The Expand Memory record, the manager's global in it and the manager
 * itself, made when 68k code first asks Gestalt for Open Transport (the
 * glue's start-up does, before anything else). */
void ot_68k_prepare(void)
{
    if (!slm_global || !lib_mgr) {
        memset(stub_of, 0, sizeof stub_of);
        memset(bound, 0, sizeof bound);
        memset(impl, 0, sizeof impl);
        slm_global = tb_NewPtr(kSLMGlobalSize, true, true);
        lib_mgr = tb_NewPtr(8, true, true);
        vtable = tb_NewPtr(kVtableSize, true, true);
        dispatch_stub = native_register(dispatch, "SLMFuncDispatch", 0);
        unsupported_stub = native_register(unsupported, "OT unsupported", 0);
        gaddr ok = native_register(slm_ok, "SLM ok", 0);
        for (uint32_t k = 0; k < kVtableSize; k += 4)
            wr32(vtable + k, ok);
        wr32(vtable + 0x9C, native_register(slm_init, "SLM InitLibraryManager", 0));
        wr32(vtable + 0xA0, native_register(slm_cleanup, "SLM CleanupLibraryManager", 0));
        wr32(lib_mgr, vtable);
        wr32(slm_global + 0x10, lib_mgr);
        wr32(slm_global + 0x88, dispatch_stub);
    }
    gaddr em = rd32(LM_EXPAND_MEM);
    if (!em || tb_GetPtrSize(em) < kExpandMemMin) {
        gaddr grown = tb_NewPtr(kExpandMemMin, true, true);
        if (grown) {
            if (em)
                gmemcpy(grown, em, (uint32_t)tb_GetPtrSize(em));
            wr32(LM_EXPAND_MEM, grown);
            em = grown;
        }
    }
    if (em)
        wr32(em + kSLMGlobalOffset, slm_global);
}

void ot_68k_reset(void)
{
    slm_global = lib_mgr = vtable = dispatch_stub = unsupported_stub = 0;
    memset(stub_of, 0, sizeof stub_of);
    memset(bound, 0, sizeof bound);
    memset(impl, 0, sizeof impl);
}

/* A function of the glue's table by name: its library class, index, convention,
 * argument sizes and result (for the tests, and for a look at what is bound). */
bool ot_68k_find(const char *name, char lib[64], int *index, char *conv, char sig[24], char *ret)
{
    for (unsigned i = 0; i < ot68k_function_count; i++) {
        const ot68k_fn *f = &ot68k_functions[i];
        if (strcmp(f->name, name))
            continue;
        snprintf(lib, 64, "%s", f->lib);
        *index = f->index;
        *conv = f->conv;
        snprintf(sig, 24, "%s", f->sig);
        *ret = f->ret;
        return true;
    }
    return false;
}
