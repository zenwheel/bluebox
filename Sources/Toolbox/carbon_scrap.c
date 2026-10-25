/*
 * The Carbon Scrap Manager, on the desk scrap (scrap.c) and so on its
 * bridge to the host's pasteboard.
 *
 * A ScrapRef names the scrap as it is between two clears: ClearCurrentScrap
 * (ZeroScrap) moves the scrap count on, and refs from before are refused.
 * Flavours put with flags keep them here; promised ones (no data yet) are
 * kept until the program puts their data, which its promise keeper is
 * asked for when someone wants it.  Text and Unicode text stand in for
 * each other, as CarbonLib translated them: a program asking for 'utxt'
 * where there is only 'TEXT' (the host's text arrives so) gets it
 * translated, and one putting only 'utxt' has 'TEXT' put beside it, which
 * the host's pasteboard then gets.
 */
#include <stdlib.h>
#include <string.h>

#include "carbon_svc.h"

enum {
    noTypeErr = -102,
    internalScrapErr = -4988,
    duplicateScrapFlavorErr = -4989,
    badScrapRefErr = -4990,
    scrapPromiseNotKeptErr = -4992,
    noScrapPromiseKeeperErr = -4993,
    scrapFlavorSizeMismatchErr = -4996,
    illegalScrapFlavorFlagsErr = -4997,
    illegalScrapFlavorTypeErr = -4998,
    illegalScrapFlavorSizeErr = -4999,
};
enum { kScrapFlavorMaskSenderOnly = 1, kScrapFlavorMaskTranslated = 2 };
enum { kScrapFlavorSizeUnknown = -1 };
#define TYPE_TEXT 0x54455854u      /* 'TEXT' */
#define TYPE_UTXT 0x75747874u      /* 'utxt' */
#define TYPE_RESERVED 0x73726674u  /* 'srft' */
#define REF_TAG 0x53430000u        /* refs are this and the scrap count */
enum { uppScrapPromiseKeeperProcInfo = 0x00000FF0 };

enum { MAX_FLAVORS = 64 };
typedef struct {
    uint32_t type, flags;
    int32_t size;   /* a promise's size (or -1) */
    bool promised;  /* no data yet */
    bool translated;
} flavor;
static flavor flavors[MAX_FLAVORS];
static int nflavors;
static uint16_t flavors_count = 0xFFFF; /* the scrap count they belong to */
static gaddr keeper, keeper_data;

void carbon_scrap_reset(void)
{
    nflavors = 0;
    flavors_count = 0xFFFF;
    keeper = keeper_data = 0;
}

static uint16_t scrap_count(void)
{
    tb_InfoScrap(); /* the host's text, if any came */
    return rd16(LM_ScrapCount);
}

/* What we know of the flavours, for this scrap. */
static void sync_flavors(void)
{
    uint16_t n = scrap_count();
    if (n != flavors_count) {
        nflavors = 0;
        flavors_count = n;
    }
}

static gaddr current_ref(void) { return REF_TAG | scrap_count(); }
static bool ref_ok(gaddr ref) { return ref == current_ref(); }

static flavor *find_flavor(uint32_t type)
{
    for (int i = 0; i < nflavors; i++)
        if (flavors[i].type == type)
            return &flavors[i];
    return NULL;
}

static flavor *add_flavor(uint32_t type)
{
    flavor *f = find_flavor(type);
    if (!f && nflavors < MAX_FLAVORS) {
        f = &flavors[nflavors++];
        memset(f, 0, sizeof *f);
        f->type = type;
    }
    return f;
}

/* The desk scrap's entry for a type: its data's offset in the scrap
 * handle and its length, or -1. */
static int32_t scrap_entry(uint32_t type, gaddr *data)
{
    gaddr h = rd32(LM_ScrapHandle);
    if (!h || !rd32(h))
        return -1;
    int32_t size = (int32_t)rd32(LM_ScrapSize), pos = 0;
    while (pos + 8 <= size) {
        gaddr q = rd32(h) + (uint32_t)pos;
        int32_t len = (int32_t)rd32(q + 4);
        if (rd32(q) == type) {
            *data = q + 8;
            return len;
        }
        pos += 8 + ((len + 1) & ~1);
    }
    return -1;
}

/* Ask the promise keeper for a promised flavour. */
static void keep_promise(uint32_t type)
{
    flavor *f = find_flavor(type);
    if (!f || !f->promised || !keeper)
        return;
    uint32_t args[3] = { current_ref(), type, keeper_data };
    svc_call_upp(keeper, uppScrapPromiseKeeperProcInfo, args, 3);
}

/* A flavour's data as a new pointer block (*len its length), translating
 * between text and Unicode text where only the other is there; 0 when
 * the scrap has neither. */
static gaddr flavor_data(uint32_t type, int32_t *len, bool *translated)
{
    *translated = false;
    keep_promise(type);
    gaddr d;
    int32_t n = scrap_entry(type, &d);
    if (n >= 0) {
        gaddr p = tb_NewPtr(n ? n : 1, false, false);
        if (p)
            gmemcpy(p, d, (uint32_t)n);
        *len = n;
        return p;
    }
    if (type != TYPE_UTXT && type != TYPE_TEXT)
        return 0;
    uint32_t other = type == TYPE_UTXT ? TYPE_TEXT : TYPE_UTXT;
    keep_promise(other);
    n = scrap_entry(other, &d);
    if (n < 0)
        return 0;
    gaddr p;
    if (type == TYPE_UTXT) {
        uint8_t *b = malloc((size_t)n + 1);
        uint16_t *u = malloc(sizeof *u * ((size_t)n + 1));
        gget(b, d, (uint32_t)n);
        size_t k = svc_macroman_to_utf16(b, (size_t)n, u);
        p = tb_NewPtr((int32_t)(2 * k + 1), false, false);
        for (size_t i = 0; p && i < k; i++)
            wr16(p + 2 * (uint32_t)i, u[i]);
        *len = (int32_t)(2 * k);
        free(b);
        free(u);
    } else {
        size_t units = (size_t)n / 2;
        uint16_t *u = malloc(sizeof *u * (units + 1));
        uint8_t *b = malloc(units + 1);
        for (size_t i = 0; i < units; i++)
            u[i] = rd16(d + 2 * (uint32_t)i);
        size_t k = svc_utf16_to_macroman(u, units, b);
        p = tb_NewPtr((int32_t)(k + 1), false, false);
        if (p)
            gput(p, b, (uint32_t)k);
        *len = (int32_t)k;
        free(b);
        free(u);
    }
    *translated = true;
    return p;
}

/* GetCurrentScrap(&scrap) */
static void sc_GetCurrentScrap(ppc_cpu *c)
{
    if (!A(0))
        RET(paramErr);
    wr32(A(0), current_ref());
    RET(noErr);
}

/* GetScrapFlavorFlags(scrap, type, &flags): never blocks; promised
 * flavours count. */
static void sc_GetScrapFlavorFlags(ppc_cpu *c)
{
    gaddr ref = A(0), out = A(2);
    uint32_t type = A(1);
    if (!ref_ok(ref))
        RET(badScrapRefErr);
    if (!out)
        RET(paramErr);
    sync_flavors();
    flavor *f = find_flavor(type);
    gaddr d;
    if (f || scrap_entry(type, &d) >= 0) {
        wr32(out, f ? f->flags : 0);
        RET(noErr);
    }
    uint32_t other = type == TYPE_UTXT ? TYPE_TEXT : type == TYPE_TEXT ? TYPE_UTXT : 0;
    if (other && (find_flavor(other) || scrap_entry(other, &d) >= 0)) {
        wr32(out, kScrapFlavorMaskTranslated);
        RET(noErr);
    }
    RET(noTypeErr);
}

/* GetScrapFlavorSize(scrap, type, &size) */
static void sc_GetScrapFlavorSize(ppc_cpu *c)
{
    gaddr ref = A(0), out = A(2);
    if (!ref_ok(ref))
        RET(badScrapRefErr);
    if (!out)
        RET(paramErr);
    sync_flavors();
    flavor *f = find_flavor(A(1));
    if (f && f->promised && f->size >= 0 && !keeper) {
        wr32(out, (uint32_t)f->size); /* the size promised */
        RET(noErr);
    }
    int32_t len;
    bool translated;
    gaddr p = flavor_data(A(1), &len, &translated);
    if (!p) {
        f = find_flavor(A(1));
        RET(f && f->promised ? (keeper ? scrapPromiseNotKeptErr : noScrapPromiseKeeperErr) : noTypeErr);
    }
    tb_DisposePtr(p);
    wr32(out, (uint32_t)len);
    RET(noErr);
}

/* GetScrapFlavorData(scrap, type, &byteCount, destination): at most
 * *byteCount bytes copied; *byteCount becomes the bytes available. */
static void sc_GetScrapFlavorData(ppc_cpu *c)
{
    gaddr ref = A(0), count = A(2), dest = A(3);
    if (!ref_ok(ref))
        RET(badScrapRefErr);
    if (!count || !dest)
        RET(paramErr);
    sync_flavors();
    int32_t len;
    bool translated;
    gaddr p = flavor_data(A(1), &len, &translated);
    if (!p) {
        flavor *f = find_flavor(A(1));
        RET(f && f->promised ? (keeper ? scrapPromiseNotKeptErr : noScrapPromiseKeeperErr) : noTypeErr);
    }
    int32_t want = (int32_t)rd32(count);
    gmemcpy(dest, p, (uint32_t)(want < len ? (want < 0 ? 0 : want) : len));
    tb_DisposePtr(p);
    wr32(count, (uint32_t)len);
    RET(noErr);
}

/* ClearCurrentScrap() */
static void sc_ClearCurrentScrap(ppc_cpu *c)
{
    tb_ZeroScrap();
    nflavors = 0;
    flavors_count = rd16(LM_ScrapCount);
    keeper = keeper_data = 0;
    RET(noErr);
}

/* PutScrapFlavor(scrap, type, flags, size, data): data NULL is a promise
 * (of size bytes, or kScrapFlavorSizeUnknown); size 0 says there will be
 * none. */
static void sc_PutScrapFlavor(ppc_cpu *c)
{
    gaddr ref = A(0), data = A(4);
    uint32_t type = A(1), flags = A(2);
    int32_t size = (int32_t)A(3);
    if (!ref_ok(ref))
        RET(badScrapRefErr);
    if (type == TYPE_RESERVED || type == 0)
        RET(illegalScrapFlavorTypeErr);
    if (flags & ~(uint32_t)(kScrapFlavorMaskSenderOnly | kScrapFlavorMaskTranslated))
        RET(illegalScrapFlavorFlagsErr);
    if (size < kScrapFlavorSizeUnknown || (size == kScrapFlavorSizeUnknown && data))
        RET(illegalScrapFlavorSizeErr);
    sync_flavors();
    flavor *f = find_flavor(type);
    gaddr d;
    bool there = scrap_entry(type, &d) >= 0;
    if (there && !(f && f->translated))
        RET(duplicateScrapFlavorErr);
    if (f && f->promised && f->size >= 0 && data && size != f->size)
        RET(scrapFlavorSizeMismatchErr);
    if (!data && size != 0) { /* a promise */
        if (f && !f->promised)
            RET(duplicateScrapFlavorErr);
        f = add_flavor(type);
        if (!f)
            RET(internalScrapErr);
        f->flags = flags;
        f->size = size;
        f->promised = true;
        RET(noErr);
    }
    if (there) /* our translation, which says the same, stands */
        RET(noErr);
    int32_t e = tb_PutScrap(size, (int32_t)type, data);
    if (e != noErr)
        RET(e);
    f = add_flavor(type);
    if (f) {
        f->flags = flags;
        f->size = size;
        f->promised = false;
        f->translated = false;
    }
    /* Unicode text alone: plain text beside it, for programs that read
     * only that, and for the host. */
    if (type == TYPE_UTXT && scrap_entry(TYPE_TEXT, &d) < 0 && !(find_flavor(TYPE_TEXT))) {
        int32_t len;
        bool tr;
        gaddr p = flavor_data(TYPE_TEXT, &len, &tr);
        if (p) {
            tb_PutScrap(len, (int32_t)TYPE_TEXT, p);
            tb_DisposePtr(p);
            flavor *t = add_flavor(TYPE_TEXT);
            if (t) {
                t->flags = kScrapFlavorMaskTranslated;
                t->size = len;
                t->translated = true;
            }
        }
    }
    RET(noErr);
}

/* SetScrapPromiseKeeper(scrap, upp, userData) */
static void sc_SetScrapPromiseKeeper(ppc_cpu *c)
{
    if (!ref_ok(A(0)))
        RET(badScrapRefErr);
    keeper = A(1);
    keeper_data = A(2);
    RET(noErr);
}

/* The flavours, in the scrap's order, then promised ones. */
static int list_flavors(uint32_t *types, uint32_t *flags, int max)
{
    sync_flavors();
    int n = 0;
    gaddr h = rd32(LM_ScrapHandle);
    int32_t size = (int32_t)rd32(LM_ScrapSize), pos = 0;
    while (h && rd32(h) && pos + 8 <= size && n < max) {
        gaddr q = rd32(h) + (uint32_t)pos;
        flavor *f = find_flavor(rd32(q));
        types[n] = rd32(q);
        flags[n++] = f ? f->flags : 0;
        pos += 8 + (((int32_t)rd32(q + 4) + 1) & ~1);
    }
    for (int i = 0; i < nflavors && n < max; i++)
        if (flavors[i].promised) {
            types[n] = flavors[i].type;
            flags[n++] = flavors[i].flags;
        }
    /* Text and Unicode text, each for the other. */
    bool text = false, utxt = false;
    for (int i = 0; i < n; i++)
        text |= types[i] == TYPE_TEXT, utxt |= types[i] == TYPE_UTXT;
    if (text != utxt && n < max) {
        types[n] = text ? TYPE_UTXT : TYPE_TEXT;
        flags[n++] = kScrapFlavorMaskTranslated;
    }
    return n;
}

/* GetScrapFlavorCount(scrap, &count) */
static void sc_GetScrapFlavorCount(ppc_cpu *c)
{
    if (!ref_ok(A(0)))
        RET(badScrapRefErr);
    if (!A(1))
        RET(paramErr);
    uint32_t types[MAX_FLAVORS * 2], flags[MAX_FLAVORS * 2];
    wr32(A(1), (uint32_t)list_flavors(types, flags, MAX_FLAVORS * 2));
    RET(noErr);
}

/* GetScrapFlavorInfoList(scrap, &count, info[]): ScrapFlavorInfo is
 * the type and the flags. */
static void sc_GetScrapFlavorInfoList(ppc_cpu *c)
{
    if (!ref_ok(A(0)))
        RET(badScrapRefErr);
    gaddr count = A(1), info = A(2);
    if (!count || (!info && rd32(count)))
        RET(paramErr);
    uint32_t types[MAX_FLAVORS * 2], flags[MAX_FLAVORS * 2];
    int n = list_flavors(types, flags, MAX_FLAVORS * 2);
    uint32_t max = rd32(count), k = 0;
    for (; k < (uint32_t)n && k < max; k++) {
        wr32(info + 8 * k, types[k]);
        wr32(info + 8 * k + 4, flags[k]);
    }
    wr32(count, k);
    RET(noErr);
}

/* CallInScrapPromises(): every promise kept now. */
static void sc_CallInScrapPromises(ppc_cpu *c)
{
    sync_flavors();
    bool any = false;
    for (int i = 0; i < nflavors; i++)
        if (flavors[i].promised) {
            any = true;
            keep_promise(flavors[i].type);
        }
    RET(any && !keeper ? noScrapPromiseKeeperErr : noErr);
}

const ppc_export carbon_scrap_exports[] = {
    { "CallInScrapPromises", sc_CallInScrapPromises },
    { "ClearCurrentScrap", sc_ClearCurrentScrap },
    { "GetCurrentScrap", sc_GetCurrentScrap },
    { "GetScrapFlavorCount", sc_GetScrapFlavorCount },
    { "GetScrapFlavorData", sc_GetScrapFlavorData },
    { "GetScrapFlavorFlags", sc_GetScrapFlavorFlags },
    { "GetScrapFlavorInfoList", sc_GetScrapFlavorInfoList },
    { "GetScrapFlavorSize", sc_GetScrapFlavorSize },
    { "PutScrapFlavor", sc_PutScrapFlavor },
    { "SetScrapPromiseKeeper", sc_SetScrapPromiseKeeper },
};
const unsigned carbon_scrap_export_count = sizeof carbon_scrap_exports / sizeof carbon_scrap_exports[0];
