/*
 * Resource Manager.
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
#include "trap.h"

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

/* A compressed resource whose decompressor is not built in: run the
 * 'dcmp' resource the file (or System) supplies, as the Resource Manager
 * did.  Header version 8 gets (source, dest, working buffer, size);
 * version 9 gets (size, source, dest, header).  Execution starts at 0 when
 * bytes 4-7 say 'dcmp', else at the offset in bytes 2-3. */
static bool guest_decompress(const uint8_t *d, uint32_t len, uint8_t **out, size_t *out_len)
{
    static int depth;
    int version = d[6];
    int16_t id = version == 8 ? (int16_t)((d[14] << 8) | d[15]) : (int16_t)((d[12] << 8) | d[13]);
    uint32_t hdrlen = (uint32_t)((d[4] << 8) | d[5]);
    uint32_t outlen = ((uint32_t)d[8] << 24) | ((uint32_t)d[9] << 16) | ((uint32_t)d[10] << 8) | d[11];
    if ((version != 8 && version != 9) || hdrlen > len || depth > 0 || !g_cpu)
        return false;
    depth++;
    gaddr code = tb_GetResource((int32_t)fourcc("dcmp"), id);
    bool ok = false;
    if (code && rd32(code)) {
        tb_HLock(code);
        gaddr base = rd32(code);
        uint32_t entry = rd32(base + 4) == fourcc("dcmp") ? 0 : rd16(base + 2);
        /* Version 8 decompresses in place, as the Resource Manager does it:
         * the compressed data sits at the end of the destination buffer,
         * which has room for the header's expansion bytes. */
        uint32_t data_len = len - hdrlen;
        uint32_t room = outlen + (version == 8 ? d[13] : 0) + 0x100;
        if (room < data_len + 0x100)
            room = data_len + 0x100;
        gaddr dst = tb_NewPtr((int32_t)room, false, true);
        gaddr in = version == 8 ? 0 : tb_NewPtr((int32_t)(len + 0x100), false, true);
        uint32_t worklen = version == 8 ? (len * 256 < 0x1000000 ? len * 256 : 0x1000000) : 0;
        gaddr work = worklen ? tb_NewPtr((int32_t)worklen, false, true) : 0;
        if (dst && (version == 8 ? work != 0 : in != 0)) {
            gaddr src = version == 8 ? dst + room - data_len : in + hdrlen;
            if (version == 8)
                gput(src, d + hdrlen, data_len);
            else
                gput(in, d, len);
            cpu68k *c = g_cpu;
            uint32_t sp = c->a[7];
            if (version == 8) {
                cpu_push32(c, src);
                cpu_push32(c, dst);
                cpu_push32(c, work);
                cpu_push32(c, data_len);
            } else {
                cpu_push32(c, data_len + 0x100);
                cpu_push32(c, src);
                cpu_push32(c, dst);
                cpu_push32(c, in);
            }
            guest_call(base + entry);
            c->a[7] = sp;
            *out = malloc(outlen ? outlen : 1);
            gget(*out, dst, outlen);
            *out_len = outlen;
            ok = !c->halted;
        }
        if (work)
            tb_DisposePtr(work);
        if (dst)
            tb_DisposePtr(dst);
        if (in)
            tb_DisposePtr(in);
        tb_HUnlock(code);
    }
    depth--;
    return ok;
}

static resfile files[MAX_RESFILES];
static int16_t update_file(resfile *f);
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

bool rm_fork_of(int16_t rn, const uint8_t **data, uint32_t *len)
{
    resfile *f = file_for(rn);
    if (!f || !f->fork)
        return false;
    *data = f->fork;
    *len = f->len;
    return true;
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
static gaddr load_ref_direct(gaddr maph, uint32_t r, uint32_t type)
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
        if (!rsrc_decompress(src, len, &dec, &declen, err, sizeof err) &&
            !guest_decompress(src, len, &dec, &declen)) {
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

/* The Resource Manager loads every resource through OS trap A0FC
 * (ResourceStub): A4 = the map handle, A2 and A3 = the reference entry,
 * the handle back in A0.  Installers and code compressors patch it to
 * expand resources as they load, so a patched stub is called. */
void raw_ResourceStub(cpu68k *c, uint16_t tw)
{
    gaddr maph = c->a[4];
    gaddr ref = c->a[2];
    uint32_t type = 0;
    uint32_t r = maph && rd32(maph) && ref > mp_of(maph) ? ref - mp_of(maph) : 0;
    /* The type, for logging: the type entry whose reference list holds r. */
    for (unsigned i = 0; r && i < map_ntypes(maph); i++)
        if (r >= ref_off(maph, i, 0) && r < ref_off(maph, i, 0) + type_count(maph, i) * 12)
            type = type_code(maph, i);
    c->a[0] = r ? load_ref_direct(maph, r, type) : 0;
}

static gaddr load_ref(gaddr maph, uint32_t r, uint32_t type)
{
    cpu68k *c = g_cpu;
    gaddr stub = rd32(OS_TRAP_TABLE + 0xFC * 4);
    if (!c || native_index_for(stub) >= 0)
        return load_ref_direct(maph, r, type);
    gaddr h = rd32(mp_of(maph) + r + 8);
    if (h && rd32(h))
        return h; /* already loaded: the stub isn't consulted */
    uint32_t d[8], a[8];
    memcpy(d, c->d, sizeof d);
    memcpy(a, c->a, sizeof a);
    c->a[4] = maph;
    c->a[3] = c->a[2] = mp_of(maph) + r;
    c->a[0] = stub;
    guest_call_regs(stub);
    gaddr result = c->a[0];
    memcpy(c->d, d, sizeof d);
    memcpy(c->a, a, sizeof a);
    return result;
}

/* ResErrProc: called with the error in D0 whenever ResErr is set to one
 * (not again for errors its own Resource Manager calls make). */
void res_err_proc(int16_t e)
{
    static bool busy;
    gaddr proc = rd32(LM_ResErrProc);
    if (busy || !proc || !g_cpu)
        return;
    busy = true;
    uint32_t d0 = g_cpu->d[0];
    g_cpu->d[0] = (uint32_t)(int32_t)e;
    guest_call(proc);
    g_cpu->d[0] = d0;
    busy = false;
}

/* RomMapInsert: the next call also searches the ROM's resources (here the
 * System file stands in for the ROM, so it always does), loading them as
 * TmpResLoad says; both are cleared after that one call. */
static uint8_t rom_map_begin(void)
{
    uint8_t saved = rd8(LM_ResLoad);
    if (rd8(LM_RomMapInsert))
        wr8(LM_ResLoad, rd8(LM_TmpResLoad));
    return saved;
}

static void rom_map_end(uint8_t saved)
{
    if (!rd8(LM_RomMapInsert))
        return;
    wr8(LM_ResLoad, saved);
    wr8(LM_RomMapInsert, 0);
    wr8(LM_TmpResLoad, 0);
}

static gaddr get_res_in(uint32_t type, int16_t id, bool one);

static gaddr get_res(uint32_t type, int16_t id, bool one)
{
    uint8_t saved = rom_map_begin();
    gaddr h = get_res_in(type, id, one);
    rom_map_end(saved);
    return h;
}

static gaddr get_res_in(uint32_t type, int16_t id, bool one)
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
    if (path)
        fm_fcb_record(f->refnum, path, true, !read_only, len, 0);
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
    update_file(f);
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
    fm_fcb_forget(f->refnum);
    free(f->path);
    free(f->fork);
    memset(f, 0, sizeof *f);
    set_res_err(noErr);
}


/* Open a file's resource fork.  Already open: the Resource Manager returns
 * the existing refnum, except for a read-only open (fsRdPerm), which gets
 * an access path and map of its own (Squark opens itself read-only and
 * closes that again: the application's own map must stay). */
static int16_t open_path_perm(const char *path, int perm)
{
    for (int i = 0; i < MAX_RESFILES && perm != 1; i++)
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

static int16_t open_path(const char *path) { return open_path_perm(path, 0); }

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
    return open_path_perm(path, perm);
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
    return open_path_perm(path, perm);
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

/* ---------------------------------------------------------------------- */
/* Writing
 *
 * Structural changes (adding, removing, renaming) rebuild the guest map in
 * place from a host list of references; the map handle and the resources'
 * handles stay the same.  Writing a file rebuilds the whole fork: changed
 * resources from their handles, the rest from the bytes last read or
 * written.  Files opened read-only (the application, fonts) keep their
 * changes in memory. */

#define MAP_CHANGED 0x20
#define MAP_READ_ONLY 0x80

typedef struct {
    uint32_t type;
    int16_t id;
    uint8_t attrs;
    uint32_t doff;
    gaddr h;
    uint8_t name[256]; /* Pascal string; name[0] == 0xFF means none */
} ref_rec;

typedef struct {
    ref_rec *v;
    size_t n, cap;
} ref_list;

static void refs_push(ref_list *l, const ref_rec *r)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->v = realloc(l->v, l->cap * sizeof *l->v);
    }
    l->v[l->n++] = *r;
}

static void read_refs(gaddr maph, ref_list *out)
{
    memset(out, 0, sizeof *out);
    unsigned nt = map_ntypes(maph);
    for (unsigned i = 0; i < nt; i++) {
        unsigned n = type_count(maph, i);
        for (unsigned j = 0; j < n; j++) {
            gaddr m = mp_of(maph);
            uint32_t r = ref_off(maph, i, j);
            ref_rec rec = { .type = type_code(maph, i),
                            .id = (int16_t)rd16(m + r),
                            .attrs = rd8(m + r + 4),
                            .doff = rd32(m + r + 4) & 0x00FFFFFF,
                            .h = rd32(m + r + 8) };
            uint16_t noff = rd16(m + r + 2);
            if (noff == 0xFFFF)
                rec.name[0] = 0xFF;
            else
                gget(rec.name, m + rd16(m + 26) + noff, (uint32_t)rd8(m + rd16(m + 26) + noff) + 1);
            refs_push(out, &rec);
        }
    }
}

/* Serialize a map: 28-byte header, type list, reference lists, names. */
static uint8_t *build_map(const uint8_t header[16], int16_t refnum, uint16_t attrs, const ref_list *l, uint32_t *len)
{
    uint32_t types[512];
    unsigned per[512] = { 0 }, nt = 0;
    size_t names = 0;
    for (size_t k = 0; k < l->n; k++) {
        unsigned t = 0;
        while (t < nt && types[t] != l->v[k].type)
            t++;
        if (t == nt && nt < 512)
            types[nt++] = l->v[k].type;
        per[t]++;
        if (l->v[k].name[0] != 0xFF)
            names += (size_t)l->v[k].name[0] + 1;
    }
    uint32_t tl = 28, rl = tl + 2 + nt * 8, nl = rl + (uint32_t)l->n * 12;
    *len = nl + (uint32_t)names;
    uint8_t *m = calloc(1, *len);
    memcpy(m, header, 16);
    m[20] = (uint8_t)(refnum >> 8), m[21] = (uint8_t)refnum;
    m[22] = (uint8_t)(attrs >> 8), m[23] = (uint8_t)attrs;
    m[24] = (uint8_t)(tl >> 8), m[25] = (uint8_t)tl;
    m[26] = (uint8_t)(nl >> 8), m[27] = (uint8_t)nl;
    uint16_t ntm1 = (uint16_t)(nt - 1);
    m[tl] = (uint8_t)(ntm1 >> 8), m[tl + 1] = (uint8_t)ntm1;
    uint32_t r = rl, np = 0;
    for (unsigned t = 0; t < nt; t++) {
        uint8_t *te = m + tl + 2 + t * 8;
        te[0] = (uint8_t)(types[t] >> 24), te[1] = (uint8_t)(types[t] >> 16);
        te[2] = (uint8_t)(types[t] >> 8), te[3] = (uint8_t)types[t];
        te[4] = (uint8_t)((per[t] - 1) >> 8), te[5] = (uint8_t)(per[t] - 1);
        te[6] = (uint8_t)((r - tl) >> 8), te[7] = (uint8_t)(r - tl);
        for (size_t k = 0; k < l->n; k++) {
            const ref_rec *e = &l->v[k];
            if (e->type != types[t])
                continue;
            uint8_t *p = m + r;
            p[0] = (uint8_t)(e->id >> 8), p[1] = (uint8_t)e->id;
            if (e->name[0] == 0xFF) {
                p[2] = p[3] = 0xFF;
            } else {
                p[2] = (uint8_t)(np >> 8), p[3] = (uint8_t)np;
                memcpy(m + nl + np, e->name, (size_t)e->name[0] + 1);
                np += (uint32_t)e->name[0] + 1;
            }
            p[4] = e->attrs;
            p[5] = (uint8_t)(e->doff >> 16), p[6] = (uint8_t)(e->doff >> 8), p[7] = (uint8_t)e->doff;
            p[8] = (uint8_t)(e->h >> 24), p[9] = (uint8_t)(e->h >> 16);
            p[10] = (uint8_t)(e->h >> 8), p[11] = (uint8_t)e->h;
            r += 12;
        }
    }
    return m;
}

/* Replace the guest map's contents, keeping its header copy and chain link. */
static bool store_refs(gaddr maph, const ref_list *l, uint16_t attrs)
{
    uint8_t header[16];
    gget(header, mp_of(maph), 16);
    gaddr next = map_next(maph);
    uint32_t len;
    uint8_t *m = build_map(header, map_refnum(maph), attrs, l, &len);
    tb_SetHandleSize(maph, (int32_t)len);
    bool ok = (uint32_t)tb_GetHandleSize(maph) == len;
    if (ok) {
        gput(mp_of(maph), m, len);
        wr32(mp_of(maph) + 16, next);
    }
    free(m);
    return ok;
}

static uint16_t map_attrs(gaddr maph) { return rd16(mp_of(maph) + 22); }
static void mark_map_changed(gaddr maph) { wr16(mp_of(maph) + 22, map_attrs(maph) | MAP_CHANGED); }

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

/* A whole fork from a reference list; data offsets in l are updated. */
static uint8_t *build_fork(resfile *f, ref_list *l, uint16_t attrs, uint32_t *len_out)
{
    size_t cap = 256 + 1024, len = 256;
    uint8_t *out = calloc(1, cap);
    uint32_t old_dstart = f && f->len >= 4 ? rd32(mp_of(f->maph)) : 0;
    for (size_t k = 0; k < l->n; k++) {
        ref_rec *e = &l->v[k];
        const uint8_t *src = NULL;
        uint32_t n = 0;
        uint8_t *tmp = NULL;
        if ((e->attrs & RES_CHANGED) && e->h && rd32(e->h)) {
            n = (uint32_t)tb_GetHandleSize(e->h);
            src = tmp = malloc(n ? n : 1);
            gget(tmp, rd32(e->h), n);
            e->attrs &= (uint8_t)~RES_COMPRESSED;
        } else if (f && f->fork && (uint64_t)old_dstart + e->doff + 4 <= f->len) {
            const uint8_t *p = f->fork + old_dstart + e->doff;
            n = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
            if ((uint64_t)old_dstart + e->doff + 4 + n > f->len)
                n = 0;
            src = p + 4;
        }
        while (len + 4 + n > cap) {
            cap *= 2;
            out = realloc(out, cap);
        }
        e->doff = (uint32_t)len - 256;
        put32(out + len, n);
        if (n)
            memcpy(out + len + 4, src, n);
        len += 4 + n;
        e->attrs &= (uint8_t)~RES_CHANGED;
        free(tmp);
    }
    uint32_t dlen = (uint32_t)len - 256;
    uint8_t header[16];
    put32(header, 256);
    put32(header + 4, (uint32_t)len);
    put32(header + 8, dlen);
    uint32_t mlen;
    uint8_t *m = build_map(header, 0, attrs, l, &mlen);
    put32(header + 12, mlen);
    memcpy(m, header, 16);
    out = realloc(out, len + mlen);
    memcpy(out, header, 16);
    /* On disk the map's handle fields are zero. */
    uint8_t *disk = out + len;
    memcpy(disk, m, mlen);
    uint32_t tl = 28, nt = (uint32_t)((disk[tl] << 8) | disk[tl + 1]) + 1;
    if (l->n == 0)
        nt = 0;
    for (uint32_t k = 0; k < (uint32_t)l->n; k++)
        memset(disk + tl + 2 + nt * 8 + k * 12 + 8, 0, 4);
    free(m);
    *len_out = (uint32_t)len + mlen;
    return out;
}

/* Store a fork in a host file, keeping its data fork and Finder info. */
static bool write_host_fork(const char *path, const uint8_t *fork, uint32_t len)
{
    mac_file mf;
    char err[256];
    if (!appledouble_read(path, &mf, err, sizeof err))
        memset(&mf, 0, sizeof mf);
    free(mf.rsrc);
    mf.rsrc = malloc(len);
    memcpy(mf.rsrc, fork, len);
    mf.rsrc_len = len;
    bool ok = appledouble_write(path, &mf, err, sizeof err);
    if (!ok)
        log_printf("rsrc: %s: %s\n", path, err);
    mac_file_free(&mf);
    return ok;
}

static int16_t update_file(resfile *f)
{
    gaddr maph = f->maph;
    ref_list l;
    read_refs(maph, &l);
    bool changed = map_attrs(maph) & MAP_CHANGED;
    for (size_t k = 0; k < l.n && !changed; k++)
        changed = l.v[k].attrs & RES_CHANGED;
    if (!changed) {
        free(l.v);
        return noErr;
    }
    uint16_t attrs = map_attrs(maph) & (uint16_t)~(MAP_CHANGED | MAP_READ_ONLY);
    uint32_t len;
    uint8_t *fork = build_fork(f, &l, attrs, &len);
    int16_t err = noErr;
    if (f->path && !f->read_only && !write_host_fork(f->path, fork, len))
        err = ioErr;
    free(f->fork);
    f->fork = fork;
    f->len = len;
    fm_fcb_eof(f->refnum, len);
    /* The guest map now describes the new fork. */
    gput(mp_of(maph), fork, 16);
    if (!store_refs(maph, &l, attrs | (f->read_only ? MAP_READ_ONLY : 0)))
        err = memFullErr;
    free(l.v);
    return err;
}

void tb_UpdateResFile(int16_t rn)
{
    resfile *f = file_for(rn ? rn : sys_refnum);
    if (!f) {
        set_res_err(resFNotFound);
        return;
    }
    set_res_err(update_file(f));
}

/* At ExitToShell, as the Resource Manager did when closing every file. */
void rm_update_all(void)
{
    for (int i = 0; i < MAX_RESFILES; i++)
        if (files[i].used && files[i].path && !files[i].read_only)
            update_file(&files[i]);
}

static resfile *editable(gaddr res, gaddr *maph, uint32_t *r)
{
    if (!find_handle(res, maph, r, NULL)) {
        set_res_err(resNotFound);
        return NULL;
    }
    return file_for_map(*maph);
}

void tb_ChangedResource(gaddr res)
{
    gaddr maph;
    uint32_t r;
    if (!editable(res, &maph, &r))
        return;
    gaddr a = mp_of(maph) + r + 4;
    if (rd8(a) & RES_PROTECTED) {
        set_res_err(resAttrErr);
        return;
    }
    wr8(a, rd8(a) | RES_CHANGED);
    mark_map_changed(maph);
    set_res_err(noErr);
}

void tb_WriteResource(gaddr res)
{
    gaddr maph;
    uint32_t r;
    resfile *f = editable(res, &maph, &r);
    if (!f)
        return;
    if (!(rd8(mp_of(maph) + r + 4) & RES_CHANGED)) {
        set_res_err(noErr);
        return;
    }
    set_res_err(update_file(f));
}

void tb_SetResAttrs(gaddr res, int16_t attrs)
{
    gaddr maph;
    uint32_t r;
    if (!editable(res, &maph, &r))
        return;
    wr8(mp_of(maph) + r + 4, (uint8_t)attrs);
    set_res_err(noErr);
}

void tb_SetResInfo(gaddr res, int16_t id, gaddr name)
{
    gaddr maph;
    uint32_t r;
    if (!editable(res, &maph, &r))
        return;
    if (rd8(mp_of(maph) + r + 4) & RES_PROTECTED) {
        set_res_err(resAttrErr);
        return;
    }
    ref_list l;
    read_refs(maph, &l);
    for (size_t k = 0; k < l.n; k++)
        if (l.v[k].h == res) {
            l.v[k].id = id;
            if (name)
                gget(l.v[k].name, name, (uint32_t)rd8(name) + 1);
        }
    set_res_err(store_refs(maph, &l, map_attrs(maph) | MAP_CHANGED) ? noErr : memFullErr);
    free(l.v);
}

void tb_AddResource(gaddr data, int32_t typ, int16_t id, gaddr name)
{
    gaddr maph0;
    uint32_t r0;
    gaddr maph = cur_map();
    if (!data || !maph || find_handle(data, &maph0, &r0, NULL)) {
        set_res_err(addResFailed);
        return;
    }
    ref_list l;
    read_refs(maph, &l);
    ref_rec rec = { .type = (uint32_t)typ, .id = id, .attrs = RES_CHANGED, .h = data };
    rec.name[0] = 0xFF;
    if (name && rd8(name))
        gget(rec.name, name, (uint32_t)rd8(name) + 1);
    /* Keep a type's references together: insert after the last of its type. */
    size_t at = l.n;
    for (size_t k = 0; k < l.n; k++)
        if (l.v[k].type == (uint32_t)typ)
            at = k + 1;
    refs_push(&l, &rec);
    memmove(&l.v[at + 1], &l.v[at], (l.n - 1 - at) * sizeof *l.v);
    l.v[at] = rec;
    if (store_refs(maph, &l, map_attrs(maph) | MAP_CHANGED)) {
        tb_HSetRBit(data);
        set_res_err(noErr);
    } else {
        set_res_err(addResFailed);
    }
    free(l.v);
}

void tb_RemoveResource(gaddr res)
{
    gaddr maph;
    uint32_t r;
    if (!editable(res, &maph, &r))
        return;
    if (rd8(mp_of(maph) + r + 4) & RES_PROTECTED) {
        set_res_err(rmvResFailed);
        return;
    }
    ref_list l;
    read_refs(maph, &l);
    size_t w = 0;
    for (size_t k = 0; k < l.n; k++)
        if (l.v[k].h != res)
            l.v[w++] = l.v[k];
    l.n = w;
    store_refs(maph, &l, map_attrs(maph) | MAP_CHANGED);
    free(l.v);
    tb_HClrRBit(res);
    set_res_err(noErr);
}

void tb_ReadPartialResource(gaddr res, int32_t offset, gaddr buffer, int32_t count)
{
    tb_LoadResource(res);
    if (rd16(LM_ResErr))
        return;
    if (offset < 0 || count < 0 || offset + count > tb_GetHandleSize(res)) {
        set_res_err(-39 /* eofErr */);
        return;
    }
    gmemcpy(buffer, rd32(res) + (uint32_t)offset, (uint32_t)count);
}

void tb_WritePartialResource(gaddr res, int32_t offset, gaddr buffer, int32_t count)
{
    tb_LoadResource(res);
    if (rd16(LM_ResErr))
        return;
    if (offset < 0 || count < 0) {
        set_res_err(paramErr);
        return;
    }
    if (offset + count > tb_GetHandleSize(res))
        tb_SetHandleSize(res, offset + count);
    gmemcpy(rd32(res) + (uint32_t)offset, buffer, (uint32_t)count);
    tb_ChangedResource(res);
    if (!rd16(LM_ResErr))
        tb_WriteResource(res);
}

void tb_SetResourceSize(gaddr res, int32_t size)
{
    tb_LoadResource(res);
    if (rd16(LM_ResErr))
        return;
    tb_SetHandleSize(res, size);
    tb_ChangedResource(res);
}

/* Creating: an empty resource fork for a new or fork-less file. */
static void create_res_file(const char *path, uint32_t type, uint32_t creator, bool set_info)
{
    mac_file mf;
    char err[256];
    bool exists = appledouble_read(path, &mf, err, sizeof err);
    if (exists && mf.rsrc_len) {
        mac_file_free(&mf);
        set_res_err(-48 /* dupFNErr */);
        return;
    }
    if (!exists) {
        memset(&mf, 0, sizeof mf);
        mf.data = malloc(1);
        put32(mf.finder_info, set_info ? type : 0x3F3F3F3F);
        put32(mf.finder_info + 4, set_info ? creator : 0x3F3F3F3F);
        mf.has_finder_info = true;
    }
    ref_list l = { 0 };
    uint32_t len;
    uint8_t *fork = build_fork(NULL, &l, 0, &len);
    free(mf.rsrc);
    mf.rsrc = fork;
    mf.rsrc_len = len;
    bool ok = appledouble_write(path, &mf, err, sizeof err);
    mac_file_free(&mf);
    set_res_err(ok ? noErr : ioErr);
}

void tb_HCreateResFile(int16_t vrefnum, int32_t parid, gaddr name)
{
    char path[1024];
    if (!fm_host_path(vrefnum, parid, name, true, path, sizeof path)) {
        set_res_err(-120 /* dirNFErr */);
        return;
    }
    create_res_file(path, 0, 0, false);
}

void tb_CreateResFile(gaddr fn) { tb_HCreateResFile(0, 0, fn); }

void tb_FSpCreateResFile(gaddr spec, int32_t creator, int32_t file_type, int16_t script)
{
    char path[1024];
    if (!fm_host_path((int16_t)rd16(spec), (int32_t)rd32(spec + 2), spec + 6, true, path, sizeof path)) {
        set_res_err(-120 /* dirNFErr */);
        return;
    }
    create_res_file(path, (uint32_t)file_type, (uint32_t)creator, true);
}

/* Override maps aren't used here, so scanning them has no effect. */
static bool scan_override;
void tb_SetScanOverride(int8_t scan) { scan_override = scan != 0; }
int8_t tb_GetScanOverride(void) { return scan_override; }
