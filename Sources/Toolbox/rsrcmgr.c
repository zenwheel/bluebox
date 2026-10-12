/*
 * Resource Manager (read side).
 *
 * Each open resource file has its map loaded into a guest handle in the
 * original format, chained through the "next map" field from TopMapHndl, so
 * apps that walk maps directly see what they expect.  Reference entries hold
 * the resource's handle once loaded.  Resource data comes from the host copy
 * of the fork and is decompressed on load when needed.
 *
 * Map layout: 16-byte header copy, next map handle (16), refnum (20),
 * file attributes (22), type list offset (24), name list offset (26).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forks.h"
#include "tb_internal.h"

#define MAX_RESFILES 64

typedef struct {
    bool used;
    int16_t refnum;
    char *path;       /* host path, NULL for in-memory files */
    uint8_t *fork;    /* host copy of the resource fork */
    uint32_t len;
    gaddr maph;       /* guest handle to the map */
    bool read_only;
} resfile;

static resfile files[MAX_RESFILES];
static int16_t sys_refnum;

/* FCB-style reference numbers: 2, 96, 190, ... */
static int16_t refnum_for_slot(int i) { return (int16_t)(2 + i * 94); }

static resfile *file_for(int16_t rn)
{
    for (int i = 0; i < MAX_RESFILES; i++)
        if (files[i].used && files[i].refnum == rn)
            return &files[i];
    return NULL;
}

static resfile *file_for_map(gaddr maph)
{
    for (int i = 0; i < MAX_RESFILES; i++)
        if (files[i].used && files[i].maph == maph)
            return &files[i];
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* Map access (offsets, because the map handle can move) */

static inline gaddr mp_of(gaddr maph) { return rd32(maph); }
static inline gaddr map_next(gaddr maph) { return rd32(mp_of(maph) + 16); }
static inline int16_t map_refnum(gaddr maph) { return (int16_t)rd16(mp_of(maph) + 20); }

static unsigned map_ntypes(gaddr maph)
{
    gaddr m = mp_of(maph);
    uint16_t n = rd16(m + rd16(m + 24));
    return n == 0xFFFF ? 0 : (unsigned)n + 1;
}

/* Offset (from map start) of type entry i. */
static uint32_t type_off(gaddr maph, unsigned i)
{
    gaddr m = mp_of(maph);
    return rd16(m + 24) + 2u + i * 8u;
}

static uint32_t type_code(gaddr maph, unsigned i) { return rd32(mp_of(maph) + type_off(maph, i)); }
static unsigned type_count(gaddr maph, unsigned i) { return (unsigned)rd16(mp_of(maph) + type_off(maph, i) + 4) + 1; }

/* Offset (from map start) of reference j of type i. */
static uint32_t ref_off(gaddr maph, unsigned i, unsigned j)
{
    gaddr m = mp_of(maph);
    return rd16(m + 24) + (uint32_t)rd16(m + type_off(maph, i) + 6) + j * 12u;
}

static int find_type(gaddr maph, uint32_t type)
{
    unsigned n = map_ntypes(maph);
    for (unsigned i = 0; i < n; i++)
        if (type_code(maph, i) == type)
            return (int)i;
    return -1;
}

/* Find (type, id) in one map; returns ref offset or 0. */
static uint32_t find_ref(gaddr maph, uint32_t type, int16_t id)
{
    int t = find_type(maph, type);
    if (t < 0)
        return 0;
    unsigned n = type_count(maph, (unsigned)t);
    for (unsigned j = 0; j < n; j++) {
        uint32_t r = ref_off(maph, (unsigned)t, j);
        if ((int16_t)rd16(mp_of(maph) + r) == id)
            return r;
    }
    return 0;
}

static bool name_matches(gaddr maph, uint32_t r, gaddr pname)
{
    gaddr m = mp_of(maph);
    uint16_t noff = rd16(m + r + 2);
    if (noff == 0xFFFF)
        return false;
    gaddr n = m + rd16(m + 26) + noff;
    uint8_t len = rd8(n);
    if (len != rd8(pname))
        return false;
    for (unsigned k = 1; k <= len; k++) {
        uint8_t a = rd8(n + k), b = rd8(pname + k);
        if (a >= 'a' && a <= 'z')
            a -= 32;
        if (b >= 'a' && b <= 'z')
            b -= 32;
        if (a != b)
            return false;
    }
    return true;
}

/* The map to start a search from: CurMap, or the top if CurMap is unknown. */
static gaddr cur_map(void)
{
    int16_t cur = (int16_t)rd16(LM_CurMap);
    for (gaddr h = rd32(LM_TopMapHndl); h; h = map_next(h))
        if (map_refnum(h) == cur)
            return h;
    return rd32(LM_TopMapHndl);
}

/* Locate the reference entry holding resource handle h. */
static bool find_handle(gaddr h, gaddr *maph_out, uint32_t *ref_out, uint32_t *type_out)
{
    if (!h)
        return false;
    for (gaddr maph = rd32(LM_TopMapHndl); maph; maph = map_next(maph)) {
        unsigned nt = map_ntypes(maph);
        for (unsigned i = 0; i < nt; i++) {
            unsigned n = type_count(maph, i);
            for (unsigned j = 0; j < n; j++) {
                uint32_t r = ref_off(maph, i, j);
                if (rd32(mp_of(maph) + r + 8) == h) {
                    *maph_out = maph;
                    *ref_out = r;
                    if (type_out)
                        *type_out = type_code(maph, i);
                    return true;
                }
            }
        }
    }
    return false;
}

/* ---------------------------------------------------------------------- */
/* Loading */

/* Read the data for ref r of map maph into handle h (allocating if 0). */
static gaddr load_ref(gaddr maph, uint32_t r, uint32_t type)
{
    resfile *f = file_for_map(maph);
    gaddr m = mp_of(maph);
    uint8_t attrs = rd8(m + r + 4);
    uint32_t doff = rd32(m + r + 4) & 0x00FFFFFF;
    gaddr h = rd32(m + r + 8);

    if (h && rd32(h))
        return h; /* already loaded */
    if (!f) {
        set_res_err(mapReadErr);
        return 0;
    }
    if (!rd8(LM_ResLoad)) {
        if (!h) {
            h = tb_NewEmptyHandle(attrs & RES_SYS_HEAP);
            if (!h)
                return 0;
            wr32(mp_of(maph) + r + 8, h);
            tb_HSetRBit(h);
        }
        set_res_err(noErr);
        return h;
    }

    uint32_t dstart = rd32(m + 0); /* header copy: data offset */
    if ((uint64_t)dstart + doff + 4 > f->len) {
        set_res_err(mapReadErr);
        return 0;
    }
    const uint8_t *src = f->fork + dstart + doff;
    uint32_t len = ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) | ((uint32_t)src[2] << 8) | src[3];
    src += 4;
    if ((uint64_t)dstart + doff + 4 + len > f->len) {
        set_res_err(mapReadErr);
        return 0;
    }
    uint8_t *dec = NULL;
    size_t declen = 0;
    if ((attrs & RES_COMPRESSED) && rsrc_is_compressed(src, len)) {
        char err[128];
        if (!rsrc_decompress(src, len, &dec, &declen, err, sizeof err)) {
            char tc[5];
            log_printf("rsrc: '%s' %d: %s\n", fourcc_str(type, tc), (int16_t)rd16(m + r), err);
            set_res_err(mapReadErr);
            return 0;
        }
        src = dec;
        len = (uint32_t)declen;
    }

    gaddr zone = (attrs & RES_SYS_HEAP) ? rd32(LM_SysZone) : rd32(LM_TheZone);
    if (h) {
        gaddr saved = rd32(LM_TheZone);
        wr32(LM_TheZone, zone);
        tb_ReallocateHandle(h, (int32_t)len);
        wr32(LM_TheZone, saved);
        if (!rd32(h)) {
            free(dec);
            set_res_err(memFullErr);
            return 0;
        }
    } else {
        h = mm_new_handle_in(zone, len, false);
        if (!h) {
            free(dec);
            set_res_err(memFullErr);
            return 0;
        }
        wr32(mp_of(maph) + r + 8, h);
    }
    gput(rd32(h), src, len);
    free(dec);
    tb_HSetRBit(h);
    if (attrs & RES_PURGEABLE)
        tb_HPurge(h);
    if (attrs & RES_LOCKED)
        tb_HLock(h);
    if (g_log_mask & LOG_RSRC) {
        char tc[5];
        log_printf("rsrc: loaded '%s' %d (%u bytes) -> %08X\n", fourcc_str(type, tc),
                   (int16_t)rd16(mp_of(maph) + r), len, h);
    }
    set_res_err(noErr);
    return h;
}

static gaddr get_res(uint32_t type, int16_t id, bool one)
{
    for (gaddr maph = cur_map(); maph; maph = map_next(maph)) {
        uint32_t r = find_ref(maph, type, id);
        if (r)
            return load_ref(maph, r, type);
        if (one)
            break;
    }
    if (g_log_mask & LOG_RSRC) {
        char tc[5];
        log_printf("rsrc: '%s' %d not found\n", fourcc_str(type, tc), id);
    }
    /* Real System 7.1 leaves ResError at noErr for a missing resource
     * (oracle: Tests/Probes/MemRsrc/expected-system71.txt). */
    set_res_err(noErr);
    return 0;
}

gaddr tb_GetResource(int32_t typ, int16_t id) { return get_res((uint32_t)typ, id, false); }
gaddr tb_Get1Resource(int32_t typ, int16_t id) { return get_res((uint32_t)typ, id, true); }
gaddr tb_RGetResource(int32_t typ, int16_t id) { return get_res((uint32_t)typ, id, false); }

static gaddr get_named(uint32_t type, gaddr name, bool one)
{
    for (gaddr maph = cur_map(); maph; maph = map_next(maph)) {
        int t = find_type(maph, type);
        if (t >= 0) {
            unsigned n = type_count(maph, (unsigned)t);
            for (unsigned j = 0; j < n; j++) {
                uint32_t r = ref_off(maph, (unsigned)t, j);
                if (name_matches(maph, r, name))
                    return load_ref(maph, r, type);
            }
        }
        if (one)
            break;
    }
    set_res_err(resNotFound);
    return 0;
}

gaddr tb_GetNamedResource(int32_t typ, gaddr nam) { return get_named((uint32_t)typ, nam, false); }
gaddr tb_Get1NamedResource(int32_t typ, gaddr s) { return get_named((uint32_t)typ, s, true); }

static int16_t count_res(uint32_t type, bool one)
{
    int n = 0;
    for (gaddr maph = cur_map(); maph; maph = map_next(maph)) {
        int t = find_type(maph, type);
        if (t >= 0)
            n += (int)type_count(maph, (unsigned)t);
        if (one)
            break;
    }
    set_res_err(noErr);
    return (int16_t)n;
}

int16_t tb_CountResources(int32_t typ) { return count_res((uint32_t)typ, false); }
int16_t tb_Count1Resources(int32_t typ) { return count_res((uint32_t)typ, true); }

static gaddr ind_res(uint32_t type, int16_t index, bool one)
{
    if (index < 1) {
        set_res_err(resNotFound);
        return 0;
    }
    int remaining = index;
    for (gaddr maph = cur_map(); maph; maph = map_next(maph)) {
        int t = find_type(maph, type);
        if (t >= 0) {
            unsigned n = type_count(maph, (unsigned)t);
            if ((unsigned)remaining <= n)
                return load_ref(maph, ref_off(maph, (unsigned)t, (unsigned)remaining - 1), type);
            remaining -= (int)n;
        }
        if (one)
            break;
    }
    set_res_err(resNotFound);
    return 0;
}

gaddr tb_GetIndResource(int32_t typ, int16_t indx) { return ind_res((uint32_t)typ, indx, false); }
gaddr tb_Get1IndResource(int32_t typ, int16_t i) { return ind_res((uint32_t)typ, i, true); }

/* Distinct types, in search order. */
static int16_t types_visit(bool one, int16_t want, uint32_t *found)
{
    uint32_t seen[512];
    int ns = 0;
    for (gaddr maph = cur_map(); maph; maph = map_next(maph)) {
        unsigned nt = map_ntypes(maph);
        for (unsigned i = 0; i < nt; i++) {
            uint32_t t = type_code(maph, i);
            bool dup = false;
            for (int k = 0; k < ns; k++)
                if (seen[k] == t)
                    dup = true;
            if (dup || ns >= 512)
                continue;
            seen[ns++] = t;
            if (ns == want && found) {
                *found = t;
                return (int16_t)ns;
            }
        }
        if (one)
            break;
    }
    return (int16_t)ns;
}

int16_t tb_CountTypes(void) { set_res_err(noErr); return types_visit(false, -1, NULL); }
int16_t tb_Count1Types(void) { set_res_err(noErr); return types_visit(true, -1, NULL); }

static void ind_type(gaddr typ, int16_t indx, bool one)
{
    uint32_t t = 0;
    if (indx >= 1 && types_visit(one, indx, &t) == indx) {
        wr32(typ, t);
        set_res_err(noErr);
    } else {
        wr32(typ, 0);
        set_res_err(resNotFound);
    }
}

void tb_GetIndType(gaddr typ, int16_t indx) { ind_type(typ, indx, false); }
void tb_Get1IndType(gaddr typ, int16_t indx) { ind_type(typ, indx, true); }

void tb_LoadResource(gaddr res)
{
    gaddr maph;
    uint32_t r, type;
    if (!find_handle(res, &maph, &r, &type)) {
        set_res_err(resNotFound);
        return;
    }
    if (rd32(res)) {
        set_res_err(noErr);
        return;
    }
    uint8_t saved = rd8(LM_ResLoad);
    wr8(LM_ResLoad, 1);
    load_ref(maph, r, type);
    wr8(LM_ResLoad, saved);
}

void tb_ReleaseResource(gaddr res)
{
    gaddr maph;
    uint32_t r;
    if (!find_handle(res, &maph, &r, NULL)) {
        set_res_err(resNotFound);
        return;
    }
    wr32(mp_of(maph) + r + 8, 0);
    tb_HClrRBit(res);
    tb_DisposeHandle(res);
    set_res_err(noErr);
}

void tb_DetachResource(gaddr res)
{
    gaddr maph;
    uint32_t r;
    if (!find_handle(res, &maph, &r, NULL)) {
        set_res_err(resNotFound);
        return;
    }
    wr32(mp_of(maph) + r + 8, 0);
    tb_HClrRBit(res);
    set_res_err(noErr);
}

void tb_GetResInfo(gaddr res, gaddr id1, gaddr typ, gaddr name)
{
    gaddr maph;
    uint32_t r, type;
    if (!find_handle(res, &maph, &r, &type)) {
        set_res_err(resNotFound);
        return;
    }
    gaddr m = mp_of(maph);
    if (id1)
        wr16(id1, rd16(m + r));
    if (typ)
        wr32(typ, type);
    if (name) {
        uint16_t noff = rd16(m + r + 2);
        if (noff == 0xFFFF) {
            wr8(name, 0);
        } else {
            gaddr n = m + rd16(m + 26) + noff;
            gmemcpy(name, n, (uint32_t)rd8(n) + 1);
        }
    }
    set_res_err(noErr);
}

int16_t tb_GetResAttrs(gaddr res)
{
    gaddr maph;
    uint32_t r;
    if (!find_handle(res, &maph, &r, NULL)) {
        set_res_err(resNotFound);
        return 0;
    }
    set_res_err(noErr);
    return rd8(mp_of(maph) + r + 4);
}

static int32_t size_on_disk(gaddr res, bool decompressed)
{
    gaddr maph;
    uint32_t r;
    if (!find_handle(res, &maph, &r, NULL)) {
        set_res_err(resNotFound);
        return -1;
    }
    resfile *f = file_for_map(maph);
    gaddr m = mp_of(maph);
    uint32_t doff = rd32(m + r + 4) & 0x00FFFFFF;
    uint32_t dstart = rd32(m);
    if (!f || (uint64_t)dstart + doff + 4 > f->len) {
        set_res_err(mapReadErr);
        return -1;
    }
    const uint8_t *src = f->fork + dstart + doff;
    uint32_t len = ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) | ((uint32_t)src[2] << 8) | src[3];
    set_res_err(noErr);
    if (decompressed && (uint64_t)dstart + doff + 4 + len <= f->len)
        return (int32_t)rsrc_decompressed_size(src + 4, len);
    return (int32_t)len;
}

int32_t tb_GetResourceSizeOnDisk(gaddr res) { return size_on_disk(res, false); }

int32_t tb_GetMaxResourceSize(gaddr h)
{
    if (h && rd32(h))
        return tb_GetHandleSize(h);
    return size_on_disk(h, true);
}

int16_t tb_HomeResFile(gaddr res)
{
    gaddr maph;
    uint32_t r;
    if (!find_handle(res, &maph, &r, NULL)) {
        set_res_err(resNotFound);
        return -1;
    }
    set_res_err(noErr);
    return map_refnum(maph);
}

int16_t tb_CurResFile(void) { return (int16_t)rd16(LM_CurMap); }

void tb_UseResFile(int16_t rn)
{
    if (rn == 0)
        rn = sys_refnum;
    if (!file_for(rn)) {
        set_res_err(resFNotFound);
        return;
    }
    wr16(LM_CurMap, (uint16_t)rn);
    set_res_err(noErr);
}

void tb_SetResLoad(int8_t load) { wr8(LM_ResLoad, load ? 1 : 0); }
int16_t tb_ResError(void) { return (int16_t)rd16(LM_ResErr); }
void tb_SetResPurge(int8_t install) {}
int16_t tb_InitResources(void) { return sys_refnum; }
void tb_RsrcZoneInit(void) {}

int16_t tb_GetResFileAttrs(int16_t rn)
{
    resfile *f = file_for(rn ? rn : sys_refnum);
    if (!f) {
        set_res_err(resFNotFound);
        return 0;
    }
    set_res_err(noErr);
    return (int16_t)rd16(mp_of(f->maph) + 22);
}

void tb_SetResFileAttrs(int16_t rn, int16_t attrs)
{
    resfile *f = file_for(rn ? rn : sys_refnum);
    if (!f) {
        set_res_err(resFNotFound);
        return;
    }
    wr16(mp_of(f->maph) + 22, (uint16_t)attrs);
    set_res_err(noErr);
}

static int16_t unique_id(uint32_t type, bool one)
{
    for (int id = 128; id < 32767; id++) {
        bool used = false;
        for (gaddr maph = cur_map(); maph && !used; maph = one ? 0 : map_next(maph))
            used = find_ref(maph, type, (int16_t)id) != 0;
        if (!used)
            return (int16_t)id;
    }
    return -1;
}

int16_t tb_UniqueID(int32_t typ) { return unique_id((uint32_t)typ, false); }
int16_t tb_Unique1ID(int32_t typ) { return unique_id((uint32_t)typ, true); }

/* Toolbox Utilities that are thin wrappers on GetResource. */
gaddr tb_GetString(int16_t i) { return tb_GetResource((int32_t)fourcc("STR "), i); }
gaddr tb_GetPattern(int16_t id) { return tb_GetResource((int32_t)fourcc("PAT "), id); }
gaddr tb_GetCursor(int16_t id) { return tb_GetResource((int32_t)fourcc("CURS"), id); }
gaddr tb_GetPicture(int16_t id) { return tb_GetResource((int32_t)fourcc("PICT"), id); }
gaddr tb_GetIcon(int16_t id) { return tb_GetResource((int32_t)fourcc("ICON"), id); }

/* ---------------------------------------------------------------------- */
/* Opening and closing */

/* An empty map: header + map header + empty type list. */
static const uint8_t empty_fork[] = {
    0, 0, 0, 0x10, 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0, 0x1E,
    0, 0, 0, 0x10, 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0, 0x1E,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0x1C, 0, 0x1E, 0xFF, 0xFF,
};

static int16_t open_fork(const char *path, const uint8_t *fork, uint32_t len, bool read_only, gaddr zone)
{
    char err[160];
    rsrc_index idx;
    if (!rsrc_parse(fork, len, &idx, err, sizeof err) || len == 0) {
        if (len)
            log_printf("rsrc: %s: %s\n", path ? path : "(memory)", err);
        set_res_err(mapReadErr);
        return -1;
    }
    rsrc_index_free(&idx);
    int slot = -1;
    for (int i = 0; i < MAX_RESFILES; i++) {
        if (!files[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        set_res_err(memFullErr);
        return -1;
    }
    uint32_t mstart = ((uint32_t)fork[4] << 24) | ((uint32_t)fork[5] << 16) | ((uint32_t)fork[6] << 8) | fork[7];
    uint32_t mlen = ((uint32_t)fork[12] << 24) | ((uint32_t)fork[13] << 16) | ((uint32_t)fork[14] << 8) | fork[15];
    gaddr maph = mm_new_handle_in(zone, mlen, false);
    if (!maph) {
        set_res_err(memFullErr);
        return -1;
    }
    gaddr m = rd32(maph);
    gput(m, fork + mstart, mlen);
    gput(m, fork, 16); /* header copy */
    /* Clear stored handles (they are meaningless on disk). */
    unsigned nt = rd16(m + rd16(m + 24)) == 0xFFFF ? 0 : rd16(m + rd16(m + 24)) + 1u;
    for (unsigned i = 0; i < nt; i++) {
        uint32_t to = rd16(m + 24) + 2u + i * 8u;
        unsigned n = rd16(m + to + 4) + 1u;
        uint32_t rl = rd16(m + 24) + (uint32_t)rd16(m + to + 6);
        for (unsigned j = 0; j < n; j++)
            wr32(m + rl + j * 12 + 8, 0);
    }
    resfile *f = &files[slot];
    f->used = true;
    f->refnum = refnum_for_slot(slot);
    f->path = path ? strdup(path) : NULL;
    f->fork = malloc(len);
    memcpy(f->fork, fork, len);
    f->len = len;
    f->maph = maph;
    f->read_only = read_only;
    wr16(m + 20, (uint16_t)f->refnum);
    if (read_only)
        wr16(m + 22, rd16(m + 22) | 0x80); /* mapReadOnly */

    /* Link at the top of the chain and make it current. */
    wr32(m + 16, rd32(LM_TopMapHndl));
    wr32(LM_TopMapHndl, maph);
    wr16(LM_CurMap, (uint16_t)f->refnum);
    /* Preload resources marked preload. */
    for (unsigned i = 0; i < map_ntypes(maph); i++) {
        unsigned n = type_count(maph, i);
        for (unsigned j = 0; j < n; j++) {
            uint32_t r = ref_off(maph, i, j);
            if (rd8(mp_of(maph) + r + 4) & RES_PRELOAD)
                load_ref(maph, r, type_code(maph, i));
        }
    }
    set_res_err(noErr);
    return f->refnum;
}

int16_t rm_open_host_file(const char *path, const uint8_t *fork, uint32_t len, bool read_only)
{
    return open_fork(path, fork, len, read_only, rd32(LM_TheZone));
}

void rm_set_system(int16_t rn)
{
    sys_refnum = rn;
    wr32(LM_SysMapHndl, rd32(LM_TopMapHndl));
    wr16(LM_SysMap, (uint16_t)rn);
}

int16_t rm_open_empty_system(void)
{
    int16_t rn = open_fork(NULL, empty_fork, sizeof empty_fork, true, rd32(LM_SysZone));
    if (rn >= 0) {
        sys_refnum = rn;
        wr32(LM_SysMapHndl, rd32(LM_TopMapHndl));
        wr16(LM_SysMap, (uint16_t)rn);
    }
    return rn;
}

void tb_CloseResFile(int16_t rn)
{
    if (rn == 0 || rn == sys_refnum) {
        set_res_err(noErr); /* the System file stays open */
        return;
    }
    resfile *f = file_for(rn);
    if (!f) {
        set_res_err(resFNotFound);
        return;
    }
    gaddr maph = f->maph;
    /* Dispose loaded resources. */
    for (unsigned i = 0; i < map_ntypes(maph); i++) {
        unsigned n = type_count(maph, i);
        for (unsigned j = 0; j < n; j++) {
            gaddr h = rd32(mp_of(maph) + ref_off(maph, i, j) + 8);
            if (h)
                tb_DisposeHandle(h);
        }
    }
    /* Unlink. */
    gaddr next = map_next(maph);
    if (rd32(LM_TopMapHndl) == maph) {
        wr32(LM_TopMapHndl, next);
    } else {
        for (gaddr h = rd32(LM_TopMapHndl); h; h = map_next(h)) {
            if (map_next(h) == maph) {
                wr32(mp_of(h) + 16, next);
                break;
            }
        }
    }
    if ((int16_t)rd16(LM_CurMap) == rn)
        wr16(LM_CurMap, (uint16_t)(rd32(LM_TopMapHndl) ? map_refnum(rd32(LM_TopMapHndl)) : sys_refnum));
    tb_DisposeHandle(maph);
    free(f->path);
    free(f->fork);
    memset(f, 0, sizeof *f);
    set_res_err(noErr);
}

bool fm_host_path(int16_t vref, int32_t dirid, gaddr name, bool hfs, char *out, size_t outsize);

static int16_t open_path(const char *path)
{
    /* Already open?  The Resource Manager returns the existing refnum. */
    for (int i = 0; i < MAX_RESFILES; i++)
        if (files[i].used && files[i].path && !strcmp(files[i].path, path)) {
            set_res_err(noErr);
            return files[i].refnum;
        }
    mac_file mf;
    char err[256];
    if (!appledouble_read(path, &mf, err, sizeof err)) {
        set_res_err(fnfErr);
        return -1;
    }
    int16_t rn = rm_open_host_file(path, mf.rsrc, (uint32_t)mf.rsrc_len, false);
    mac_file_free(&mf);
    if (rn < 0 && !rd16(LM_ResErr))
        set_res_err(-39); /* eofErr: no resource fork */
    return rn;
}

static int16_t open_named(gaddr fn)
{
    char path[1024];
    if (!fm_host_path(0, 0, fn, false, path, sizeof path)) {
        set_res_err(-35);
        return -1;
    }
    return open_path(path);
}

int16_t tb_HOpenResFile(int16_t vref, int32_t dirid, gaddr file_name, int8_t perm)
{
    char path[1024];
    if (!fm_host_path(vref, dirid, file_name, true, path, sizeof path)) {
        set_res_err(-35);
        return -1;
    }
    return open_path(path);
}

int16_t tb_FSpOpenResFile(gaddr spec, int8_t perms)
{
    return tb_HOpenResFile((int16_t)rd16(spec), (int32_t)rd32(spec + 2), spec + 6, perms);
}

int16_t tb_OpenResFile(gaddr fn) { return open_named(fn); }
int16_t tb_OpenRFPerm(gaddr fn, int16_t vref, uint8_t perm)
{
    char path[1024];
    if (!fm_host_path(vref, 0, fn, false, path, sizeof path)) {
        set_res_err(-35);
        return -1;
    }
    return open_path(path);
}

void rm_init(void)
{
    for (int i = 0; i < MAX_RESFILES; i++) {
        free(files[i].path);
        free(files[i].fork);
    }
    memset(files, 0, sizeof files);
    sys_refnum = 0;
    wr32(LM_TopMapHndl, 0);
    wr8(LM_ResLoad, 1);
    set_res_err(noErr);
}
