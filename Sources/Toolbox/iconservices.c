/*
 * Icon Services (Mac OS 8.5, IconServicesLib): IconRefs and 'icns' icon
 * families.
 *
 * An icon family is a handle in the 'icns' format: the type 'icns' and the
 * total length, then elements, each its type, its length (header included)
 * and its data: the Icon Utilities' members (ICN#, icl4, icl8, ics#, ics4,
 * ics8, icm#, icm4, icm8, and the 48-pixel ich#, ich4, ich8), 32-bit colour
 * (il32, is32, ih32, it32: one run-length coded plane per channel) and
 * 8-bit masks (l8mk, s8mk, h8mk, t8mk).
 *
 * An IconRef is a small system-heap block standing for an entry here: its
 * icons as a family, the creator and type it was found or registered by,
 * and its owners.  GetIconRef finds the Finder's icons: ours from the
 * System file for the system creator ('macs') and generic types, and an
 * application's bundle (its BNDL and FREFs) for the types it owns, the
 * application found by creator as the desktop database does.  Files and
 * folders get their custom icons (resource -16455, a folder's in its
 * "Icon\r" file) when they have them.  Entries found by creator and type
 * stay cached when their last owner releases them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "icons_internal.h"
#include "qd_internal.h"
#include "tb_internal.h"

enum { invalidIconRefErr = -2580, noSuchIconErr = -2581, noIconDataAvailableErr = -2582 };
enum { kCustomIconResource = -16455, kHasCustomIcon = 0x0400 };

#define SYSTEM_CREATOR 0x6D616373u /* 'macs', kSystemIconsCreator */

/* ---------------------------------------------------------------------- */
/* Families in host memory */

typedef struct {
    uint8_t *b;
    uint32_t n;
} fbuf;

static void fb_init(fbuf *f)
{
    f->n = 8;
    f->b = malloc(8);
    memcpy(f->b, "icns\0\0\0\x08", 8);
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* An element's data, or NULL. */
static const uint8_t *fb_find(const fbuf *f, uint32_t type, uint32_t *len)
{
    for (uint32_t off = 8; off + 8 <= f->n;) {
        uint32_t n = be32(f->b + off + 4);
        if (n < 8 || off + n > f->n)
            break;
        if (be32(f->b + off) == type) {
            *len = n - 8;
            return f->b + off + 8;
        }
        off += n;
    }
    return NULL;
}

static void fb_remove(fbuf *f, uint32_t type)
{
    for (uint32_t off = 8; off + 8 <= f->n;) {
        uint32_t n = be32(f->b + off + 4);
        if (n < 8 || off + n > f->n)
            break;
        if (be32(f->b + off) == type) {
            memmove(f->b + off, f->b + off + n, f->n - off - n);
            f->n -= n;
            put_be32(f->b + 4, f->n);
            return;
        }
        off += n;
    }
}

/* Add an element, replacing one of the same type. */
static void fb_put(fbuf *f, uint32_t type, const uint8_t *d, uint32_t len)
{
    fb_remove(f, type);
    f->b = realloc(f->b, f->n + 8 + len);
    put_be32(f->b + f->n, type);
    put_be32(f->b + f->n + 4, len + 8);
    memcpy(f->b + f->n + 8, d, len);
    f->n += 8 + len;
    put_be32(f->b + 4, f->n);
}

/* A guest family handle's contents; an empty family if it isn't one. */
static bool fb_load(gaddr fam, fbuf *f)
{
    fb_init(f);
    if (!fam || !rd32(fam))
        return false;
    uint32_t n = (uint32_t)tb_GetHandleSize(fam);
    if (n < 8 || rd32(rd32(fam)) != fourcc("icns"))
        return false;
    uint32_t total = rd32(rd32(fam) + 4);
    if (total >= 8 && total < n)
        n = total;
    f->b = realloc(f->b, n);
    gget(f->b, rd32(fam), n);
    f->n = n;
    put_be32(f->b + 4, n);
    return true;
}

static gaddr fb_handle(const fbuf *f, bool sys)
{
    gaddr h = tb_NewHandle((int32_t)f->n, sys, false);
    if (h)
        gput(rd32(h), f->b, f->n);
    return h;
}

/* Back into an existing handle. */
static void fb_store(const fbuf *f, gaddr fam)
{
    tb_SetHandleSize(fam, (int32_t)f->n);
    if (rd32(fam) && (uint32_t)tb_GetHandleSize(fam) == f->n)
        gput(rd32(fam), f->b, f->n);
}

/* ---------------------------------------------------------------------- */
/* Element types and the selector bits that choose them */

static const struct {
    const char *type;
    uint32_t bit;
} kinds[] = {
    { "ICN#", 0x1 }, { "icl8", 0x2 }, { "icl4", 0x4 }, { "il32", 0x8 }, { "l8mk", 0x10 },
    { "ics#", 0x100 }, { "ics8", 0x200 }, { "ics4", 0x400 }, { "is32", 0x800 }, { "s8mk", 0x1000 },
    { "icm#", 0x10000 }, { "icm8", 0x20000 }, { "icm4", 0x40000 },
    { "ich#", 0x1000000 }, { "ich8", 0x2000000 }, { "ich4", 0x4000000 }, { "ih32", 0x8000000 }, { "h8mk", 0x10000000 },
    /* Thumbnails come only with all the data. */
    { "it32", 0 }, { "t8mk", 0 },
};

static bool selected(uint32_t type, uint32_t which)
{
    if (which == 0xFFFFFFFFu)
        return true;
    for (unsigned i = 0; i < sizeof kinds / sizeof kinds[0]; i++)
        if (fourcc(kinds[i].type) == type)
            return (which & kinds[i].bit) != 0;
    return false;
}

static uint32_t kind_bits(const fbuf *f)
{
    uint32_t bits = 0, len;
    for (unsigned i = 0; i < sizeof kinds / sizeof kinds[0]; i++)
        if (fb_find(f, fourcc(kinds[i].type), &len))
            bits |= kinds[i].bit;
    return bits;
}

/* The elements of `src` chosen by `which`, as a new family. */
static void fb_select(const fbuf *src, uint32_t which, fbuf *out)
{
    fb_init(out);
    for (uint32_t off = 8; off + 8 <= src->n;) {
        uint32_t n = be32(src->b + off + 4);
        if (n < 8 || off + n > src->n)
            break;
        if (selected(be32(src->b + off), which))
            fb_put(out, be32(src->b + off), src->b + off + 8, n - 8);
        off += n;
    }
}

/* ---------------------------------------------------------------------- */
/* Families from resources */

static bool handle_data(gaddr h, uint8_t **d, uint32_t *n)
{
    if (!h)
        return false;
    if (!rd32(h))
        tb_LoadResource(h);
    if (!rd32(h))
        return false;
    *n = (uint32_t)tb_GetHandleSize(h);
    *d = malloc(*n ? *n : 1);
    gget(*d, rd32(h), *n);
    return true;
}

static const char *const classic[] = { "ICN#", "icl4", "icl8", "ics#", "ics4", "ics8", "icm#", "icm4", "icm8" };

/* The icon resources with an ID, from the current resource file alone or
 * the whole chain, and an 'icns' resource of that ID merged in. */
static bool fb_from_resources(int16_t id, bool one, fbuf *f)
{
    fb_init(f);
    bool any = false;
    uint8_t *d;
    uint32_t n;
    for (unsigned i = 0; i < sizeof classic / sizeof classic[0]; i++) {
        uint32_t t = fourcc(classic[i]);
        gaddr h = one ? tb_Get1Resource((int32_t)t, id) : tb_GetResource((int32_t)t, id);
        if (handle_data(h, &d, &n)) {
            fb_put(f, t, d, n);
            free(d);
            any = true;
        }
    }
    gaddr h = one ? tb_Get1Resource((int32_t)fourcc("icns"), id) : tb_GetResource((int32_t)fourcc("icns"), id);
    if (handle_data(h, &d, &n)) {
        fbuf r = { d, n };
        if (n >= 8 && be32(d) == fourcc("icns")) {
            if (be32(d + 4) >= 8 && be32(d + 4) < n)
                r.n = be32(d + 4);
            for (uint32_t off = 8; off + 8 <= r.n;) {
                uint32_t en = be32(d + off + 4);
                if (en < 8 || off + en > r.n)
                    break;
                fb_put(f, be32(d + off), d + off + 8, en - 8);
                any = true;
                off += en;
            }
        }
        free(d);
    }
    return any;
}

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* The icon resource ID an application's bundle gives a file type: its
 * FREF for the type names a local ID, which the BNDL's ICN# list maps. */
static bool bundle_icon_id(uint32_t ftype, int16_t *id)
{
    int16_t local = 0;
    bool found = false;
    int nfref = tb_Count1Resources((int32_t)fourcc("FREF"));
    for (int i = 1; i <= nfref && !found; i++) {
        uint8_t *d;
        uint32_t n;
        if (!handle_data(tb_Get1IndResource((int32_t)fourcc("FREF"), (int16_t)i), &d, &n))
            continue;
        if (n >= 6 && be32(d) == ftype) {
            local = (int16_t)u16(d + 4);
            found = true;
        }
        free(d);
    }
    if (!found)
        return false;
    uint8_t *d;
    uint32_t n;
    if (!handle_data(tb_Get1IndResource((int32_t)fourcc("BNDL"), 1), &d, &n))
        return false;
    found = false;
    if (n >= 8) {
        uint32_t off = 8;
        int ntypes = (int16_t)u16(d + 6) + 1;
        for (int t = 0; t < ntypes && off + 6 <= n && !found; t++) {
            uint32_t type = be32(d + off);
            int count = (int16_t)u16(d + off + 4) + 1;
            off += 6;
            for (int k = 0; k < count && off + 4 <= n; k++, off += 4)
                if (type == fourcc("ICN#") && (int16_t)u16(d + off) == local) {
                    *id = (int16_t)u16(d + off + 2);
                    found = true;
                    break;
                }
        }
    }
    free(d);
    return found;
}

/* Icons from a file's resource fork, opened read-only (a map of its own,
 * even for the running application's file): resource `id`, or the
 * bundle's icons for `bundle_type` when that is nonzero. */
static bool fb_from_file(int16_t vref, int32_t dir, gaddr name, int16_t id, uint32_t bundle_type, fbuf *f)
{
    int16_t cur = tb_CurResFile();
    int16_t err = (int16_t)rd16(LM_ResErr);
    int16_t rn = tb_HOpenResFile(vref, dir, name, 1);
    bool ok = false;
    if (rn > 0) {
        tb_UseResFile(rn);
        if (!bundle_type || bundle_icon_id(bundle_type, &id))
            ok = fb_from_resources(id, true, f);
        tb_CloseResFile(rn);
        tb_UseResFile(cur);
    }
    wr16(LM_ResErr, (uint16_t)err);
    if (!ok)
        fb_init(f);
    return ok;
}

static bool fb_from_spec(gaddr spec, int16_t id, uint32_t bundle_type, fbuf *f)
{
    return fb_from_file((int16_t)rd16(spec + FSSpec_vRefNum_off), (int32_t)rd32(spec + FSSpec_parID_off),
                        spec + FSSpec_name_off, id, bundle_type, f);
}

static bool fb_from_path(const char *path, uint32_t bundle_type, fbuf *f)
{
    gaddr spec = tb_NewPtr(FSSpec_SIZEOF, false, true);
    fm_spec_for_path(path, spec);
    bool ok = fb_from_spec(spec, 0, bundle_type, f);
    tb_DisposePtr(spec);
    return ok;
}

/* Our System file's icons for the system creator's types. */
static const struct {
    const char *type;
    int16_t id;
} system_icons[] = {
    { "docu", -4000 }, { "APPL", -3996 }, { "fldr", -3999 }, { "ofld", -3997 }, { "flpy", -3998 },
    { "hdsk", -3995 }, { "trsh", -3993 }, { "ftrh", -3984 }, { "desk", -3992 }, { "macs", -3983 },
    { "amnu", -3982 }, { "strt", -3981 }, { "ownd", -3980 }, { "drop", -3979 }, { "mntd", -3978 },
    { "shfl", -3977 }, { "ctrl", -3976 }, { "prnt", -3975 }, { "prff", -3974 }, { "extn", -3973 },
    { "font", -3968 },
    /* No icons of their own here: like the nearest that is. */
    { "APPC", -3996 }, { "APPD", -3996 }, { "appe", -3996 }, { "cdev", -3996 }, { "dfil", -3996 },
    { "cddr", -3995 }, { "ramd", -3995 }, { "srvr", -3995 }, { "shrd", -3995 },
    { "INIT", -4000 }, { "pref", -4000 }, { "sdoc", -4000 }, { "edtf", -4000 }, { "qery", -4000 },
    { "ffil", -4000 }, { "tfil", -4000 }, { "FFIL", -4000 }, { "suit", -4000 }, { "shlb", -4000 },
    { "thng", -4000 }, { "clpt", -4000 }, { "clpp", -4000 }, { "pref", -4000 }, { "dbfl", -4000 },
    { "sfil", -4000 }, { "fdrp", -3999 }, { "pubf", -3999 },
};

static bool system_icon_id(uint32_t type, int16_t *id)
{
    for (unsigned i = 0; i < sizeof system_icons / sizeof system_icons[0]; i++)
        if (fourcc(system_icons[i].type) == type) {
            *id = system_icons[i].id;
            return true;
        }
    return false;
}

static bool fb_from_system(int16_t id, fbuf *f)
{
    int16_t cur = tb_CurResFile();
    int16_t err = (int16_t)rd16(LM_ResErr);
    tb_UseResFile((int16_t)rd16(LM_SysMap));
    bool ok = fb_from_resources(id, true, f);
    tb_UseResFile(cur);
    wr16(LM_ResErr, (uint16_t)err);
    return ok;
}

/* ---------------------------------------------------------------------- */
/* IconRefs */

typedef struct {
    gaddr ref;              /* the IconRef handed out; 0 for a free slot */
    uint32_t creator, type; /* what it is found by (keyed) */
    bool keyed, registered;
    gaddr family;           /* its icons, in the system heap */
    uint16_t owners;
    gaddr override;         /* an IconRef shown in its place */
    gaddr bg, fg;           /* a composite's two */
} isref;

static isref *refs;
static int nrefs;
static bool custom_icons = true;

void iconservices_reset(void)
{
    free(refs);
    refs = NULL;
    nrefs = 0;
    custom_icons = true;
}

static isref *lookup(gaddr ref)
{
    if (!ref)
        return NULL;
    for (int i = 0; i < nrefs; i++)
        if (refs[i].ref == ref)
            return &refs[i];
    return NULL;
}

static isref *find_keyed(uint32_t creator, uint32_t type)
{
    for (int i = 0; i < nrefs; i++)
        if (refs[i].ref && refs[i].keyed && refs[i].creator == creator && refs[i].type == type)
            return &refs[i];
    return NULL;
}

/* A new entry owning a family made from `f` (one owner). */
static isref *new_ref(const fbuf *f)
{
    int i = 0;
    while (i < nrefs && refs[i].ref)
        i++;
    if (i == nrefs) {
        refs = realloc(refs, (size_t)(nrefs + 1) * sizeof *refs);
        nrefs++;
    }
    isref *e = &refs[i];
    memset(e, 0, sizeof *e);
    e->ref = tb_NewPtr(8, true, true);
    e->family = fb_handle(f, true);
    if (!e->ref || !e->family) {
        if (e->ref)
            tb_DisposePtr(e->ref);
        if (e->family)
            tb_DisposeHandle(e->family);
        e->ref = 0;
        return NULL;
    }
    gput(e->ref, "IcRf", 4);
    e->owners = 1;
    return e;
}

static void release(isref *e);

static void free_ref(isref *e)
{
    gaddr over = e->override, bg = e->bg, fg = e->fg;
    tb_DisposeHandle(e->family);
    tb_DisposePtr(e->ref);
    memset(e, 0, sizeof *e);
    release(lookup(over));
    release(lookup(bg));
    release(lookup(fg));
}

/* One owner fewer: an entry that can't be found again goes. */
static void release(isref *e)
{
    if (!e)
        return;
    if (e->owners)
        e->owners--;
    if (!e->owners && !e->keyed && !e->registered)
        free_ref(e);
}

/* What is drawn for an entry: its override's, if it has one. */
static isref *shown(isref *e)
{
    for (int i = 0; e && e->override && i < 8; i++) {
        isref *o = lookup(e->override);
        if (!o)
            break;
        e = o;
    }
    return e;
}

static int16_t hand_out(isref *e, gaddr out)
{
    if (out)
        wr32(out, e ? e->ref : 0);
    return e ? noErr : memFullErr;
}

static isref *keyed_ref(uint32_t creator, uint32_t type, const fbuf *f)
{
    isref *e = new_ref(f);
    if (e) {
        e->creator = creator;
        e->type = type;
        e->keyed = true;
    }
    return e;
}

/* The entry for a creator and type, with an owner added. */
static isref *icon_for(uint32_t creator, uint32_t type)
{
    isref *e = find_keyed(creator, type);
    if (e) {
        e->owners++;
        return e;
    }
    fbuf f = { NULL, 0 };
    int16_t id;
    char app[1024];
    bool ok = false;
    if (creator == SYSTEM_CREATOR) {
        ok = system_icon_id(type, &id) && fb_from_system(id, &f);
    } else if (creator && fm_app_for_creator(creator, app, sizeof app)) {
        ok = fb_from_path(app, type, &f);
    }
    if (ok) {
        e = keyed_ref(creator, type, &f);
        free(f.b);
        return e;
    }
    free(f.b);
    /* The generic icon for the type: the system's. */
    if (creator == SYSTEM_CREATOR) {
        if (type == fourcc("docu"))
            return NULL; /* no System file icons at all */
        return icon_for(SYSTEM_CREATOR, fourcc("docu"));
    }
    if (!system_icon_id(type, &id))
        type = fourcc("docu");
    return icon_for(SYSTEM_CREATOR, type);
}

int16_t tb_GetIconRef(int16_t vRefNum, int32_t creator, int32_t iconType, gaddr theIconRef)
{
    isref *e = icon_for(creator ? (uint32_t)creator : SYSTEM_CREATOR, (uint32_t)iconType);
    if (!e) {
        if (theIconRef)
            wr32(theIconRef, 0);
        return noSuchIconErr;
    }
    return hand_out(e, theIconRef);
}

/* Catalog information for an item: a scratch CInfoPBRec (the caller
 * disposes of it), its name copied so the call leaves the caller's alone. */
static gaddr cat_info(int16_t vref, int32_t dir, gaddr name, int16_t index, int16_t *err)
{
    gaddr pb = tb_NewPtr(CInfoPBRec_SIZEOF + 256, false, true);
    if (!pb) {
        *err = memFullErr;
        return 0;
    }
    gaddr nm = pb + CInfoPBRec_SIZEOF;
    if (name)
        gmemcpy(nm, name, (uint32_t)rd8(name) + 1);
    wr32(pb + HFileInfo_ioNamePtr_off, nm);
    wr16(pb + HFileInfo_ioVRefNum_off, (uint16_t)vref);
    wr32(pb + HFileInfo_ioDirID_off, (uint32_t)dir);
    wr16(pb + HFileInfo_ioFDirIndex_off, (uint16_t)index);
    *err = tb_PBGetCatInfo(pb, false);
    return pb;
}

/* A folder's icon: its custom icon, the disk's for the root, or the
 * generic folder's. */
static isref *folder_icon(int16_t vref, int32_t dirid, uint16_t flags)
{
    if (custom_icons && (flags & kHasCustomIcon)) {
        gaddr name = tb_NewPtr(8, false, true);
        gput(name, "\x05Icon\r", 6);
        fbuf f;
        bool ok = fb_from_file(vref, dirid, name, kCustomIconResource, 0, &f);
        tb_DisposePtr(name);
        isref *e = ok ? new_ref(&f) : NULL;
        free(f.b);
        if (e)
            return e;
    }
    return icon_for(SYSTEM_CREATOR, fourcc(dirid == 2 ? "hdsk" : "fldr"));
}

int16_t tb_GetIconRefFromFile(gaddr theFile, gaddr theIconRef, gaddr theLabel)
{
    if (theIconRef)
        wr32(theIconRef, 0);
    if (!theFile || !theIconRef)
        return paramErr;
    int16_t vref = (int16_t)rd16(theFile + FSSpec_vRefNum_off);
    int32_t parid = (int32_t)rd32(theFile + FSSpec_parID_off);
    gaddr name = theFile + FSSpec_name_off;
    int16_t err;
    gaddr pb = cat_info(vref, parid, rd8(name) ? name : 0, rd8(name) ? 0 : -1, &err);
    if (!pb)
        return err;
    if (err) {
        tb_DisposePtr(pb);
        return err;
    }
    isref *e;
    uint16_t flags;
    if (rd8(pb + HFileInfo_ioFlAttrib_off) & 0x10) {
        flags = rd16(pb + DirInfo_ioDrFndrInfo_off + DInfo_frFlags_off);
        e = folder_icon(vref, (int32_t)rd32(pb + DirInfo_ioDrDirID_off), flags);
    } else {
        gaddr fi = pb + HFileInfo_ioFlFndrInfo_off;
        uint32_t type = rd32(fi + FInfo_fdType_off), creator = rd32(fi + FInfo_fdCreator_off);
        flags = rd16(fi + FInfo_fdFlags_off);
        e = NULL;
        fbuf f = { NULL, 0 };
        if (custom_icons && (flags & kHasCustomIcon)) {
            if (fb_from_spec(theFile, kCustomIconResource, 0, &f))
                e = new_ref(&f);
            free(f.b);
        }
        /* An application's own icon, from its bundle: kept for its creator. */
        if (!e && type == fourcc("APPL")) {
            if (creator && (e = find_keyed(creator, type))) {
                e->owners++;
            } else {
                if (fb_from_spec(theFile, 0, type, &f))
                    e = creator ? keyed_ref(creator, type, &f) : new_ref(&f);
                free(f.b);
            }
        }
        if (!e)
            e = icon_for(creator, type);
    }
    tb_DisposePtr(pb);
    if (theLabel)
        wr16(theLabel, (uint16_t)((flags >> 1) & 7));
    if (!e)
        return noSuchIconErr;
    return hand_out(e, theIconRef);
}

int16_t tb_GetIconRefFromFolder(int16_t vRefNum, int32_t parentFolderID, int32_t folderID, int8_t attributes,
                                int8_t accessPrivileges, gaddr theIconRef)
{
    uint16_t flags = 0;
    int16_t err;
    gaddr pb = cat_info(vRefNum, folderID, 0, -1, &err);
    if (pb && !err)
        flags = rd16(pb + DirInfo_ioDrFndrInfo_off + DInfo_frFlags_off);
    if (pb)
        tb_DisposePtr(pb);
    isref *e = folder_icon(vRefNum, folderID, flags);
    if (!e) {
        if (theIconRef)
            wr32(theIconRef, 0);
        return noSuchIconErr;
    }
    return hand_out(e, theIconRef);
}

/* Register: the entry for a creator and type takes these icons. */
static int16_t register_family(uint32_t creator, uint32_t type, const fbuf *f, gaddr theIconRef)
{
    isref *e = find_keyed(creator, type);
    if (e) {
        gaddr h = fb_handle(f, true);
        if (!h)
            return memFullErr;
        tb_DisposeHandle(e->family);
        e->family = h;
        e->owners++;
    } else if (!(e = keyed_ref(creator, type, f))) {
        return memFullErr;
    }
    e->registered = true;
    return hand_out(e, theIconRef);
}

int16_t tb_RegisterIconRefFromIconFamily(int32_t creator, int32_t iconType, gaddr iconFamily, gaddr theIconRef)
{
    fbuf f = { NULL, 0 };
    if (!fb_load(iconFamily, &f)) {
        free(f.b);
        return paramErr;
    }
    int16_t err = register_family((uint32_t)creator, (uint32_t)iconType, &f, theIconRef);
    free(f.b);
    return err;
}

int16_t tb_RegisterIconRefFromResource(int32_t creator, int32_t iconType, gaddr resourceFile, int16_t resourceID,
                                       gaddr theIconRef)
{
    fbuf f = { NULL, 0 };
    bool ok = resourceFile ? fb_from_spec(resourceFile, resourceID, 0, &f) : fb_from_resources(resourceID, false, &f);
    int16_t err = ok ? register_family((uint32_t)creator, (uint32_t)iconType, &f, theIconRef) : resNotFound;
    free(f.b);
    return err;
}

/* An 'icns' file's data fork. */
static int16_t read_icon_file(gaddr spec, fbuf *f)
{
    fb_init(f);
    char path[1024];
    if (!spec || !fm_host_path((int16_t)rd16(spec + FSSpec_vRefNum_off), (int32_t)rd32(spec + FSSpec_parID_off),
                               spec + FSSpec_name_off, true, path, sizeof path))
        return fnfErr;
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return fnfErr;
    uint8_t hdr[8];
    int16_t err = paramErr;
    if (fread(hdr, 1, 8, fp) == 8 && be32(hdr) == fourcc("icns") && be32(hdr + 4) >= 8 &&
        be32(hdr + 4) < 64u << 20) {
        uint32_t n = be32(hdr + 4);
        f->b = realloc(f->b, n);
        memcpy(f->b, hdr, 8);
        if (fread(f->b + 8, 1, n - 8, fp) == n - 8) {
            f->n = n;
            err = noErr;
        }
    }
    fclose(fp);
    if (err) {
        free(f->b);
        fb_init(f);
    }
    return err;
}

int16_t tb_RegisterIconRefFromIconFile(int32_t creator, int32_t iconType, gaddr iconFile, gaddr theIconRef)
{
    fbuf f = { NULL, 0 };
    int16_t err = read_icon_file(iconFile, &f);
    if (!err)
        err = register_family((uint32_t)creator, (uint32_t)iconType, &f, theIconRef);
    free(f.b);
    return err;
}

int16_t tb_ReadIconFile(gaddr iconFile, gaddr iconFamily)
{
    fbuf f = { NULL, 0 };
    int16_t err = read_icon_file(iconFile, &f);
    gaddr h = err ? 0 : fb_handle(&f, false);
    free(f.b);
    if (!err && !h)
        err = memFullErr;
    if (iconFamily)
        wr32(iconFamily, h);
    return err;
}

int16_t tb_WriteIconFile(gaddr iconFamily, gaddr iconFile)
{
    fbuf f = { NULL, 0 };
    if (!iconFile || !fb_load(iconFamily, &f)) {
        free(f.b);
        return paramErr;
    }
    tb_FSpCreate(iconFile, (int32_t)fourcc("????"), (int32_t)fourcc("icns"), 0);
    char path[1024];
    int16_t err = ioErr;
    if (fm_host_path((int16_t)rd16(iconFile + FSSpec_vRefNum_off), (int32_t)rd32(iconFile + FSSpec_parID_off),
                     iconFile + FSSpec_name_off, true, path, sizeof path)) {
        FILE *fp = fopen(path, "wb");
        if (fp) {
            if (fwrite(f.b, 1, f.n, fp) == f.n)
                err = noErr;
            fclose(fp);
            as_file_changed(path);
        }
    } else {
        err = fnfErr;
    }
    free(f.b);
    return err;
}

int16_t tb_UnregisterIconRef(int32_t creator, int32_t iconType)
{
    isref *e = find_keyed((uint32_t)creator, (uint32_t)iconType);
    if (!e || !e->registered)
        return noSuchIconErr;
    e->registered = false;
    e->keyed = false;
    if (!e->owners)
        free_ref(e);
    return noErr;
}

int16_t tb_UpdateIconRef(gaddr theIconRef) { return lookup(theIconRef) ? noErr : invalidIconRefErr; }

static int16_t set_override(isref *e, isref *o)
{
    o->owners++;
    gaddr old = e->override;
    e->override = o->ref;
    release(lookup(old));
    return noErr;
}

int16_t tb_OverrideIconRefFromResource(gaddr theIconRef, gaddr resourceFile, int16_t resourceID)
{
    isref *e = lookup(theIconRef);
    if (!e)
        return invalidIconRefErr;
    fbuf f = { NULL, 0 };
    bool ok = resourceFile ? fb_from_spec(resourceFile, resourceID, 0, &f) : fb_from_resources(resourceID, false, &f);
    isref *o = ok ? new_ref(&f) : NULL;
    free(f.b);
    if (!o)
        return ok ? memFullErr : resNotFound;
    e = lookup(theIconRef); /* the table may have moved */
    set_override(e, o);
    release(o);
    return noErr;
}

int16_t tb_OverrideIconRef(gaddr oldIconRef, gaddr newIconRef)
{
    isref *e = lookup(oldIconRef), *o = lookup(newIconRef);
    if (!e || !o)
        return invalidIconRefErr;
    if (e == o)
        return paramErr;
    return set_override(e, o);
}

int16_t tb_RemoveIconRefOverride(gaddr theIconRef)
{
    isref *e = lookup(theIconRef);
    if (!e)
        return invalidIconRefErr;
    gaddr old = e->override;
    e->override = 0;
    release(lookup(old));
    return noErr;
}

int16_t tb_CompositeIconRef(gaddr backgroundIconRef, gaddr foregroundIconRef, gaddr compositeIconRef)
{
    if (compositeIconRef)
        wr32(compositeIconRef, 0);
    if (!lookup(backgroundIconRef) || !lookup(foregroundIconRef))
        return invalidIconRefErr;
    /* Its family is the background's. */
    fbuf f = { NULL, 0 };
    fb_load(lookup(backgroundIconRef)->family, &f);
    isref *e = new_ref(&f);
    free(f.b);
    if (!e)
        return memFullErr;
    e->bg = backgroundIconRef;
    e->fg = foregroundIconRef;
    lookup(backgroundIconRef)->owners++;
    lookup(foregroundIconRef)->owners++;
    return hand_out(e, compositeIconRef);
}

int16_t tb_IsIconRefComposite(gaddr compositeIconRef, gaddr backgroundIconRef, gaddr foregroundIconRef)
{
    isref *e = lookup(compositeIconRef);
    if (!e)
        return invalidIconRefErr;
    if (backgroundIconRef)
        wr32(backgroundIconRef, e->bg);
    if (foregroundIconRef)
        wr32(foregroundIconRef, e->fg);
    return noErr;
}

int8_t tb_IsValidIconRef(gaddr theIconRef) { return lookup(theIconRef) != NULL; }

int16_t tb_AcquireIconRef(gaddr theIconRef)
{
    isref *e = lookup(theIconRef);
    if (!e)
        return invalidIconRefErr;
    e->owners++;
    return noErr;
}

int16_t tb_ReleaseIconRef(gaddr theIconRef)
{
    isref *e = lookup(theIconRef);
    if (!e)
        return invalidIconRefErr;
    release(e);
    return noErr;
}

int16_t tb_GetIconRefOwners(gaddr theIconRef, gaddr owners)
{
    isref *e = lookup(theIconRef);
    if (!e)
        return invalidIconRefErr;
    if (owners)
        wr16(owners, e->owners);
    return noErr;
}

int16_t tb_FlushIconRefs(int32_t creator, int32_t iconType)
{
    isref *e = find_keyed((uint32_t)creator, (uint32_t)iconType);
    if (e && !e->owners && !e->registered)
        free_ref(e);
    return noErr;
}

int16_t tb_FlushIconRefsByVolume(int16_t vRefNum)
{
    for (int i = 0; i < nrefs; i++)
        if (refs[i].ref && !refs[i].owners && !refs[i].registered)
            free_ref(&refs[i]);
    return noErr;
}

int16_t tb_SetCustomIconsEnabled(int16_t vRefNum, int8_t enableCustomIcons)
{
    custom_icons = enableCustomIcons != 0;
    return noErr;
}

int16_t tb_GetCustomIconsEnabled(int16_t vRefNum, gaddr customIconsEnabled)
{
    if (customIconsEnabled)
        wr8(customIconsEnabled, custom_icons);
    return noErr;
}

/* ---------------------------------------------------------------------- */
/* Families: conversions and their data */

int16_t tb_IconRefToIconFamily(gaddr theIconRef, uint32_t whichIcons, gaddr iconFamily)
{
    if (iconFamily)
        wr32(iconFamily, 0);
    isref *e = shown(lookup(theIconRef));
    if (!e)
        return invalidIconRefErr;
    fbuf f, sel;
    fb_load(e->family, &f);
    fb_select(&f, whichIcons, &sel);
    gaddr h = fb_handle(&sel, false);
    free(f.b);
    free(sel.b);
    if (!h)
        return memFullErr;
    if (iconFamily)
        wr32(iconFamily, h);
    return noErr;
}

int16_t tb_IconFamilyToIconSuite(gaddr iconFamily, uint32_t whichIcons, gaddr iconSuite)
{
    if (iconSuite)
        wr32(iconSuite, 0);
    fbuf f = { NULL, 0 };
    if (!fb_load(iconFamily, &f)) {
        free(f.b);
        return paramErr;
    }
    gaddr sp = tb_NewPtr(4, false, true);
    tb_NewIconSuite(sp);
    gaddr suite = rd32(sp);
    tb_DisposePtr(sp);
    if (!suite) {
        free(f.b);
        return memFullErr;
    }
    for (uint32_t off = 8; off + 8 <= f.n;) {
        uint32_t n = be32(f.b + off + 4), type = be32(f.b + off);
        if (n < 8 || off + n > f.n)
            break;
        if (selected(type, whichIcons)) {
            gaddr h = tb_NewHandle((int32_t)(n - 8), false, false);
            if (h) {
                gput(rd32(h), f.b + off + 8, n - 8);
                tb_AddIconToSuite(h, suite, (int32_t)type);
            }
        }
        off += n;
    }
    free(f.b);
    if (iconSuite)
        wr32(iconSuite, suite);
    return noErr;
}

int16_t tb_IconSuiteToIconFamily(gaddr iconSuite, uint32_t whichIcons, gaddr iconFamily)
{
    if (iconFamily)
        wr32(iconFamily, 0);
    if (!iconSuite || !rd32(iconSuite))
        return paramErr;
    fbuf f = { NULL, 0 };
    fb_init(&f);
    gaddr s = rd32(iconSuite);
    int count = (int16_t)rd16(s + 2);
    for (int i = 0; i < count; i++) {
        s = rd32(iconSuite);
        uint32_t type = rd32(s + 4 + (uint32_t)i * 8);
        gaddr h = rd32(s + 4 + (uint32_t)i * 8 + 4);
        uint8_t *d;
        uint32_t n;
        if (selected(type, whichIcons) && handle_data(h, &d, &n)) {
            fb_put(&f, type, d, n);
            free(d);
        }
    }
    gaddr h = fb_handle(&f, false);
    free(f.b);
    if (!h)
        return memFullErr;
    if (iconFamily)
        wr32(iconFamily, h);
    return noErr;
}

int16_t tb_SetIconFamilyData(gaddr iconFamily, int32_t iconType, gaddr h)
{
    fbuf f = { NULL, 0 };
    if (!h || !rd32(h) || !fb_load(iconFamily, &f)) {
        free(f.b);
        return paramErr;
    }
    uint32_t n = (uint32_t)tb_GetHandleSize(h);
    uint8_t *d = malloc(n ? n : 1);
    gget(d, rd32(h), n);
    fb_put(&f, (uint32_t)iconType, d, n);
    free(d);
    fb_store(&f, iconFamily);
    free(f.b);
    return (uint32_t)tb_GetHandleSize(iconFamily) == f.n ? noErr : memFullErr;
}

int16_t tb_GetIconFamilyData(gaddr iconFamily, int32_t iconType, gaddr h)
{
    fbuf f = { NULL, 0 };
    if (!h || !fb_load(iconFamily, &f)) {
        free(f.b);
        return paramErr;
    }
    uint32_t n = 0;
    const uint8_t *d = fb_find(&f, (uint32_t)iconType, &n);
    /* Data it doesn't have: an empty handle. */
    tb_SetHandleSize(h, d ? (int32_t)n : 0);
    int16_t err = noErr;
    if (d && rd32(h) && (uint32_t)tb_GetHandleSize(h) == n)
        gput(rd32(h), d, n);
    else if (d)
        err = memFullErr;
    free(f.b);
    return err;
}

int16_t tb_GetIconSizesFromIconRef(uint32_t iconSelectorInput, gaddr iconSelectorOutputPtr,
                                   uint32_t iconServicesUsageFlags, gaddr theIconRef)
{
    isref *e = shown(lookup(theIconRef));
    if (!e)
        return invalidIconRefErr;
    fbuf f = { NULL, 0 };
    fb_load(e->family, &f);
    uint32_t bits = kind_bits(&f);
    free(f.b);
    if (iconSelectorOutputPtr)
        wr32(iconSelectorOutputPtr, bits & iconSelectorInput);
    return noErr;
}

int8_t tb_IsDataAvailableInIconRef(int32_t inIconKind, gaddr inIconRef)
{
    isref *e = shown(lookup(inIconRef));
    if (!e)
        return false;
    fbuf f = { NULL, 0 };
    fb_load(e->family, &f);
    uint32_t n;
    bool there = fb_find(&f, (uint32_t)inIconKind, &n) != NULL;
    free(f.b);
    return there;
}

gaddr tb_GetIconRefVariant(gaddr inIconRef, int32_t inVariant, gaddr outTransform)
{
    /* No variants here: the icon itself, untransformed. */
    if (outTransform)
        wr16(outTransform, 0);
    return inIconRef;
}

/* ---------------------------------------------------------------------- */
/* Drawing and hit testing */

/* 32-bit colour: three planes run-length coded (a byte n below $80 is
 * followed by n + 1 literal bytes; from $80, one byte repeated n - $7D
 * times), or plain ARGB when the data is exactly that size. */
static bool unpack32(const uint8_t *d, uint32_t n, int w, int h, uint8_t *rgba)
{
    uint32_t px = (uint32_t)(w * h);
    if (n == px * 4) {
        for (uint32_t i = 0; i < px; i++) {
            rgba[i * 4] = d[i * 4 + 1];
            rgba[i * 4 + 1] = d[i * 4 + 2];
            rgba[i * 4 + 2] = d[i * 4 + 3];
        }
        return true;
    }
    uint32_t pos = 0;
    for (int ch = 0; ch < 3; ch++) {
        uint32_t i = 0;
        while (i < px) {
            if (pos >= n)
                return false;
            uint8_t c = d[pos++];
            if (c < 0x80) {
                for (int k = 0; k <= c; k++) {
                    if (pos >= n)
                        return false;
                    if (i < px)
                        rgba[i++ * 4 + (uint32_t)ch] = d[pos];
                    pos++;
                }
            } else {
                if (pos >= n)
                    return false;
                uint8_t v = d[pos++];
                for (int k = 0; k < c - 0x7D; k++)
                    if (i < px)
                        rgba[i++ * 4 + (uint32_t)ch] = v;
            }
        }
    }
    return true;
}

static const struct {
    int w, h;
    const char *mono, *c4, *c8, *c32, *mask8;
} sizes[] = {
    { 128, 128, NULL, NULL, NULL, "it32", "t8mk" },
    { 48, 48, "ich#", "ich4", "ich8", "ih32", "h8mk" },
    { 32, 32, "ICN#", "icl4", "icl8", "il32", "l8mk" },
    { 16, 16, "ics#", "ics4", "ics8", "is32", "s8mk" },
    { 16, 12, "icm#", "icm4", "icm8", NULL, NULL },
};
enum { NSIZES = sizeof sizes / sizeof sizes[0] };

/* One size of a family, if it has that size. */
static bool family_size_image(const fbuf *f, int s, icon_img *img)
{
    int w = sizes[s].w, h = sizes[s].h;
    uint32_t plane = (uint32_t)(w / 8 * h), n;
    const uint8_t *mono = sizes[s].mono ? fb_find(f, fourcc(sizes[s].mono), &n) : NULL;
    if (mono && n < plane * 2)
        mono = NULL;
    const uint8_t *mask8 = sizes[s].mask8 ? fb_find(f, fourcc(sizes[s].mask8), &n) : NULL;
    if (mask8 && n < (uint32_t)(w * h))
        mask8 = NULL;
    uint32_t n32 = 0;
    const uint8_t *c32 = sizes[s].c32 ? fb_find(f, fourcc(sizes[s].c32), &n32) : NULL;
    if (c32 && s == 0) { /* thumbnails start with four zero bytes */
        if (n32 < 4) {
            c32 = NULL;
        } else {
            c32 += 4;
            n32 -= 4;
        }
    }
    if (!mono && !c32)
        return false;
    const uint8_t *mask1 = mono ? mono + plane : NULL;
    int depth = qd_screen_depth();
    uint32_t n8 = 0, n4 = 0;
    const uint8_t *c8 = sizes[s].c8 ? fb_find(f, fourcc(sizes[s].c8), &n8) : NULL;
    const uint8_t *c4 = sizes[s].c4 ? fb_find(f, fourcc(sizes[s].c4), &n4) : NULL;
    if (c8 && n8 < (uint32_t)(w * h))
        c8 = NULL;
    if (c4 && n4 < (uint32_t)(w * h / 2))
        c4 = NULL;
    bool drawn = false;
    /* Deep colour on deep screens; the indexed icons were drawn for their
     * depths, and are used there. */
    if (c32 && (depth > 8 || !(c8 || c4 || mono))) {
        memset(img->px, 0, (size_t)(w * h * 4));
        drawn = unpack32(c32, n32, w, h, img->px);
        if (drawn) {
            img->w = w;
            img->h = h;
            for (int i = 0; i < w * h; i++)
                img->px[i * 4 + 3] = 255;
            if (!mask8 && mask1)
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++)
                        if (!(mask1[y * (w / 8) + x / 8] & (0x80 >> (x % 8))))
                            img->px[(y * w + x) * 4 + 3] = 0;
        }
    }
    if (!drawn) {
        if (!mono)
            return false;
        if (depth >= 8 && c8)
            icon_render(img, w, h, 8, c8, mask1);
        else if (depth >= 4 && c4)
            icon_render(img, w, h, 4, c4, mask1);
        else
            icon_render(img, w, h, 1, mono, mask1);
    }
    /* An 8-bit mask: drawn where it is at least half on. */
    if (mask8)
        for (int i = 0; i < w * h; i++)
            img->px[i * 4 + 3] = mask8[i] >= 0x80 ? 255 : 0;
    return true;
}

/* The family's best image for a rectangle of `size` pixels: the smallest
 * at least that size, else the largest below it. */
static bool family_image(gaddr fam, int size, icon_img *img)
{
    fbuf f = { NULL, 0 };
    fb_load(fam, &f);
    bool ok = false;
    for (int s = NSIZES - 1; s >= 0 && !ok; s--)
        if (sizes[s].h >= size)
            ok = family_size_image(&f, s, img);
    for (int s = 0; s < NSIZES && !ok; s++)
        if (sizes[s].h < size)
            ok = family_size_image(&f, s, img);
    free(f.b);
    return ok;
}

/* `src` drawn over `dst`, scaled to it. */
static void overlay(icon_img *dst, const icon_img *src)
{
    for (int y = 0; y < dst->h; y++)
        for (int x = 0; x < dst->w; x++) {
            const uint8_t *p = src->px + ((y * src->h / dst->h) * src->w + x * src->w / dst->w) * 4;
            if (p[3])
                memcpy(dst->px + (y * dst->w + x) * 4, p, 4);
        }
}

static bool ref_image(isref *e, int size, icon_img *img, int depth)
{
    e = shown(e);
    if (!e || depth > 4)
        return false;
    isref *bg = lookup(e->bg), *fg = lookup(e->fg);
    if (bg && fg) {
        if (!ref_image(bg, size, img, depth + 1))
            return false;
        icon_img *top = malloc(sizeof *top);
        if (ref_image(fg, size, top, depth + 1))
            overlay(img, top);
        free(top);
        return true;
    }
    return family_image(e->family, size, img);
}

/* An IconRef's image placed for a rectangle: where it lands. */
static bool placed(gaddr theIconRef, gaddr iconRect, int16_t align, icon_img *img, qrect *dst)
{
    isref *e = lookup(theIconRef);
    if (!e || !iconRect)
        return false;
    qrect r = qrect_get(iconRect);
    int rw = r.right - r.left, rh = r.bottom - r.top;
    if (!ref_image(e, rw < rh ? rw : rh, img, 0))
        return false;
    *dst = icon_place(img, r, align);
    return dst->right > dst->left && dst->bottom > dst->top;
}

/* Is the icon drawn at (h, v)? */
static bool opaque_at(const icon_img *img, qrect dst, int h, int v)
{
    if (h < dst.left || h >= dst.right || v < dst.top || v >= dst.bottom)
        return false;
    int sx = (h - dst.left) * img->w / (dst.right - dst.left), sy = (v - dst.top) * img->h / (dst.bottom - dst.top);
    return img->px[(sy * img->w + sx) * 4 + 3] != 0;
}

int16_t tb_PlotIconRef(gaddr theRect, int16_t align, int16_t transform, uint32_t theIconServicesUsageFlags,
                       gaddr theIconRef)
{
    isref *e = lookup(theIconRef);
    if (!e)
        return invalidIconRefErr;
    if (!theRect)
        return paramErr;
    qrect r = qrect_get(theRect);
    int rw = r.right - r.left, rh = r.bottom - r.top;
    icon_img *img = malloc(sizeof *img);
    bool ok = ref_image(e, rw < rh ? rw : rh, img, 0);
    if (ok)
        icon_plot(img, r, align, transform);
    free(img);
    return ok ? noErr : noIconDataAvailableErr;
}

int32_t tb_PlotIconRefInContext(gaddr inContext, gaddr inRect, int16_t inAlign, int16_t inTransform,
                                gaddr inLabelColor, uint32_t inFlags, gaddr inIconRef)
{
    /* Into the current port: there are no Quartz contexts here. */
    return tb_PlotIconRef(inRect, inAlign, inTransform, 0, inIconRef);
}

int8_t tb_PtInIconRef(gaddr testPt, gaddr iconRect, int16_t align, uint32_t theIconServicesUsageFlags,
                      gaddr theIconRef)
{
    if (!testPt)
        return false;
    icon_img *img = malloc(sizeof *img);
    qrect dst;
    bool in = placed(theIconRef, iconRect, align, img, &dst) &&
              opaque_at(img, dst, (int16_t)rd16(testPt + 2), (int16_t)rd16(testPt));
    free(img);
    return in;
}

int8_t tb_RectInIconRef(gaddr testRect, gaddr iconRect, int16_t align, uint32_t iconServicesUsageFlags,
                        gaddr theIconRef)
{
    if (!testRect)
        return false;
    icon_img *img = malloc(sizeof *img);
    qrect dst, t = qrect_get(testRect);
    bool in = false;
    if (placed(theIconRef, iconRect, align, img, &dst)) {
        int top = t.top > dst.top ? t.top : dst.top, bottom = t.bottom < dst.bottom ? t.bottom : dst.bottom;
        int left = t.left > dst.left ? t.left : dst.left, right = t.right < dst.right ? t.right : dst.right;
        for (int v = top; v < bottom && !in; v++)
            for (int h = left; h < right && !in; h++)
                in = opaque_at(img, dst, h, v);
    }
    free(img);
    return in;
}

int16_t tb_IconRefToRgn(gaddr theRgn, gaddr iconRect, int16_t align, uint32_t iconServicesUsageFlags,
                        gaddr theIconRef)
{
    if (!lookup(theIconRef))
        return invalidIconRefErr;
    if (!theRgn || !iconRect)
        return paramErr;
    icon_img *img = malloc(sizeof *img);
    qrect dst;
    hrgn rgn;
    hrgn_init(&rgn);
    if (placed(theIconRef, iconRect, align, img, &dst)) {
        int dw = dst.right - dst.left;
        int16_t *xs = malloc(sizeof *xs * (size_t)(dw + 2));
        for (int v = dst.top; v < dst.bottom; v++) {
            int n = 0;
            bool on = false;
            for (int h = dst.left; h <= dst.right; h++) {
                bool o = h < dst.right && opaque_at(img, dst, h, v);
                if (o != on) {
                    xs[n++] = (int16_t)h;
                    on = o;
                }
            }
            if (n)
                hrgn_add_runs(&rgn, v, xs, n);
        }
        free(xs);
    }
    rgn_write(theRgn, &rgn);
    hrgn_free(&rgn);
    free(img);
    return noErr;
}

int8_t tb_IsIconRefMaskEmpty(gaddr iconRef)
{
    isref *e = lookup(iconRef);
    if (!e)
        return true;
    icon_img *img = malloc(sizeof *img);
    bool empty = true;
    if (ref_image(e, 32, img, 0))
        for (int i = 0; i < img->w * img->h && empty; i++)
            empty = img->px[i * 4 + 3] == 0;
    free(img);
    return empty;
}
