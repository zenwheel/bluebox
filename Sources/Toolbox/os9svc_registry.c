/*
 * NameRegistryLib (NameRegistry.h): the Name Registry, the tree of named
 * entries with properties that Open Firmware's device tree is held in.
 * Blue Box has no devices to put in it, so it starts with the one plane
 * programs ask for, "Devices", holding the tree's root "device-tree";
 * programs can create and delete entries and properties, look them up by
 * path, and iterate and search the tree as on a Mac.  The Open Firmware
 * calls (paths and node IDs of the firmware) are not provided.
 * PowerPC only.
 *
 * A RegEntryID is its serial number and a tag in contents[0] and [1]; one
 * that has been disposed or never initialized is zero, and one whose entry
 * is gone is nrInvalidNodeErr.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "os9svc.h"
#include "ppc_bridge.h"
#include "tb_internal.h"

#define R(n) (c->gpr[3 + (n)])
#define RETURN(v) do { c->gpr[3] = (uint32_t)(int32_t)(v); c->pc = c->lr; return; } while (0)

enum {
    nrNotEnoughMemoryErr = -2537,
    nrInvalidNodeErr = -2538,
    nrNotFoundErr = -2539,
    nrNotCreatedErr = -2540,
    nrNameErr = -2541,
    nrOverrunErr = -2548,
    nrPropertyAlreadyExists = -2553,
    nrIterationDone = -2554,
    nrLockedErr = -2536,
};
enum { kRegIterRoot = 2, kRegIterParents, kRegIterChildren, kRegIterSubTrees, kRegIterSibling, kRegIterContinue = 1 };
enum { MAX_ENTRY_NAME = 47, MAX_PROP_NAME = 31, ID_TAG = 0x4E526567 /* 'NReg' */ };

typedef struct prop {
    char name[MAX_PROP_NAME + 1];
    uint8_t *value;
    uint32_t size, mods;
    gaddr copy; /* the value in guest memory, once a program asked for a pointer to it */
    struct prop *next;
} prop;

typedef struct entry {
    uint32_t serial;
    char name[MAX_ENTRY_NAME + 1];
    struct entry *parent, *first_child, *next_sibling;
    prop *props;
    uint32_t mods;
    gaddr node; /* its Open Firmware device node, once a program asked for it */
} entry;

static entry *root;      /* the unnamed root of the planes */
static bool resetting;   /* the machine's memory is gone: nothing in it to give back */
static uint32_t next_serial = 1;

/* Iteration state, for entries and for properties. */
typedef struct {
    bool used;
    uint32_t start;      /* the entry iteration is from */
    uint32_t *list;      /* the serials still to give */
    uint32_t n, at;
    bool primed;
} eiter;
typedef struct {
    bool used;
    uint32_t entry;
    char (*names)[MAX_PROP_NAME + 1];
    uint32_t n, at;
} piter;
#define MAX_ITERS 32
#define ITER_TAG 0x4E490000u
#define PITER_TAG 0x4E500000u
static eiter eiters[MAX_ITERS];
static piter piters[MAX_ITERS];

static entry *find_in(entry *e, uint32_t serial)
{
    if (e->serial == serial)
        return e;
    for (entry *k = e->first_child; k; k = k->next_sibling) {
        entry *f = find_in(k, serial);
        if (f)
            return f;
    }
    return NULL;
}

static entry *new_entry(entry *parent, const char *name)
{
    entry *e = calloc(1, sizeof *e);
    if (!e)
        return NULL;
    e->serial = next_serial++;
    snprintf(e->name, sizeof e->name, "%s", name);
    e->parent = parent;
    if (parent) {
        entry **p = &parent->first_child;
        while (*p)
            p = &(*p)->next_sibling;
        *p = e;
    }
    return e;
}

static void drop_copy(prop *p)
{
    if (p->copy && !resetting)
        tb_DisposePtr(p->copy);
    p->copy = 0;
}

static prop *prop_find(entry *e, const char *name)
{
    for (prop *p = e->props; p; p = p->next)
        if (!strcmp(p->name, name))
            return p;
    return NULL;
}

static int prop_set(entry *e, const char *name, const uint8_t *value, uint32_t size, bool create)
{
    prop *p = prop_find(e, name);
    if (!p) {
        if (!create)
            return nrNotFoundErr;
        p = calloc(1, sizeof *p);
        if (!p)
            return nrNotEnoughMemoryErr;
        snprintf(p->name, sizeof p->name, "%s", name);
        prop **tail = &e->props;
        while (*tail)
            tail = &(*tail)->next;
        *tail = p;
    } else if (create) {
        return nrPropertyAlreadyExists;
    }
    uint8_t *copy = size ? malloc(size) : NULL;
    if (size && !copy)
        return nrNotEnoughMemoryErr;
    if (size)
        memcpy(copy, value, size);
    free(p->value);
    drop_copy(p);
    p->value = copy;
    p->size = size;
    return noErr;
}

static void free_props(entry *e)
{
    while (e->props) {
        prop *p = e->props;
        e->props = p->next;
        drop_copy(p);
        free(p->value);
        free(p);
    }
}

static void free_tree(entry *e)
{
    while (e->first_child) {
        entry *k = e->first_child;
        e->first_child = k->next_sibling;
        free_tree(k);
    }
    free_props(e);
    if (e->node && !resetting)
        tb_DisposePtr(e->node);
    free(e);
}

static void registry_init(void)
{
    if (root)
        return;
    root = new_entry(NULL, "");
    entry *devices = new_entry(root, "Devices");
    entry *tree = new_entry(devices, "device-tree");
    if (!root || !devices || !tree)
        return;
    prop_set(root, "name", (const uint8_t *)"", 1, true);
    prop_set(devices, "name", (const uint8_t *)"Devices", 8, true);
    prop_set(tree, "name", (const uint8_t *)"device-tree", 12, true);
    devices->mods = tree->mods = 0;
}

void os9svc_registry_reset(void)
{
    resetting = true;
    if (root)
        free_tree(root);
    resetting = false;
    root = NULL;
    next_serial = 1;
    for (int i = 0; i < MAX_ITERS; i++) {
        free(eiters[i].list);
        free(piters[i].names);
    }
    memset(eiters, 0, sizeof eiters);
    memset(piters, 0, sizeof piters);
}

/* ---- IDs ---- */

static void id_put(gaddr id, const entry *e)
{
    wr32(id, e->serial);
    wr32(id + 4, ID_TAG);
    wr32(id + 8, 0);
    wr32(id + 12, 0);
}

static bool id_valid(gaddr id) { return id && rd32(id + 4) == ID_TAG; }

/* The entry an ID names. */
static int id_entry(gaddr id, entry **out)
{
    registry_init();
    if (!id_valid(id))
        return nrInvalidNodeErr;
    *out = root ? find_in(root, rd32(id)) : NULL;
    return *out ? noErr : nrInvalidNodeErr;
}

static void get_cstr(gaddr p, char *out, size_t max)
{
    size_t n = 0;
    while (n + 1 < max && rd8(p + n)) {
        out[n] = (char)rd8(p + n);
        n++;
    }
    out[n] = 0;
}

/* A property of the entry an ID names: its bytes (valid until the registry
 * changes) and size; for the Driver Loader. */
int os9_registry_property(gaddr id, const char *name, const uint8_t **data, uint32_t *size)
{
    entry *e;
    int err = id_entry(id, &e);
    if (err)
        return err;
    prop *p = prop_find(e, name);
    if (!p)
        return nrNotFoundErr;
    *data = p->value;
    *size = p->size;
    return noErr;
}

/* Whether an ID names an entry (nrInvalidNodeErr if not), and whether it
 * is on a PCI bus: itself or an ancestor has device_type "pci". */
int os9_registry_on_pci(gaddr id, bool *pci)
{
    entry *e;
    int err = id_entry(id, &e);
    if (err)
        return err;
    *pci = false;
    for (; e; e = e->parent) {
        prop *p = prop_find(e, "device_type");
        if (p && p->size >= 3 && !memcmp(p->value, "pci", 3) && (p->size == 3 || p->value[3] == 0)) {
            *pci = true;
            break;
        }
    }
    return noErr;
}

static void ds_IDInit(ppc_cpu *c)
{
    if (!R(0))
        RETURN(paramErr);
    for (int i = 0; i < 4; i++)
        wr32(R(0) + 4u * (uint32_t)i, 0);
    RETURN(noErr);
}

static void ds_IDCompare(ppc_cpu *c)
{
    if (!R(0) || !R(1))
        RETURN(0);
    for (int i = 0; i < 4; i++)
        if (rd32(R(0) + 4u * (uint32_t)i) != rd32(R(1) + 4u * (uint32_t)i))
            RETURN(0);
    RETURN(1);
}

static void ds_IDCopy(ppc_cpu *c)
{
    if (!R(0) || !R(1))
        RETURN(paramErr);
    for (int i = 0; i < 4; i++)
        wr32(R(1) + 4u * (uint32_t)i, rd32(R(0) + 4u * (uint32_t)i));
    RETURN(noErr);
}

static void ds_IDDispose(ppc_cpu *c) { ds_IDInit(c); }

/* ---- Paths ---- */

/* The entry at a path under start (the root when start is NULL or the path
 * begins with a plane's name there): components separated by ':'. */
static int lookup(entry *start, const char *path, entry **out)
{
    entry *e = start ? start : root;
    char comp[MAX_ENTRY_NAME + 2];
    const char *p = path;
    if (*p == ':')
        p++;
    while (*p) {
        size_t n = strcspn(p, ":");
        if (n > MAX_ENTRY_NAME)
            return nrNameErr;
        memcpy(comp, p, n);
        comp[n] = 0;
        entry *k = e->first_child;
        while (k && strcmp(k->name, comp))
            k = k->next_sibling;
        if (!k)
            return nrNotFoundErr;
        e = k;
        p += n;
        if (*p == ':')
            p++;
    }
    *out = e;
    return noErr;
}

/* The path of an entry: the names from the plane down. */
static size_t entry_path(const entry *e, char *out, size_t max)
{
    const entry *chain[64];
    int n = 0;
    for (const entry *k = e; k && k->parent && n < 64; k = k->parent)
        chain[n++] = k;
    size_t len = 0;
    for (int i = n - 1; i >= 0; i--) {
        int w = snprintf(out ? out + len : NULL, out ? max - len : 0, "%s%s", i == n - 1 ? "" : ":", chain[i]->name);
        len += (size_t)w;
    }
    return len;
}

/* RegistryCStrEntryCreate(parentEntry, name, *newEntry): a name or a path
 * whose last part is the new entry. */
static void ds_CStrEntryCreate(ppc_cpu *c)
{
    registry_init();
    if (!R(1))
        RETURN(paramErr);
    entry *parent = root;
    if (id_valid(R(0))) {
        int err = id_entry(R(0), &parent);
        if (err)
            RETURN(err);
    }
    char path[256];
    get_cstr(R(1), path, sizeof path);
    char *last = strrchr(path, ':');
    const char *name = path;
    if (last) {
        *last = 0;
        name = last + 1;
        int err = lookup(parent, path, &parent);
        if (err)
            RETURN(err == nrNotFoundErr ? nrNotCreatedErr : err);
    }
    if (!*name || strlen(name) > MAX_ENTRY_NAME)
        RETURN(nrNameErr);
    if (parent == root)
        RETURN(nrNotCreatedErr); /* the planes are not ours to add */
    for (entry *k = parent->first_child; k; k = k->next_sibling)
        if (!strcmp(k->name, name))
            RETURN(nrNotCreatedErr);
    entry *e = new_entry(parent, name);
    if (!e)
        RETURN(nrNotEnoughMemoryErr);
    prop_set(e, "name", (const uint8_t *)name, (uint32_t)strlen(name) + 1, true);
    if (R(2))
        id_put(R(2), e);
    RETURN(noErr);
}

static void ds_EntryDelete(ppc_cpu *c)
{
    entry *e;
    int err = id_entry(R(0), &e);
    if (err)
        RETURN(err);
    if (!e->parent || !e->parent->parent)
        RETURN(nrLockedErr); /* the root and the planes */
    entry **p = &e->parent->first_child;
    while (*p != e)
        p = &(*p)->next_sibling;
    *p = e->next_sibling;
    e->next_sibling = NULL;
    free_tree(e);
    RETURN(noErr);
}

static entry *copy_tree(const entry *src, entry *parent)
{
    entry *e = new_entry(parent, src->name);
    if (!e)
        return NULL;
    e->mods = src->mods;
    for (prop *p = src->props; p; p = p->next) {
        if (prop_set(e, p->name, p->value, p->size, true))
            return NULL;
        prop_find(e, p->name)->mods = p->mods;
    }
    for (entry *k = src->first_child; k; k = k->next_sibling)
        if (!copy_tree(k, e))
            return NULL;
    return e;
}

/* RegistryEntryCopy(parent, source, *dest): the source and what is under
 * it, as a new child of the parent. */
static void ds_EntryCopy(ppc_cpu *c)
{
    entry *parent, *src;
    int err = id_entry(R(0), &parent);
    if (!err)
        err = id_entry(R(1), &src);
    if (err)
        RETURN(err);
    for (entry *k = parent->first_child; k; k = k->next_sibling)
        if (!strcmp(k->name, src->name))
            RETURN(nrNotCreatedErr);
    entry *e = copy_tree(src, parent);
    if (!e)
        RETURN(nrNotEnoughMemoryErr);
    if (R(2))
        id_put(R(2), e);
    RETURN(noErr);
}

/* RegistryCStrEntryLookup(searchPoint, path, *found) */
static void ds_CStrEntryLookup(ppc_cpu *c)
{
    registry_init();
    entry *start = NULL, *found;
    if (!R(1) || !R(2))
        RETURN(paramErr);
    if (id_valid(R(0))) {
        int err = id_entry(R(0), &start);
        if (err)
            RETURN(err);
    }
    char path[512];
    get_cstr(R(1), path, sizeof path);
    int err = lookup(start, path, &found);
    if (err)
        RETURN(err);
    id_put(R(2), found);
    RETURN(noErr);
}

static void ds_EntryToPathSize(ppc_cpu *c)
{
    entry *e;
    int err = id_entry(R(0), &e);
    if (err)
        RETURN(err);
    if (!R(1))
        RETURN(paramErr);
    wr32(R(1), (uint32_t)entry_path(e, NULL, 0) + 1);
    RETURN(noErr);
}

static void ds_CStrEntryToPath(ppc_cpu *c)
{
    entry *e;
    int err = id_entry(R(0), &e);
    if (err)
        RETURN(err);
    if (!R(1))
        RETURN(paramErr);
    char path[1024];
    size_t len = entry_path(e, path, sizeof path);
    if (len + 1 > R(2))
        RETURN(nrOverrunErr);
    for (size_t i = 0; i <= len; i++)
        wr8(R(1) + i, (uint8_t)path[i]);
    RETURN(noErr);
}

/* RegistryCStrEntryToName(entry, *parent, nameComponent, *done): the
 * entry's name and its parent; done when the parent is the root. */
static void ds_CStrEntryToName(ppc_cpu *c)
{
    entry *e;
    int err = id_entry(R(0), &e);
    if (err)
        RETURN(err);
    if (R(1) && e->parent)
        id_put(R(1), e->parent);
    if (R(2))
        for (size_t i = 0; i <= strlen(e->name); i++)
            wr8(R(2) + i, (uint8_t)e->name[i]);
    if (R(3))
        wr8(R(3), !e->parent || !e->parent->parent);
    RETURN(noErr);
}

/* ---- Properties ---- */

static int prop_name(gaddr p, char *out)
{
    if (!p)
        return paramErr;
    get_cstr(p, out, MAX_PROP_NAME + 2);
    return strlen(out) > MAX_PROP_NAME || !*out ? nrNameErr : noErr;
}

/* The entry and property of (entryID, propertyName). */
static int prop_of(gaddr id, gaddr name, entry **e, prop **p)
{
    char n[MAX_PROP_NAME + 2];
    int err = id_entry(id, e);
    if (!err)
        err = prop_name(name, n);
    if (err)
        return err;
    *p = prop_find(*e, n);
    return *p ? noErr : nrNotFoundErr;
}

static void ds_PropertyCreate(ppc_cpu *c)
{
    entry *e;
    char n[MAX_PROP_NAME + 2];
    int err = id_entry(R(0), &e);
    if (!err)
        err = prop_name(R(1), n);
    if (err)
        RETURN(err);
    uint32_t size = R(3);
    if (size && !R(2))
        RETURN(paramErr);
    uint8_t *buf = size ? malloc(size) : NULL;
    if (size && !buf)
        RETURN(nrNotEnoughMemoryErr);
    for (uint32_t i = 0; i < size; i++)
        buf[i] = rd8(R(2) + i);
    err = prop_set(e, n, buf, size, true);
    free(buf);
    RETURN(err);
}

static void ds_PropertyDelete(ppc_cpu *c)
{
    entry *e;
    prop *p;
    int err = prop_of(R(0), R(1), &e, &p);
    if (err)
        RETURN(err);
    for (prop **q = &e->props; *q; q = &(*q)->next)
        if (*q == p) {
            *q = p->next;
            break;
        }
    free(p->value);
    free(p);
    RETURN(noErr);
}

static void ds_PropertyRename(ppc_cpu *c)
{
    entry *e;
    prop *p;
    char n[MAX_PROP_NAME + 2];
    int err = prop_of(R(0), R(1), &e, &p);
    if (!err)
        err = prop_name(R(2), n);
    if (err)
        RETURN(err);
    if (prop_find(e, n))
        RETURN(nrPropertyAlreadyExists);
    snprintf(p->name, sizeof p->name, "%s", n);
    RETURN(noErr);
}

static void ds_PropertyGetSize(ppc_cpu *c)
{
    entry *e;
    prop *p;
    int err = prop_of(R(0), R(1), &e, &p);
    if (err)
        RETURN(err);
    if (!R(2))
        RETURN(paramErr);
    wr32(R(2), p->size);
    RETURN(noErr);
}

/* RegistryPropertyGet(entry, name, value, *size): the size in is the room,
 * out the value's size; a value that doesn't fit is not copied. */
static void ds_PropertyGet(ppc_cpu *c)
{
    entry *e;
    prop *p;
    int err = prop_of(R(0), R(1), &e, &p);
    if (err)
        RETURN(err);
    if (!R(3))
        RETURN(paramErr);
    uint32_t room = rd32(R(3));
    wr32(R(3), p->size);
    if (p->size > room)
        RETURN(nrOverrunErr);
    for (uint32_t i = 0; R(2) && i < p->size; i++)
        wr8(R(2) + i, p->value[i]);
    RETURN(noErr);
}

static void ds_PropertySet(ppc_cpu *c)
{
    entry *e;
    prop *p;
    int err = prop_of(R(0), R(1), &e, &p);
    if (err)
        RETURN(err);
    uint32_t size = R(3);
    if (size && !R(2))
        RETURN(paramErr);
    uint8_t *buf = size ? malloc(size) : NULL;
    if (size && !buf)
        RETURN(nrNotEnoughMemoryErr);
    for (uint32_t i = 0; i < size; i++)
        buf[i] = rd8(R(2) + i);
    err = prop_set(e, p->name, buf, size, false);
    free(buf);
    RETURN(err);
}

static void ds_EntryGetMod(ppc_cpu *c)
{
    entry *e;
    int err = id_entry(R(0), &e);
    if (err)
        RETURN(err);
    if (!R(1))
        RETURN(paramErr);
    wr32(R(1), e->mods);
    RETURN(noErr);
}

static void ds_EntrySetMod(ppc_cpu *c)
{
    entry *e;
    int err = id_entry(R(0), &e);
    if (err)
        RETURN(err);
    e->mods = R(1);
    RETURN(noErr);
}

static void ds_PropertyGetMod(ppc_cpu *c)
{
    entry *e;
    prop *p;
    int err = prop_of(R(0), R(1), &e, &p);
    if (err)
        RETURN(err);
    if (!R(2))
        RETURN(paramErr);
    wr32(R(2), p->mods);
    RETURN(noErr);
}

static void ds_PropertySetMod(ppc_cpu *c)
{
    entry *e;
    prop *p;
    int err = prop_of(R(0), R(1), &e, &p);
    if (err)
        RETURN(err);
    p->mods = R(2);
    RETURN(noErr);
}

/* ---- Property iteration ---- */

static piter *piter_of(gaddr cookie)
{
    uint32_t v = cookie ? rd32(cookie) : 0, i = (v & 0xFFFF) - 1;
    return (v & 0xFFFF0000u) == PITER_TAG && i < MAX_ITERS && piters[i].used ? &piters[i] : NULL;
}

/* RegistryPropertyIterateCreate(entry, *cookie): the names as they are now. */
static void ds_PropertyIterateCreate(ppc_cpu *c)
{
    entry *e;
    int err = id_entry(R(0), &e);
    if (err)
        RETURN(err);
    if (!R(1))
        RETURN(paramErr);
    for (int i = 0; i < MAX_ITERS; i++)
        if (!piters[i].used) {
            uint32_t n = 0;
            for (prop *p = e->props; p; p = p->next)
                n++;
            char(*names)[MAX_PROP_NAME + 1] = n ? malloc(n * sizeof *names) : NULL;
            if (n && !names)
                RETURN(nrNotEnoughMemoryErr);
            n = 0;
            for (prop *p = e->props; p; p = p->next)
                memcpy(names[n++], p->name, sizeof *names);
            piters[i] = (piter){ true, e->serial, names, n, 0 };
            wr32(R(1), PITER_TAG | (uint32_t)(i + 1));
            RETURN(noErr);
        }
    RETURN(nrNotEnoughMemoryErr);
}

static void ds_PropertyIterateDispose(ppc_cpu *c)
{
    piter *it = piter_of(R(0));
    if (!it)
        RETURN(paramErr);
    free(it->names);
    memset(it, 0, sizeof *it);
    wr32(R(0), 0);
    RETURN(noErr);
}

/* RegistryPropertyIterate(*cookie, foundProperty, *done) */
static void ds_PropertyIterate(ppc_cpu *c)
{
    piter *it = piter_of(R(0));
    if (!it || !R(1))
        RETURN(paramErr);
    if (it->at >= it->n) {
        if (R(2))
            wr8(R(2), 1);
        RETURN(nrIterationDone);
    }
    const char *name = it->names[it->at++];
    for (size_t i = 0; i <= strlen(name); i++)
        wr8(R(1) + i, (uint8_t)name[i]);
    if (R(2))
        wr8(R(2), it->at >= it->n);
    RETURN(noErr);
}

/* ---- Entry iteration and search ---- */

static eiter *eiter_of(gaddr cookie)
{
    uint32_t v = cookie ? rd32(cookie) : 0, i = (v & 0xFFFF) - 1;
    return (v & 0xFFFF0000u) == ITER_TAG && i < MAX_ITERS && eiters[i].used ? &eiters[i] : NULL;
}

static void ds_EntryIterateCreate(ppc_cpu *c)
{
    registry_init();
    if (!R(0))
        RETURN(paramErr);
    for (int i = 0; i < MAX_ITERS; i++)
        if (!eiters[i].used) {
            memset(&eiters[i], 0, sizeof eiters[i]);
            eiters[i].used = true;
            /* from the device tree, or the plane it was in */
            entry *plane = root ? root->first_child : NULL;
            eiters[i].start = plane ? (plane->first_child ? plane->first_child : plane)->serial : 0;
            wr32(R(0), ITER_TAG | (uint32_t)(i + 1));
            RETURN(noErr);
        }
    RETURN(nrNotEnoughMemoryErr);
}

static void ds_EntryIterateDispose(ppc_cpu *c)
{
    eiter *it = eiter_of(R(0));
    if (!it)
        RETURN(paramErr);
    free(it->list);
    memset(it, 0, sizeof *it);
    wr32(R(0), 0);
    RETURN(noErr);
}

static void ds_EntryIterateSet(ppc_cpu *c)
{
    eiter *it = eiter_of(R(0));
    entry *e;
    if (!it)
        RETURN(paramErr);
    int err = id_entry(R(1), &e);
    if (err)
        RETURN(err);
    it->start = e->serial;
    RETURN(noErr);
}

/* A filter an iteration applies: a property (by name and value, or just
 * its presence), or modifier bits on the entry or on a property. */
typedef struct {
    const char *prop;
    const uint8_t *value;
    uint32_t size;
    bool by_value, by_entry_mods, by_prop_mods;
    uint32_t mods;
} filter;

static bool passes(const entry *e, const filter *f)
{
    if (!f)
        return true;
    if (f->by_entry_mods)
        return (e->mods & f->mods) == f->mods;
    if (f->by_prop_mods) {
        for (prop *p = e->props; p; p = p->next)
            if ((p->mods & f->mods) == f->mods)
                return true;
        return false;
    }
    prop *p = prop_find((entry *)e, f->prop);
    if (!p)
        return false;
    return !f->by_value || (p->size == f->size && !memcmp(p->value, f->value, f->size));
}

static bool list_add(eiter *it, uint32_t *cap, const entry *e, const filter *f)
{
    if (!passes(e, f))
        return true;
    if (it->n == *cap) {
        uint32_t ncap = *cap ? *cap * 2 : 16;
        uint32_t *grown = realloc(it->list, ncap * sizeof *grown);
        if (!grown)
            return false;
        it->list = grown;
        *cap = ncap;
    }
    it->list[it->n++] = e->serial;
    return true;
}

static bool add_descendants(eiter *it, uint32_t *cap, const entry *e, const filter *f)
{
    for (const entry *k = e->first_child; k; k = k->next_sibling) {
        if (!list_add(it, cap, k, f) || !add_descendants(it, cap, k, f))
            return false;
    }
    return true;
}

/* One step of an iteration or search: a new relationship builds the list of
 * entries from the start (or the last one given), Continue goes on. */
static int iterate(gaddr cookie, uint32_t op, gaddr found, gaddr done, const filter *f)
{
    registry_init();
    eiter *it = eiter_of(cookie);
    if (!it || !found)
        return paramErr;
    if (op != kRegIterContinue) {
        entry *from = root ? find_in(root, it->start) : NULL;
        if (!from)
            return nrInvalidNodeErr;
        free(it->list);
        it->list = NULL;
        it->n = it->at = 0;
        uint32_t cap = 0;
        bool ok = true;
        switch (op) {
        case kRegIterRoot:
            ok = list_add(it, &cap, root->first_child ? root->first_child : root, f);
            break;
        case kRegIterParents:
            for (entry *p = from->parent; ok && p && p->parent; p = p->parent)
                ok = list_add(it, &cap, p, f);
            break;
        case kRegIterChildren:
            for (entry *k = from->first_child; ok && k; k = k->next_sibling)
                ok = list_add(it, &cap, k, f);
            break;
        case kRegIterSubTrees:
            ok = add_descendants(it, &cap, from, f);
            break;
        case kRegIterSibling:
            for (entry *k = from->parent ? from->parent->first_child : NULL; ok && k; k = k->next_sibling)
                if (k != from)
                    ok = list_add(it, &cap, k, f);
            break;
        default:
            return paramErr;
        }
        if (!ok)
            return nrNotEnoughMemoryErr;
        it->primed = true;
    } else if (!it->primed) {
        return paramErr;
    }
    /* The next entry that is still there. */
    while (it->at < it->n) {
        entry *e = root ? find_in(root, it->list[it->at++]) : NULL;
        if (!e)
            continue;
        bool last = it->at >= it->n;
        id_put(found, e);
        it->start = e->serial;
        if (done)
            wr8(done, last);
        return noErr;
    }
    if (done)
        wr8(done, 1);
    return op == kRegIterContinue ? nrIterationDone : nrNotFoundErr;
}

static void ds_EntryIterate(ppc_cpu *c) { RETURN(iterate(R(0), R(1), R(2), R(3), NULL)); }

/* RegistryEntrySearch(cookie, relationship, found, done, propertyName,
 * propertyValue, propertySize) */
static void ds_EntrySearch(ppc_cpu *c)
{
    char name[MAX_PROP_NAME + 2];
    int err = prop_name(R(4), name);
    if (err)
        RETURN(err);
    uint32_t size = R(6);
    uint8_t *value = size && R(5) ? malloc(size) : NULL;
    for (uint32_t i = 0; value && i < size; i++)
        value[i] = rd8(R(5) + i);
    filter f = { name, value, size, R(5) != 0, false, false, 0 };
    int r = iterate(R(0), R(1), R(2), R(3), &f);
    free(value);
    RETURN(r);
}

static void ds_EntryMod(ppc_cpu *c)
{
    filter f = { NULL, NULL, 0, false, true, false, R(4) };
    RETURN(iterate(R(0), R(1), R(2), R(3), &f));
}

static void ds_EntryPropertyMod(ppc_cpu *c)
{
    filter f = { NULL, NULL, 0, false, false, true, R(4) };
    RETURN(iterate(R(0), R(1), R(2), R(3), &f));
}

/* ---- Open Firmware ---- */

/* The Devices plane's device-tree is Open Firmware's, so an entry in it has
 * the path the firmware would give it: "/" and the names under the tree,
 * separated by '/'.  Entries of the other planes have none. */
static int of_path(const entry *e, char *out, size_t max, size_t *len)
{
    const entry *chain[64];
    int n = 0;
    const entry *k = e;
    for (; k && k->parent && n < 64; k = k->parent) {
        if (k->parent->parent && !k->parent->parent->parent && !strcmp(k->parent->name, "Devices")
            && !strcmp(k->name, "device-tree"))
            break;
        chain[n++] = k;
    }
    if (!k || !k->parent || !k->parent->parent || k->parent->parent->parent || strcmp(k->name, "device-tree"))
        return nrNotFoundErr;
    size_t l = 0;
    for (int i = n - 1; i >= 0; i--) {
        int w = snprintf(out ? out + l : NULL, out ? max - l : 0, "/%s", chain[i]->name);
        l += (size_t)w;
    }
    if (!l) {
        if (out && max > 1) {
            out[0] = '/';
            out[1] = 0;
        }
        l = 1;
    }
    *len = l;
    return noErr;
}

/* RegistryEntryToOFPathSize(entry, *size): the path and its NUL. */
static void ds_EntryToOFPathSize(ppc_cpu *c)
{
    entry *e;
    size_t len;
    int err = id_entry(R(0), &e);
    if (!err && !R(1))
        err = paramErr;
    if (!err)
        err = of_path(e, NULL, 0, &len);
    if (err)
        RETURN(err);
    wr32(R(1), (uint32_t)len + 1);
    RETURN(noErr);
}

/* RegistryCStrEntryToOFPath(entry, path, room) */
static void ds_CStrEntryToOFPath(ppc_cpu *c)
{
    entry *e;
    char path[2048];
    size_t len;
    int err = id_entry(R(0), &e);
    if (!err && !R(1))
        err = paramErr;
    if (!err)
        err = of_path(e, path, sizeof path, &len);
    if (err)
        RETURN(err);
    if (len + 1 > R(2))
        RETURN(nrOverrunErr);
    for (size_t i = 0; i <= len; i++)
        wr8(R(1) + i, (uint8_t)path[i]);
    RETURN(noErr);
}

/* A device node ID is the address of the node's record, whose first word
 * is the entry's serial number. */
static entry *find_node(entry *e, gaddr node)
{
    if (e->node == node)
        return e;
    for (entry *k = e->first_child; k; k = k->next_sibling) {
        entry *f = find_node(k, node);
        if (f)
            return f;
    }
    return NULL;
}

/* RegistryEntryIDToDeviceNodeID(entry, *node) */
static void ds_EntryIDToDeviceNodeID(ppc_cpu *c)
{
    entry *e;
    int err = id_entry(R(0), &e);
    if (!err && !R(1))
        err = paramErr;
    if (err)
        RETURN(err);
    if (!e->node) {
        e->node = tb_NewPtr(8, true, true);
        if (!e->node)
            RETURN(nrNotEnoughMemoryErr);
        wr32(e->node, e->serial);
        wr32(e->node + 4, ID_TAG);
    }
    wr32(R(1), e->node);
    RETURN(noErr);
}

/* RegistryDeviceNodeIDToEntryID(node, *entry) */
static void ds_DeviceNodeIDToEntryID(ppc_cpu *c)
{
    registry_init();
    entry *e = R(0) && root ? find_node(root, R(0)) : NULL;
    if (!e)
        RETURN(nrInvalidNodeErr);
    if (!R(1))
        RETURN(paramErr);
    id_put(R(1), e);
    RETURN(noErr);
}

/* RegistryPropertyGetPtr(entry, name, **value, *size): the value where the
 * program can read it in place (a copy kept until the property changes). */
static void ds_PropertyGetPtr(ppc_cpu *c)
{
    entry *e;
    prop *p;
    int err = prop_of(R(0), R(1), &e, &p);
    if (!err && (!R(2) || !R(3)))
        err = paramErr;
    if (!err && !p->copy) {
        p->copy = tb_NewPtr((int32_t)(p->size ? p->size : 1), true, true);
        if (!p->copy)
            err = nrNotEnoughMemoryErr;
        for (uint32_t i = 0; !err && i < p->size; i++)
            wr8(p->copy + i, p->value[i]);
    }
    if (err) {
        if (R(3))
            wr32(R(3), 0);
        RETURN(err);
    }
    wr32(R(2), p->copy);
    wr32(R(3), p->size);
    RETURN(noErr);
}

/* NR_Init(): the registry exists from the first call on; this makes it. */
static void ds_NR_Init(ppc_cpu *c)
{
    registry_init();
    RETURN(noErr);
}

const ppc_export os9_registry_exports[] = {
    { "RegistryEntryToOFPathSize", ds_EntryToOFPathSize }, { "RegistryCStrEntryToOFPath", ds_CStrEntryToOFPath },
    { "RegistryEntryIDToDeviceNodeID", ds_EntryIDToDeviceNodeID },
    { "RegistryDeviceNodeIDToEntryID", ds_DeviceNodeIDToEntryID }, { "RegistryPropertyGetPtr", ds_PropertyGetPtr },
    { "NR_Init", ds_NR_Init },
    { "RegistryEntryIDInit", ds_IDInit }, { "RegistryEntryIDCompare", ds_IDCompare },
    { "RegistryEntryIDCopy", ds_IDCopy }, { "RegistryEntryIDDispose", ds_IDDispose },
    { "RegistryCStrEntryCreate", ds_CStrEntryCreate }, { "RegistryEntryDelete", ds_EntryDelete },
    { "RegistryEntryCopy", ds_EntryCopy }, { "RegistryEntryIterateCreate", ds_EntryIterateCreate },
    { "RegistryEntryIterateDispose", ds_EntryIterateDispose }, { "RegistryEntryIterateSet", ds_EntryIterateSet },
    { "RegistryEntryIterate", ds_EntryIterate }, { "RegistryEntrySearch", ds_EntrySearch },
    { "RegistryCStrEntryLookup", ds_CStrEntryLookup }, { "RegistryEntryToPathSize", ds_EntryToPathSize },
    { "RegistryCStrEntryToPath", ds_CStrEntryToPath }, { "RegistryCStrEntryToName", ds_CStrEntryToName },
    { "RegistryPropertyCreate", ds_PropertyCreate }, { "RegistryPropertyDelete", ds_PropertyDelete },
    { "RegistryPropertyRename", ds_PropertyRename }, { "RegistryPropertyIterateCreate", ds_PropertyIterateCreate },
    { "RegistryPropertyIterateDispose", ds_PropertyIterateDispose },
    { "RegistryPropertyIterate", ds_PropertyIterate }, { "RegistryPropertyGetSize", ds_PropertyGetSize },
    { "RegistryPropertyGet", ds_PropertyGet }, { "RegistryPropertySet", ds_PropertySet },
    { "RegistryEntryGetMod", ds_EntryGetMod }, { "RegistryEntrySetMod", ds_EntrySetMod },
    { "RegistryPropertyGetMod", ds_PropertyGetMod }, { "RegistryPropertySetMod", ds_PropertySetMod },
    { "RegistryEntryMod", ds_EntryMod }, { "RegistryEntryPropertyMod", ds_EntryPropertyMod },
};
const unsigned os9_registry_export_count = sizeof os9_registry_exports / sizeof os9_registry_exports[0];
const os9_alias os9_registry_aliases[] = { { "", "" } };
const unsigned os9_registry_alias_count = 0;
