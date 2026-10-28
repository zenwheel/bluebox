/*
 * IconServicesLib's remaining exports, on the IconRefs of iconservices.c.
 * Where Mac OS 9's list has a name no header gave, the arguments here are
 * Blue Box's:
 *
 *   RegisterIconRef(creator, type, iconRef): the icons of iconRef are what
 *     GetIconRef finds for the creator and type.
 *   RegisterIconRefFromFile(creator, type, fsspec, *iconRef): the icon the
 *     file has (as GetIconRefFromFile gives it), registered so.
 *   OverrideIconRefFromFile(iconRef, fsspec): the file's icon is shown for
 *     iconRef.
 *   GetIconRefFromFileTypeAndCreator(fileType, creator, *iconRef)
 *   GetIconRefFromFileInfo(fsref, nameLength, name, whichInfo, catalogInfo,
 *     flags, *iconRef, *label): the file the FSRef is, else the kind its
 *     catalog Finder information (file type and creator) says
 *   GetIconRefFromFolderInfo(vRefNum, parentID, folderID, attributes,
 *     privileges, flags, *iconRef)
 *   NewIconFamilyFromIconRef, NewIconSuiteFromIconRef(iconRef, whichIcons,
 *     *result) and NewIconSuiteFromFile(fsspec, whichIcons, *suite)
 *   GetIconRefByName(name, *iconRef), NewIconSuiteByName(name, whichIcons,
 *     *suite) and FlushIconRefsByName(name): a Pascal string naming one of
 *     the system's icons (the kinds the Finder shows: "Folder",
 *     "Application", "Trash" ...), any case.
 */
#include <string.h>
#include <strings.h>

#include "ppc_bridge.h"
#include "tb_internal.h"
#include "os9left.h"

enum { noSuchIconErr = -2581, kAllAvailable = -1 };

#define SYSTEM_CREATOR 0x6D616373u /* 'macs' */

/* The names of the system's icons and the types they are filed under. */
static const struct { const char *name; uint32_t type; } names[] = {
    { "Application", 0x4150504C }, { "Generic Application", 0x4150504C },
    { "Document", 0x646F6375 }, { "Generic Document", 0x646F6375 },
    { "Folder", 0x666C6472 }, { "Generic Folder", 0x666C6472 },
    { "Trash", 0x74727368 }, { "Full Trash", 0x66747268 },
    { "Disk", 0x6864736B }, { "Generic Disk", 0x6864736B },
    { "Floppy", 0x666C7079 }, { "Hard Disk", 0x6864736B },
    { "Desktop", 0x6465736B }, { "System Folder", 0x6D616373 },
    { "Control Panel", 0x6374726C }, { "Extension", 0x6578746E },
};

static uint32_t type_of_name(gaddr p, bool *ok)
{
    char s[256];
    unsigned n = rd8(p);
    for (unsigned i = 0; i < n; i++)
        s[i] = (char)rd8(p + 1 + i);
    s[n] = 0;
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcasecmp(s, names[i].name)) {
            *ok = true;
            return names[i].type;
        }
    *ok = false;
    return 0;
}

static void ic_GetIconRefByName(ppc_cpu *c)
{
    if (!R(0) || !R(1))
        RETURN(paramErr);
    bool ok;
    uint32_t type = type_of_name(R(0), &ok);
    if (!ok) {
        wr32(R(1), 0);
        RETURN(noSuchIconErr);
    }
    RETURN(tb_GetIconRef(0, SYSTEM_CREATOR, (int32_t)type, R(1)));
}

static void ic_FlushIconRefsByName(ppc_cpu *c)
{
    bool ok;
    if (!R(0))
        RETURN(paramErr);
    uint32_t type = type_of_name(R(0), &ok);
    if (!ok)
        RETURN(noSuchIconErr);
    RETURN(tb_FlushIconRefs(SYSTEM_CREATOR, (int32_t)type));
}

/* An out cell for a result: the cell's long, and gone after. */
static int16_t family_of(gaddr ref, uint32_t which, gaddr *fam)
{
    gaddr cell = tb_NewPtr(4, false, true);
    if (!cell)
        return memFullErr;
    int16_t e = tb_IconRefToIconFamily(ref, which, cell);
    *fam = rd32(cell);
    tb_DisposePtr(cell);
    return e;
}

static void ic_NewIconFamilyFromIconRef(ppc_cpu *c)
{
    if (!R(2))
        RETURN(paramErr);
    RETURN(tb_IconRefToIconFamily(R(0), R(1), R(2)));
}

/* A suite of the icons of a ref. */
static int16_t suite_of(gaddr ref, uint32_t which, gaddr out)
{
    gaddr fam;
    int16_t e = family_of(ref, which, &fam);
    if (e)
        return e;
    e = tb_IconFamilyToIconSuite(fam, which, out);
    tb_DisposeHandle(fam);
    return e;
}

static void ic_NewIconSuiteFromIconRef(ppc_cpu *c)
{
    if (!R(2))
        RETURN(paramErr);
    RETURN(suite_of(R(0), R(1), R(2)));
}

/* Make a ref with a cell, run `then` on it, release it. */
static int16_t with_ref(int16_t (*get)(gaddr cell, void *a), void *a, int16_t (*then)(gaddr ref, void *b), void *b)
{
    gaddr cell = tb_NewPtr(4, false, true);
    if (!cell)
        return memFullErr;
    int16_t e = get(cell, a);
    if (!e) {
        gaddr ref = rd32(cell);
        e = then(ref, b);
        tb_ReleaseIconRef(ref);
    }
    tb_DisposePtr(cell);
    return e;
}

struct suite_req {
    uint32_t which;
    gaddr out;
};
static int16_t get_file(gaddr cell, void *a) { return tb_GetIconRefFromFile((gaddr)(uintptr_t)a, cell, 0); }
static int16_t make_suite(gaddr ref, void *b)
{
    struct suite_req *r = b;
    return suite_of(ref, r->which, r->out);
}

static void ic_NewIconSuiteFromFile(ppc_cpu *c)
{
    if (!R(0) || !R(2))
        RETURN(paramErr);
    struct suite_req r = { R(1), R(2) };
    RETURN(with_ref(get_file, (void *)(uintptr_t)R(0), make_suite, &r));
}

static int16_t get_named(gaddr cell, void *a)
{
    bool ok;
    uint32_t type = type_of_name((gaddr)(uintptr_t)a, &ok);
    return ok ? tb_GetIconRef(0, SYSTEM_CREATOR, (int32_t)type, cell) : noSuchIconErr;
}

static void ic_NewIconSuiteByName(ppc_cpu *c)
{
    if (!R(0) || !R(2))
        RETURN(paramErr);
    struct suite_req r = { R(1), R(2) };
    RETURN(with_ref(get_named, (void *)(uintptr_t)R(0), make_suite, &r));
}

/* ---- Registering and overriding ---- */

static void ic_RegisterIconRef(ppc_cpu *c)
{
    gaddr fam, made = tb_NewPtr(4, false, true);
    if (!made)
        RETURN(memFullErr);
    int16_t e = family_of(R(2), (uint32_t)kAllAvailable, &fam);
    if (!e) {
        e = tb_RegisterIconRefFromIconFamily((int32_t)R(0), (int32_t)R(1), fam, made);
        tb_DisposeHandle(fam);
        if (!e)
            tb_ReleaseIconRef(rd32(made)); /* it stays, registered */
    }
    tb_DisposePtr(made);
    RETURN(e);
}

struct reg_req {
    int32_t creator, type;
    gaddr out;
};
static int16_t register_ref(gaddr ref, void *b)
{
    struct reg_req *r = b;
    gaddr fam;
    int16_t e = family_of(ref, (uint32_t)kAllAvailable, &fam);
    if (e)
        return e;
    e = tb_RegisterIconRefFromIconFamily(r->creator, r->type, fam, r->out);
    tb_DisposeHandle(fam);
    return e;
}

static void ic_RegisterIconRefFromFile(ppc_cpu *c)
{
    if (!R(2) || !R(3))
        RETURN(paramErr);
    struct reg_req r = { (int32_t)R(0), (int32_t)R(1), R(3) };
    RETURN(with_ref(get_file, (void *)(uintptr_t)R(2), register_ref, &r));
}

static int16_t override_with(gaddr ref, void *b) { return tb_OverrideIconRef((gaddr)(uintptr_t)b, ref); }

static void ic_OverrideIconRefFromFile(ppc_cpu *c)
{
    if (!R(1))
        RETURN(paramErr);
    if (!tb_IsValidIconRef(R(0)))
        RETURN(-2580);
    RETURN(with_ref(get_file, (void *)(uintptr_t)R(1), override_with, (void *)(uintptr_t)R(0)));
}

/* ---- Getting by file type, FSRef, folder ---- */

static void ic_GetIconRefFromFileTypeAndCreator(ppc_cpu *c)
{
    if (!R(2))
        RETURN(paramErr);
    RETURN(tb_GetIconRef(0, (int32_t)R(1), (int32_t)R(0), R(2)));
}

enum { kFSCatInfoFinderInfo = 0x800, CI_NODEFLAGS = 0, CI_FINDERINFO = 72 };

static void ic_GetIconRefFromFileInfo(ppc_cpu *c)
{
    gaddr out = R(6);
    if (!out)
        RETURN(paramErr);
    wr32(out, 0);
    if (R(0)) {
        gaddr spec = tb_NewPtr(70, false, true);
        if (!spec)
            RETURN(memFullErr);
        int16_t e = tb_FSGetCatalogInfo(R(0), 0, 0, 0, spec, 0);
        if (!e)
            e = tb_GetIconRefFromFile(spec, out, R(7));
        tb_DisposePtr(spec);
        RETURN(e);
    }
    if (!R(4) || !(R(3) & kFSCatInfoFinderInfo))
        RETURN(paramErr);
    if (rd16(R(4) + CI_NODEFLAGS) & 0x10) { /* a folder */
        RETURN(tb_GetIconRef(0, SYSTEM_CREATOR, 0x666C6472 /* 'fldr' */, out));
    }
    uint32_t type = rd32(R(4) + CI_FINDERINFO), creator = rd32(R(4) + CI_FINDERINFO + 4);
    if (R(7))
        wr16(R(7), 0);
    RETURN(tb_GetIconRef(0, (int32_t)creator, (int32_t)type, out));
}

static void ic_GetIconRefFromFolderInfo(ppc_cpu *c)
{
    RETURN(tb_GetIconRefFromFolder((int16_t)R(0), (int32_t)R(1), (int32_t)R(2), (int8_t)R(3), (int8_t)R(4), R(6)));
}

void os9left_ui_reset(void) {}

const ppc_export os9left_ui_exports[] = {
    { "FlushIconRefsByName", ic_FlushIconRefsByName },
    { "GetIconRefByName", ic_GetIconRefByName },
    { "GetIconRefFromFileInfo", ic_GetIconRefFromFileInfo },
    { "GetIconRefFromFileTypeAndCreator", ic_GetIconRefFromFileTypeAndCreator },
    { "GetIconRefFromFolderInfo", ic_GetIconRefFromFolderInfo },
    { "NewIconFamilyFromIconRef", ic_NewIconFamilyFromIconRef },
    { "NewIconSuiteByName", ic_NewIconSuiteByName },
    { "NewIconSuiteFromFile", ic_NewIconSuiteFromFile },
    { "NewIconSuiteFromIconRef", ic_NewIconSuiteFromIconRef },
    { "OverrideIconRefFromFile", ic_OverrideIconRefFromFile },
    { "RegisterIconRef", ic_RegisterIconRef },
    { "RegisterIconRefFromFile", ic_RegisterIconRefFromFile },
};
const unsigned os9left_ui_export_count = sizeof os9left_ui_exports / sizeof os9left_ui_exports[0];
