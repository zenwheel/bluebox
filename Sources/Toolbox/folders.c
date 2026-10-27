/*
 * The Folder Manager's descriptors and routings (FoldersLib, Mac OS 8.6
 * and later, and CarbonLib): what each folder type is and where it goes.
 * The standard folders' descriptors are the System Folder and the folders
 * filemgr.c keeps in it; a program adds its own for the run, and FindFolder
 * then finds a relative one by its name inside the folder it names as its
 * location, making it when asked to. Routings (where the Finder puts a
 * dropped file of a type) are kept as a list for the run.
 */
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "filemgr_internal.h"
#include "tb_internal.h"

enum {
    badFolderDescErr = -4270,
    duplicateFolderDescErr = -4271,
    invalidFolderTypeErr = -4273,
    duplicateRoutingErr = -4274,
    routingNotFoundErr = -4275,
    badRoutingSizeErr = -4276,
    fnfErrFolder = -43,
};
enum { FOLDER_DESC_SIZE = 96, FOLDER_ROUTING_SIZE = 20, MAX_DESCS = 64, MAX_ROUTINGS = 64 };

#define FC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define kRelativeFolder FC('r', 'e', 'l', 'f')
#define kSpecialFolder FC('s', 'p', 'c', 'f')
#define kBlessedFolder FC('b', 'l', 's', 'f')
#define kRootFolder FC('r', 'o', 't', 'f')
#define kSystemFolderType FC('m', 'a', 'c', 's')

typedef struct {
    uint32_t type, flags, cls, location, badge_sig, badge_type;
    uint8_t name[64]; /* Pascal */
    bool removed;     /* a standard one taken out */
} folder_desc;

typedef struct {
    uint32_t file_type, from, to, flags;
} folder_routing;

static folder_desc added[MAX_DESCS];
static int nadded;
static folder_routing routings[MAX_ROUTINGS];
static int nroutings;

void folders_reset(void)
{
    nadded = nroutings = 0;
}

static void set_pname(uint8_t out[64], const char *s)
{
    size_t n = strlen(s) > 63 ? 63 : strlen(s);
    out[0] = (uint8_t)n;
    memcpy(out + 1, s, n);
}

/* A standard folder's descriptor, by type or (type 0) by index. */
static bool standard_desc(uint32_t type, int index, folder_desc *d)
{
    uint32_t t;
    const char *name;
    for (int i = 0; fm_standard_folder(i, &t, &name); i++) {
        if (type ? t != type : i != index)
            continue;
        memset(d, 0, sizeof *d);
        d->type = t;
        if (t == kSystemFolderType) {
            d->cls = kSpecialFolder;
            d->location = kBlessedFolder;
            set_pname(d->name, "System Folder");
        } else if (!name) { /* the Desktop Folder, at the volume's root */
            d->cls = kSpecialFolder;
            d->location = kRootFolder;
            set_pname(d->name, "Desktop Folder");
        } else {
            d->cls = kRelativeFolder;
            d->location = kSystemFolderType;
            set_pname(d->name, name);
        }
        return true;
    }
    return false;
}

static folder_desc *added_desc(uint32_t type)
{
    for (int i = 0; i < nadded; i++)
        if (added[i].type == type)
            return &added[i];
    return NULL;
}

/* The descriptor a type has now: one a program added, or a standard one. */
static bool find_desc(uint32_t type, folder_desc *d)
{
    folder_desc *a = added_desc(type);
    if (a) {
        if (a->removed)
            return false;
        *d = *a;
        return true;
    }
    return standard_desc(type, 0, d);
}

/* FindFolder for a type a program added: a relative folder inside its
 * location's folder (made when create). */
int16_t folders_find_added(uint32_t type, bool create, char *out, size_t outsize)
{
    folder_desc *a = added_desc(type);
    if (!a || a->removed || a->cls != kRelativeFolder || a->location == type)
        return fnfErrFolder;
    char parent[PATH_MAX], path[PATH_MAX], name[64];
    int16_t err = fm_find_folder(a->location, create, parent, sizeof parent);
    if (err)
        return err;
    memcpy(name, a->name + 1, a->name[0]);
    name[a->name[0]] = 0;
    for (char *p = name; *p; p++)
        if (*p == '/')
            *p = ':'; /* a Mac name's slash, as the host keeps it */
    snprintf(path, sizeof path, "%s/%s", parent, name);
    struct stat st;
    if (stat(path, &st) != 0) {
        if (!create || mkdir(path, 0755) != 0)
            return fnfErrFolder;
    } else if (!S_ISDIR(st.st_mode)) {
        return fnfErrFolder;
    }
    return realpath(path, out) && strlen(out) < outsize ? noErr : fnfErrFolder;
}

int16_t tb_AddFolderDescriptor(int32_t foldType, uint32_t flags, int32_t foldClass, int32_t foldLocation,
                               int32_t badgeSignature, int32_t badgeType, gaddr name, int8_t replaceFlag)
{
    if ((uint32_t)foldClass != kRelativeFolder && (uint32_t)foldClass != kSpecialFolder)
        return badFolderDescErr;
    if ((uint32_t)foldClass == kRelativeFolder && (!name || !rd8(name)))
        return badFolderDescErr;
    folder_desc d;
    if (find_desc((uint32_t)foldType, &d) && !replaceFlag)
        return duplicateFolderDescErr;
    folder_desc *a = added_desc((uint32_t)foldType);
    if (!a) {
        if (nadded >= MAX_DESCS)
            return memFullErr;
        a = &added[nadded++];
    }
    memset(a, 0, sizeof *a);
    a->type = (uint32_t)foldType;
    a->flags = flags;
    a->cls = (uint32_t)foldClass;
    a->location = (uint32_t)foldLocation;
    a->badge_sig = (uint32_t)badgeSignature;
    a->badge_type = (uint32_t)badgeType;
    if (name) {
        uint8_t n = rd8(name) > 63 ? 63 : rd8(name);
        a->name[0] = n;
        gget(a->name + 1, name + 1, n);
    }
    return noErr;
}

int16_t tb_RemoveFolderDescriptor(int32_t foldType)
{
    folder_desc d;
    if (!find_desc((uint32_t)foldType, &d))
        return invalidFolderTypeErr;
    folder_desc *a = added_desc((uint32_t)foldType);
    if (!a) {
        if (nadded >= MAX_DESCS)
            return memFullErr;
        a = &added[nadded++];
        *a = d;
    }
    a->removed = true;
    return noErr;
}

int16_t tb_GetFolderDescriptor(int32_t foldType, int32_t descSize, gaddr foldDesc)
{
    folder_desc d;
    if (!find_desc((uint32_t)foldType, &d))
        return invalidFolderTypeErr;
    if (descSize < 4 || !foldDesc)
        return paramErr;
    uint8_t out[FOLDER_DESC_SIZE] = { 0 };
    uint32_t words[8] = { FOLDER_DESC_SIZE, d.type, d.flags, d.cls, d.location, d.badge_sig, d.badge_type, 0 };
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(words[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(words[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(words[i] >> 8);
        out[i * 4 + 3] = (uint8_t)words[i];
    }
    memcpy(out + 32, d.name, 64);
    gput(foldDesc, out, (uint32_t)(descSize < FOLDER_DESC_SIZE ? descSize : FOLDER_DESC_SIZE));
    return noErr;
}

/* Each type there is now, standard ones first, to fn; returns the count. */
static uint32_t each_type(void (*fn)(uint32_t type, uint32_t i, void *ctx), void *ctx)
{
    uint32_t n = 0;
    folder_desc d;
    for (int i = 0; standard_desc(0, i, &d); i++) {
        folder_desc *a = added_desc(d.type);
        if (a && a->removed)
            continue;
        fn(d.type, n++, ctx);
    }
    for (int i = 0; i < nadded; i++)
        if (!added[i].removed && !standard_desc(added[i].type, 0, &d))
            fn(added[i].type, n++, ctx);
    return n;
}

typedef struct {
    uint32_t want;
    gaddr out;
} type_list;

static void put_type(uint32_t type, uint32_t i, void *ctx)
{
    type_list *l = ctx;
    if (l->out && i < l->want)
        wr32(l->out + 4 * i, type);
}

int16_t tb_GetFolderTypes(uint32_t requestedTypeCount, gaddr totalTypeCount, gaddr theTypes)
{
    type_list l = { requestedTypeCount, theTypes };
    uint32_t n = each_type(put_type, &l);
    if (totalTypeCount)
        wr32(totalTypeCount, n);
    return noErr;
}

int16_t tb_GetFolderName(int16_t vRefNum, int32_t foldType, gaddr foundVRefNum, gaddr name)
{
    folder_desc d;
    if (!find_desc((uint32_t)foldType, &d))
        return invalidFolderTypeErr;
    char path[PATH_MAX];
    if (foundVRefNum)
        wr16(foundVRefNum, (uint16_t)(fm_find_folder((uint32_t)foldType, false, path, sizeof path) == noErr
                                          ? fm_vref_of_path(path)
                                          : vRefNum));
    if (name)
        gput(name, d.name, (uint32_t)d.name[0] + 1);
    return noErr;
}

typedef struct {
    char path[PATH_MAX];
    uint32_t found;
} identify;

static void match_type(uint32_t type, uint32_t i, void *ctx)
{
    (void)i;
    identify *id = ctx;
    char p[PATH_MAX];
    if (!id->found && fm_find_folder(type, false, p, sizeof p) == noErr && !strcmp(p, id->path))
        id->found = type;
}

int16_t tb_IdentifyFolder(int16_t vRefNum, int32_t dirID, gaddr foldType)
{
    identify id = { .found = 0 };
    char host[PATH_MAX];
    int16_t err = fm_resolve(vRefNum, dirID, 0, host, sizeof host);
    if (err)
        return err;
    if (!realpath(host, id.path))
        return fnfErrFolder;
    each_type(match_type, &id);
    if (!id.found)
        return invalidFolderTypeErr;
    if (foldType)
        wr32(foldType, id.found);
    return noErr;
}

/* The Folder Manager keeps nothing about folders between calls here. */
int16_t tb_InvalidateFolderDescriptorCache(int16_t vRefNum, int32_t dirID)
{
    (void)vRefNum;
    (void)dirID;
    return noErr;
}

static folder_routing *find_routing(uint32_t file_type, uint32_t from)
{
    for (int i = 0; i < nroutings; i++)
        if (routings[i].file_type == file_type && routings[i].from == from)
            return &routings[i];
    return NULL;
}

int16_t tb_AddFolderRouting(int32_t fileType, int32_t routeFromFolder, int32_t routeToFolder, uint32_t flags,
                            int8_t replaceFlag)
{
    folder_routing *r = find_routing((uint32_t)fileType, (uint32_t)routeFromFolder);
    if (r && !replaceFlag)
        return duplicateRoutingErr;
    if (!r) {
        if (nroutings >= MAX_ROUTINGS)
            return memFullErr;
        r = &routings[nroutings++];
    }
    *r = (folder_routing){ (uint32_t)fileType, (uint32_t)routeFromFolder, (uint32_t)routeToFolder, flags };
    return noErr;
}

int16_t tb_RemoveFolderRouting(int32_t fileType, int32_t routeFromFolder)
{
    folder_routing *r = find_routing((uint32_t)fileType, (uint32_t)routeFromFolder);
    if (!r)
        return routingNotFoundErr;
    *r = routings[--nroutings];
    return noErr;
}

int16_t tb_FindFolderRouting(int32_t fileType, int32_t routeFromFolder, gaddr routeToFolder, gaddr flags)
{
    folder_routing *r = find_routing((uint32_t)fileType, (uint32_t)routeFromFolder);
    if (!r)
        return routingNotFoundErr;
    if (routeToFolder)
        wr32(routeToFolder, r->to);
    if (flags)
        wr32(flags, r->flags);
    return noErr;
}

int16_t tb_GetFolderRoutings(uint32_t requestedRoutingCount, gaddr totalRoutingCount, int32_t routingSize,
                             gaddr theRoutings)
{
    if (routingSize < FOLDER_ROUTING_SIZE)
        return badRoutingSizeErr;
    if (totalRoutingCount)
        wr32(totalRoutingCount, (uint32_t)nroutings);
    for (uint32_t i = 0; theRoutings && i < requestedRoutingCount && i < (uint32_t)nroutings; i++) {
        gaddr p = theRoutings + i * (uint32_t)routingSize;
        wr32(p, FOLDER_ROUTING_SIZE);
        wr32(p + 4, routings[i].file_type);
        wr32(p + 8, routings[i].from);
        wr32(p + 12, routings[i].to);
        wr32(p + 16, routings[i].flags);
    }
    return noErr;
}
