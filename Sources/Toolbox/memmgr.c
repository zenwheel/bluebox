/*
 * Memory Manager: classic heap zones in the guest arena (32-bit format).
 *
 * A zone is a 52-byte header followed by contiguous blocks up to bkLim,
 * where a 12-byte free "trailer" block marks the end.  Each block has a
 * 12-byte header:
 *
 *   +0  flags: block use in bits 7-6 (0 free, 1 nonrelocatable, 2 relocatable)
 *   +1  handle state: lock 0x80, purge 0x40, resource 0x20
 *   +2  reserved
 *   +3  size correction (unused bytes at the end)
 *   +4  physical size, header included
 *   +8  relocatable: master pointer offset from the zone; nonrelocatable: zone
 *
 * The application zone can grow on demand up to its limit, so there is no
 * fixed partition: memory is committed by the host only when touched.
 */
#include <stdlib.h>
#include <stdio.h>

#include "tb_internal.h"

enum { USE_FREE = 0, USE_NREL = 1, USE_REL = 2 };
enum { ST_LOCK = 0x80, ST_PURGE = 0x40, ST_RSRC = 0x20 };
enum { HDR = 12, MIN_BLOCK = 16 };

/* What the Memory Manager reports at most (free space, Gestalt's RAM):
 * 128 MB, about what the Quadra 800 we answer as could hold (136 MB).
 * Programs add these up in signed longs (SpookHouse adds its own pool to
 * PurgeSpace's contiguous space and a gigabyte overflowed), and the Star
 * Trek demo sized its cache from 256 MB to nothing. */
uint32_t g_mem_report_cap = 1u << 27;
static gaddr appl_max_limit; /* 0 = application zone does not grow */

/* ---------------------------------------------------------------------- */
/* Block helpers */

static inline unsigned b_use(gaddr b) { return rd8(b) >> 6; }
static inline uint8_t b_state(gaddr b) { return rd8(b + 1); }
static inline uint8_t b_sizec(gaddr b) { return rd8(b + 3); }
static inline uint32_t b_psize(gaddr b) { return rd32(b + 4); }
static inline uint32_t b_loc(gaddr b) { return rd32(b + 8); }
static inline uint32_t b_lsize(gaddr b) { return b_psize(b) - HDR - b_sizec(b); }
/* The block after b; a block too small to have a header is damage (a
 * program writing over it), and ends the walk rather than standing still
 * on it for good (Squark's heap, and MaxBlock never returned). */
static inline gaddr b_next(gaddr b) { return b_psize(b) < HDR ? 0xFFFFFFF0u : b + b_psize(b); }
static inline gaddr b_data(gaddr b) { return b + HDR; }

static void b_set(gaddr b, unsigned use, uint8_t state, uint8_t sizec, uint32_t psize, uint32_t loc)
{
    wr8(b, (uint8_t)(use << 6));
    wr8(b + 1, state);
    wr8(b + 2, 0);
    wr8(b + 3, sizec);
    wr32(b + 4, psize);
    wr32(b + 8, loc);
}

static inline gaddr z_first(gaddr z) { return z + Zone_heapData_off; }

/* Programs from the 64K ROM's days grow the application zone themselves
 * (MaxApplZone's glue in Sword of Ages): they move bkLim and HeapEnd up
 * to ApplLimit, turn the old trailer into a free block reaching the new
 * one and write the new trailer, all with 24-bit headers (size in the
 * first long's low three bytes).  When the trailer is such a header, find
 * the old trailer the same way and rewrite both in the 32-bit format. */
static void adopt_hand_growth(gaddr z, gaddr lim)
{
    for (gaddr b = z_first(z); b < lim; b += b_psize(b)) {
        uint32_t ps = b_psize(b);
        if (b_use(b) == USE_FREE && (rd32(b) & 0x00FFFFFFu) == lim - b && b + ps != lim) {
            b_set(b, USE_FREE, 0, 0, lim - b, 0);
            b_set(lim, USE_FREE, 0, 0, HDR, 0);
            return;
        }
        if (ps < HDR || (ps & 3) || b + ps > lim)
            return;
    }
}

static inline gaddr z_lim(gaddr z)
{
    gaddr lim = Zone_bkLim(z);
    if (__builtin_expect(rd32(lim) == HDR, 0) && rd32(lim + 4) != HDR && lim < 0x01000000u && lim > z)
        adopt_hand_growth(z, lim);
    return lim;
}

static inline gaddr cur_zone(void) { return rd32(LM_TheZone); }
static inline gaddr sys_zone(void) { return rd32(LM_SysZone); }
static inline gaddr appl_zone(void) { return rd32(LM_ApplZone); }
static inline gaddr pick_zone(bool sys) { return sys ? sys_zone() : cur_zone(); }

static uint32_t cap(uint32_t v) { return v > g_mem_report_cap ? g_mem_report_cap : v; }

/* Physical size for a logical size: header + data, 4-byte aligned, minimum 16. */
static uint32_t phys_for(uint32_t logical)
{
    uint64_t p = ((uint64_t)logical + HDR + 3) & ~(uint64_t)3;
    if (p < MIN_BLOCK)
        p = MIN_BLOCK;
    return p > 0xFFFFFFF0u ? 0 : (uint32_t)p;
}

/* The zone's allocPtr is the rover: allocation searches from it (next
 * fit), as the Memory Manager did, rather than from the zone's start.  It
 * must always be a block start, so whatever merges blocks moves it back. */
static void rover_merged(gaddr z, gaddr b, uint32_t ps)
{
    gaddr r = Zone_allocPtr(z);
    if (r > b && r < b + ps)
        Zone_set_allocPtr(z, b);
}

/* Merge b with any free blocks that follow it (b must be free). */
static void coalesce(gaddr z, gaddr b)
{
    gaddr lim = z_lim(z);
    uint32_t ps = b_psize(b);
    gaddr n = b + ps;
    while (n < lim && b_use(n) == USE_FREE && b_psize(n) >= HDR) { /* a smashed header ends it */
        ps += b_psize(n);
        n = b + ps;
    }
    wr32(b + 4, ps);
    rover_merged(z, b, ps);
}

/* Zones a program made with InitZone, usually inside a block of the
 * application zone (NCSA Mosaic gives each text field one).  An address in
 * such a zone belongs to it, not to the zone around it.  An entry lasts
 * while the zone's header keeps the limit it was made with and the block
 * holding it isn't disposed of. */
enum { MAX_SUBZONES = 64 };
static struct { gaddr z, lim; } subzones[MAX_SUBZONES];
static int nsubzones;

static void forget_subzones(gaddr from, gaddr to)
{
    int k = 0;
    for (int i = 0; i < nsubzones; i++)
        if (subzones[i].z >= to || subzones[i].lim + HDR <= from)
            subzones[k++] = subzones[i];
    nsubzones = k;
}

static bool in_zone(gaddr z, gaddr addr) { return z && addr >= z && addr < z_lim(z) + HDR; }

/* Which zone owns this address?  The innermost one that holds it. */
static gaddr zone_of(gaddr addr)
{
    gaddr best = 0;
    for (int i = 0; i < nsubzones; i++)
        if (z_lim(subzones[i].z) == subzones[i].lim && in_zone(subzones[i].z, addr) && subzones[i].z > best)
            best = subzones[i].z;
    if (best)
        return best;
    gaddr s = sys_zone(), a = appl_zone(), t = cur_zone();
    if (in_zone(t, addr) && t != s && t != a && (in_zone(s, t) || in_zone(a, t)))
        return t; /* the current zone, nested in another */
    if (in_zone(s, addr))
        return s;
    if (in_zone(a, addr))
        return a;
    if (in_zone(t, addr))
        return t;
    return 0;
}

/* Validate a handle; returns its block, or 0 (empty) with *err set. */
static gaddr handle_block(gaddr h, int16_t *err)
{
    *err = noErr;
    if (h == 0) {
        *err = nilHandleErr;
        return 0;
    }
    gaddr p = rd32(h);
    if (p == 0) {
        *err = nilHandleErr;
        return 0;
    }
    gaddr b = p - HDR;
    if (b_use(b) != USE_REL) {
        *err = memWZErr;
        return 0;
    }
    return b;
}

/* ---------------------------------------------------------------------- */
/* Zone setup and growth */

static void write_trailer(gaddr lim)
{
    b_set(lim, USE_FREE, 0, 0, HDR, 0);
}

static void link_masters(gaddr z, gaddr first, unsigned n)
{
    /* Thread new master pointers onto the zone's free list. */
    gaddr head = Zone_hFstFree(z);
    for (unsigned i = n; i-- > 0;) {
        wr32(first + i * 4, head);
        head = first + i * 4;
    }
    Zone_set_hFstFree(z, head);
}

void mm_init_zone(gaddr z, gaddr limit, uint16_t more_masters)
{
    limit &= ~3u;
    gmemset(z, 0, Zone_SIZEOF);
    gaddr lim = limit - HDR;
    forget_subzones(z, limit);
    Zone_set_bkLim(z, lim);
    Zone_set_moreMast(z, more_masters ? more_masters : 32);
    gaddr first = z_first(z);
    b_set(first, USE_FREE, 0, 0, lim - first, 0);
    write_trailer(lim);
    Zone_set_zcbFree(z, (int32_t)(lim - first));
    Zone_set_allocPtr(z, first);
    Zone_set_purgePtr(z, first);
}

void mm_set_appl_growth(gaddr max_limit) { appl_max_limit = max_limit; }

/* Grow the application zone so that at least `need` more bytes exist at the top. */
static bool grow_zone(gaddr z, uint32_t need)
{
    if (z != appl_zone() || !appl_max_limit)
        return false;
    gaddr lim = z_lim(z);
    uint64_t want = (uint64_t)need + (1u << 20);
    want = (want + 0xFFFF) & ~(uint64_t)0xFFFF;
    gaddr limit = rd32(LM_ApplLimit);
    if (limit > appl_max_limit || limit == 0)
        limit = appl_max_limit;
    /* Blocks are whole longs: a limit that isn't (one set from the stack's
     * place, as below 16 MB for programs not 32-bit clean) would leave the
     * zone's last block an odd size. */
    limit &= ~3u;
    if (lim + HDR >= limit)
        return false;
    uint64_t room = limit - HDR - lim;
    if (room < need)
        return false;
    if (want > room)
        want = room;
    gaddr newlim = lim + (uint32_t)want;
    /* The old trailer becomes a free block covering the new space. */
    b_set(lim, USE_FREE, 0, 0, newlim - lim, 0);
    write_trailer(newlim);
    Zone_set_bkLim(z, newlim);
    wr32(LM_HeapEnd, newlim);
    LOGF(LOG_MEM, "mm: application zone grown to %08X\n", newlim);
    return true;
}

/* ---------------------------------------------------------------------- */
/* Allocation */

static gaddr find_free(gaddr z, uint32_t psize)
{
    gaddr lim = z_lim(z), first = z_first(z);
    gaddr start = Zone_allocPtr(z);
    if (start < first || start >= lim)
        start = first;
    /* From the rover to the end, then from the start up to the rover. */
    for (int pass = 0; pass < 2; pass++) {
        gaddr end = pass ? Zone_allocPtr(z) : lim;
        if (pass && (end < first || end >= lim))
            end = lim;
        for (gaddr b = pass ? first : start; b < end; b = b_next(b)) {
            if (b_psize(b) < HDR) { /* a smashed header: no way past it */
                LOGF(LOG_MEM, "mm: zone %08X damaged at block %08X\n", z, b);
                return 0;
            }
            if (b_use(b) != USE_FREE)
                continue;
            coalesce(z, b);
            if (b_psize(b) >= psize) {
                Zone_set_allocPtr(z, b);
                return b;
            }
        }
        if (start == first)
            break;
    }
    return 0;
}

/* Slide unlocked relocatable blocks down over free space. Returns the
 * largest free block afterwards. */
static uint32_t compact(gaddr z)
{
    Zone_set_allocPtr(z, 0); /* blocks move */
    gaddr lim = z_lim(z);
    gaddr dst = 0; /* start of the free run being filled, or 0 */
    uint32_t largest = 0;
    for (gaddr b = z_first(z); b < lim;) {
        uint32_t ps = b_psize(b);
        unsigned use = b_use(b);
        if (ps < HDR) { /* a damaged header: no way past it */
            LOGF(LOG_MEM, "mm: zone %08X damaged at block %08X\n", z, b);
            dst = 0;
            break;
        }
        if (use == USE_FREE) {
            if (!dst)
                dst = b;
            b += ps;
            continue;
        }
        if (dst && use == USE_REL && !(b_state(b) & ST_LOCK)) {
            gmemcpy(dst, b, ps);
            wr32(z + b_loc(dst), dst + HDR); /* update the master pointer */
            dst += ps;
            gaddr next = b + ps;
            if (next > dst) /* mark the gap free so the walk stays valid */
                b_set(dst, USE_FREE, 0, 0, next - dst, 0);
            b = next;
            continue;
        }
        if (dst) {
            uint32_t gap = b - dst;
            if (gap) {
                b_set(dst, USE_FREE, 0, 0, gap, 0);
                if (gap > largest)
                    largest = gap;
            }
            dst = 0;
        }
        b += ps;
    }
    if (dst && lim > dst) {
        b_set(dst, USE_FREE, 0, 0, lim - dst, 0);
        if (lim - dst > largest)
            largest = lim - dst;
    }
    return largest;
}

/* Purge unlocked purgeable blocks. */
static void purge(gaddr z)
{
    gaddr lim = z_lim(z);
    for (gaddr b = z_first(z); b < lim; b = b_next(b)) {
        if (b_use(b) == USE_REL && (b_state(b) & (ST_PURGE | ST_LOCK)) == ST_PURGE) {
            gaddr h = z + b_loc(b);
            wr32(h, 0);
            b_set(b, USE_FREE, 0, 0, b_psize(b), 0);
            LOGF(LOG_MEM, "mm: purged handle %08X\n", h);
        }
    }
}

/* Find (making room if needed) a free block of at least psize bytes. */
static gaddr make_room(gaddr z, uint32_t psize)
{
    gaddr b = find_free(z, psize);
    if (b)
        return b;
    compact(z);
    if ((b = find_free(z, psize)))
        return b;
    purge(z);
    compact(z);
    if ((b = find_free(z, psize)))
        return b;
    if (grow_zone(z, psize)) {
        compact(z);
        return find_free(z, psize);
    }
    return 0;
}

/* Claim the front of free block b for a block of psize bytes. */
static void carve(gaddr b, uint32_t psize, unsigned use, uint8_t state, uint32_t logical, uint32_t loc)
{
    uint32_t have = b_psize(b);
    if (have - psize >= MIN_BLOCK) {
        b_set(b + psize, USE_FREE, 0, 0, have - psize, 0);
    } else {
        psize = have;
    }
    uint32_t sizec = psize - HDR - logical;
    if (sizec > 255) { /* oversized tail: split it off after all */
        uint32_t keep = phys_for(logical);
        b_set(b + keep, USE_FREE, 0, 0, psize - keep, 0);
        psize = keep;
        sizec = psize - HDR - logical;
    }
    b_set(b, use, state, (uint8_t)sizec, psize, loc);
}

static gaddr alloc_master(gaddr z)
{
    gaddr mp = Zone_hFstFree(z);
    if (!mp) {
        unsigned n = Zone_moreMast(z);
        if (!n)
            n = 32;
        gaddr p = mm_new_ptr_in(z, n * 4, true);
        if (!p)
            return 0;
        link_masters(z, p, n);
        mp = Zone_hFstFree(z);
    }
    Zone_set_hFstFree(z, rd32(mp));
    wr32(mp, 0);
    return mp;
}

static void free_master(gaddr z, gaddr mp)
{
    wr32(mp, Zone_hFstFree(z));
    Zone_set_hFstFree(z, mp);
}

gaddr mm_new_ptr_in(gaddr z, uint32_t size, bool clear)
{
    uint32_t ps = phys_for(size);
    gaddr b = ps ? make_room(z, ps) : 0;
    if (!b) {
        set_mem_err(memFullErr);
        return 0;
    }
    carve(b, ps, USE_NREL, 0, size, z);
    if (clear)
        gmemset(b_data(b), 0, size);
    set_mem_err(noErr);
    return b_data(b);
}

/* Allocate a relocatable block for master pointer mp. */
static gaddr alloc_rel(gaddr z, gaddr mp, uint32_t size, bool clear)
{
    uint32_t ps = phys_for(size);
    gaddr b = ps ? make_room(z, ps) : 0;
    if (!b)
        return 0;
    carve(b, ps, USE_REL, 0, size, mp - z);
    if (clear)
        gmemset(b_data(b), 0, size);
    wr32(mp, b_data(b));
    return b;
}

gaddr mm_new_handle_in(gaddr z, uint32_t size, bool clear)
{
    gaddr mp = alloc_master(z);
    if (!mp) {
        set_mem_err(memFullErr);
        return 0;
    }
    if (!alloc_rel(z, mp, size, clear)) {
        free_master(z, mp);
        set_mem_err(memFullErr);
        return 0;
    }
    set_mem_err(noErr);
    return mp;
}

/* ---------------------------------------------------------------------- */
/* Pointers */

gaddr tb_NewPtr(int32_t size, bool sys_p, bool clear_p)
{
    if (size < 0) {
        set_mem_err(memFullErr);
        return 0;
    }
    gaddr p = mm_new_ptr_in(pick_zone(sys_p), (uint32_t)size, clear_p);
    LOGF(LOG_MEM, "NewPtr(%d) -> %08X\n", size, p);
    return p;
}

void tb_DisposePtr(gaddr p)
{
    if (!p) {
        set_mem_err(noErr);
        return;
    }
    gaddr b = p - HDR;
    if (b_use(b) != USE_NREL) {
        set_mem_err(memWZErr);
        return;
    }
    forget_subzones(b, b + b_psize(b));
    b_set(b, USE_FREE, 0, 0, b_psize(b), 0);
    set_mem_err(noErr);
}

int32_t tb_GetPtrSize(gaddr p)
{
    gaddr b = p - HDR;
    if (!p || b_use(b) != USE_NREL) {
        set_mem_err(memWZErr);
        return memWZErr;
    }
    set_mem_err(noErr);
    return (int32_t)b_lsize(b);
}

/* Resize block b in place if the following free space allows. */
static bool resize_in_place(gaddr z, gaddr b, uint32_t newsize)
{
    uint32_t ps = b_psize(b);
    uint32_t need = phys_for(newsize);
    if (!need)
        return false;
    if (need > ps) {
        gaddr n = b + ps;
        if (n >= z_lim(z) || b_use(n) != USE_FREE)
            return false;
        coalesce(z, n);
        if (ps + b_psize(n) < need)
            return false;
        uint32_t total = ps + b_psize(n);
        wr32(b + 4, total);
        rover_merged(z, b, total);
        ps = total;
    }
    /* Shrink to fit, returning any tail of MIN_BLOCK or more. */
    if (ps - need >= MIN_BLOCK) {
        b_set(b + need, USE_FREE, 0, 0, ps - need, 0);
        coalesce(z, b + need);
        ps = need;
        wr32(b + 4, ps);
    }
    uint32_t sizec = ps - HDR - newsize;
    if (sizec > 255)
        return false;
    wr8(b + 3, (uint8_t)sizec);
    return true;
}

/* A free block of at least psize bytes outside [from, to). */
static gaddr find_free_outside(gaddr z, uint32_t psize, gaddr from, gaddr to)
{
    gaddr lim = z_lim(z);
    for (gaddr f = z_first(z); f < lim; f = b_next(f)) {
        if (b_psize(f) < HDR)
            return 0;
        if (b_use(f) != USE_FREE || (f >= from && f < to))
            continue;
        coalesce(z, f);
        if (b_psize(f) >= psize)
            return f;
    }
    return 0;
}

/* Room for block b to grow in place to need bytes: the unlocked
 * relocatable blocks after it moved elsewhere in the zone, as the Memory
 * Manager moved them for a locked handle or a pointer that grew (Sirtet
 * Info locks its TextEdit text, deletes it and inserts the next page). */
static bool clear_after(gaddr z, gaddr b, uint32_t need)
{
    gaddr lim = z_lim(z), end = b + need;
    if (!need || end < b)
        return false;
    if (end > lim && !(grow_zone(z, end - lim) && end <= (lim = z_lim(z))))
        return false;
    for (gaddr n = b + b_psize(b); n < end; n = b_next(n)) {
        if (n >= lim || b_psize(n) < HDR)
            return false;
        if (b_use(n) != USE_FREE && (b_use(n) != USE_REL || (b_state(n) & ST_LOCK)))
            return false; /* a pointer or a locked handle in the way */
    }
    for (gaddr n = b + b_psize(b); n < end;) {
        uint32_t ps = b_psize(n);
        if (b_use(n) == USE_REL) {
            gaddr f = find_free_outside(z, ps, b, end);
            if (!f && grow_zone(z, ps)) /* the application zone grows past the end */
                f = find_free_outside(z, ps, b, end);
            if (!f)
                return false; /* what moved stays moved: the heap is whole */
            carve(f, ps, USE_REL, b_state(n), b_lsize(n), b_loc(n));
            gmemcpy(b_data(f), b_data(n), b_lsize(n));
            wr32(z + b_loc(n), b_data(f)); /* the master pointer follows */
            b_set(n, USE_FREE, 0, 0, ps, 0);
        }
        n += ps;
    }
    return true;
}

void tb_SetPtrSize(gaddr p, int32_t newsize)
{
    gaddr b = p - HDR;
    if (!p || b_use(b) != USE_NREL || newsize < 0) {
        set_mem_err(newsize < 0 ? memFullErr : memWZErr);
        return;
    }
    gaddr z = b_loc(b);
    if (!resize_in_place(z, b, (uint32_t)newsize) &&
        !(clear_after(z, b, phys_for((uint32_t)newsize)) && resize_in_place(z, b, (uint32_t)newsize))) {
        set_mem_err(memFullErr);
        return;
    }
    set_mem_err(noErr);
}

gaddr tb_PtrZone(gaddr p)
{
    gaddr b = p - HDR;
    if (!p || b_use(b) != USE_NREL) {
        set_mem_err(memWZErr);
        return 0;
    }
    set_mem_err(noErr);
    return b_loc(b);
}

/* ---------------------------------------------------------------------- */
/* Handles */

gaddr tb_NewHandle(int32_t size, bool sys_p, bool clear_p)
{
    if (size < 0) {
        set_mem_err(memFullErr);
        return 0;
    }
    gaddr h = mm_new_handle_in(pick_zone(sys_p), (uint32_t)size, clear_p);
    LOGF(LOG_MEM, "NewHandle(%d) -> %08X\n", size, h);
    return h;
}

gaddr tb_NewEmptyHandle(bool sys_p)
{
    gaddr mp = alloc_master(pick_zone(sys_p));
    set_mem_err(mp ? noErr : memFullErr);
    return mp;
}

void tb_DisposeHandle(gaddr h)
{
    if (!h) {
        set_mem_err(nilHandleErr);
        return;
    }
    LOGF(LOG_MEM, "DisposeHandle(%08X)\n", h);
    gaddr p = rd32(h);
    gaddr z = 0;
    if (p) {
        gaddr b = p - HDR;
        if (b_use(b) != USE_REL) {
            set_mem_err(memWZErr);
            return;
        }
        z = zone_of(b);
        b_set(b, USE_FREE, 0, 0, b_psize(b), 0);
    }
    if (!z)
        z = zone_of(h);
    if (z)
        free_master(z, h);
    set_mem_err(noErr);
}

int32_t tb_GetHandleSize(gaddr h)
{
    int16_t err;
    gaddr b = handle_block(h, &err);
    if (!b) {
        /* Empty (purged) handles report nilHandleErr, as on System 7.1. */
        set_mem_err(err);
        return err;
    }
    set_mem_err(noErr);
    return (int32_t)b_lsize(b);
}

void tb_SetHandleSize(gaddr h, int32_t newsize)
{
    int16_t err;
    gaddr b = handle_block(h, &err);
    if (!b || newsize < 0) {
        set_mem_err(b ? memFullErr : err);
        return;
    }
    gaddr z = zone_of(b);
    if (resize_in_place(z, b, (uint32_t)newsize)) {
        set_mem_err(noErr);
        return;
    }
    if (b_state(b) & ST_LOCK) {
        bool grew = clear_after(z, b, phys_for((uint32_t)newsize)) && resize_in_place(z, b, (uint32_t)newsize);
        if (!grew)
            LOGF(LOG_MEM, "SetHandleSize(%08X, %d): locked, no room after it\n", h, newsize);
        set_mem_err(grew ? noErr : memFullErr);
        return;
    }
    /* Move the block: lock it while room is made so it stays put, then copy. */
    uint8_t state = b_state(b);
    uint32_t old = b_lsize(b);
    wr8(b + 1, state | ST_LOCK);
    uint32_t ps = phys_for((uint32_t)newsize);
    gaddr nb = ps ? make_room(z, ps) : 0;
    b = rd32(h) - HDR;
    if (!nb) {
        wr8(b + 1, state);
        set_mem_err(memFullErr);
        return;
    }
    carve(nb, ps, USE_REL, state, (uint32_t)newsize, h - z);
    gmemcpy(b_data(nb), b_data(b), old < (uint32_t)newsize ? old : (uint32_t)newsize);
    b_set(b, USE_FREE, 0, 0, b_psize(b), 0);
    wr32(h, b_data(nb));
    set_mem_err(noErr);
}

gaddr tb_HandleZone(gaddr h)
{
    if (!h) {
        set_mem_err(nilHandleErr);
        return 0;
    }
    gaddr p = rd32(h);
    gaddr z = zone_of(p ? p : h);
    set_mem_err(z ? noErr : memWZErr);
    return z;
}

gaddr tb_RecoverHandle(gaddr p, bool sys_p)
{
    gaddr b = p - HDR;
    if (!p || b_use(b) != USE_REL) {
        set_mem_err(memWZErr);
        return 0;
    }
    gaddr z = zone_of(b);
    if (!z)
        z = pick_zone(sys_p);
    set_mem_err(noErr);
    return z + b_loc(b);
}

void tb_ReallocateHandle(gaddr h, int32_t size)
{
    if (!h || size < 0) {
        set_mem_err(h ? memFullErr : nilHandleErr);
        return;
    }
    gaddr p = rd32(h);
    gaddr z = zone_of(p ? p : h);
    if (!z)
        z = cur_zone();
    uint8_t state = 0;
    if (p) {
        gaddr b = p - HDR;
        if (b_state(b) & ST_LOCK) {
            set_mem_err(memLockedErr);
            return;
        }
        state = b_state(b) & ST_RSRC;
        b_set(b, USE_FREE, 0, 0, b_psize(b), 0);
        wr32(h, 0);
    }
    gaddr b = alloc_rel(z, h, (uint32_t)size, false);
    if (!b) {
        set_mem_err(memFullErr);
        return;
    }
    wr8(b + 1, state);
    set_mem_err(noErr);
}

void tb_EmptyHandle(gaddr h)
{
    int16_t err;
    gaddr b = handle_block(h, &err);
    if (!b) {
        set_mem_err(h && rd32(h) == 0 ? noErr : err);
        return;
    }
    if (b_state(b) & ST_LOCK) {
        set_mem_err(memPurErr);
        return;
    }
    b_set(b, USE_FREE, 0, 0, b_psize(b), 0);
    wr32(h, 0);
    set_mem_err(noErr);
}

static void set_state_bits(gaddr h, uint8_t set, uint8_t clear)
{
    int16_t err;
    gaddr b = handle_block(h, &err);
    if (!b) {
        set_mem_err(err);
        return;
    }
    wr8(b + 1, (uint8_t)((b_state(b) | set) & ~clear));
    set_mem_err(noErr);
}

void tb_HLock(gaddr h) { set_state_bits(h, ST_LOCK, 0); }
void tb_HUnlock(gaddr h) { set_state_bits(h, 0, ST_LOCK); }
void tb_HPurge(gaddr h) { set_state_bits(h, ST_PURGE, 0); }
void tb_HNoPurge(gaddr h) { set_state_bits(h, 0, ST_PURGE); }
void tb_HSetRBit(gaddr h) { set_state_bits(h, ST_RSRC, 0); }
void tb_HClrRBit(gaddr h) { set_state_bits(h, 0, ST_RSRC); }
void tb_MoveHHi(gaddr h) { set_mem_err(noErr); }

int8_t tb_HGetState(gaddr h)
{
    int16_t err;
    gaddr b = handle_block(h, &err);
    if (!b) {
        set_mem_err(err);
        return 0;
    }
    set_mem_err(noErr);
    return (int8_t)b_state(b);
}

void tb_HSetState(gaddr h, int8_t flags)
{
    int16_t err;
    gaddr b = handle_block(h, &err);
    if (!b) {
        set_mem_err(err);
        return;
    }
    wr8(b + 1, (uint8_t)flags & (ST_LOCK | ST_PURGE | ST_RSRC));
    set_mem_err(noErr);
}

/* MoreMasterPointers(count): a block of count master pointers at once. */
void tb_MoreMasterPointers(uint32_t count)
{
    gaddr z = cur_zone();
    gaddr p = count ? mm_new_ptr_in(z, count * 4, true) : 0;
    if (p)
        link_masters(z, p, count);
}

void tb_MoreMasters(void)
{
    gaddr z = cur_zone();
    tb_MoreMasterPointers(Zone_moreMast(z) ? Zone_moreMast(z) : 32);
}

/* ---------------------------------------------------------------------- */
/* Zones and free space */

gaddr tb_GetZone(void)
{
    set_mem_err(noErr);
    return cur_zone();
}

void tb_SetZone(gaddr hz)
{
    wr32(LM_TheZone, hz);
    set_mem_err(noErr);
}

void tb_SetGrowZone(gaddr newgz)
{
    Zone_set_gzProc(cur_zone(), newgz);
    set_mem_err(noErr);
}

static uint32_t total_free(gaddr z)
{
    uint32_t sum = 0;
    gaddr lim = z_lim(z);
    for (gaddr b = z_first(z); b < lim; b = b_next(b))
        if (b_use(b) == USE_FREE)
            sum += b_psize(b);
    return sum;
}

/* Bytes the application zone could still grow by. */
static uint32_t growth_room(gaddr z)
{
    if (z != appl_zone() || !appl_max_limit)
        return 0;
    gaddr limit = rd32(LM_ApplLimit);
    if (limit > appl_max_limit || !limit)
        limit = appl_max_limit;
    limit &= ~3u; /* blocks are whole longs, as grow_zone makes them */
    return limit > z_lim(z) + HDR ? limit - HDR - z_lim(z) : 0;
}

/* Largest block obtainable by compaction (and purging, if purge is set),
 * without changing anything; includes room the zone could still grow. */
static uint32_t largest_possible(gaddr z, bool with_purge)
{
    uint32_t best = 0, run = 0;
    gaddr lim = z_lim(z);
    for (gaddr b = z_first(z); b < lim; b = b_next(b)) {
        unsigned use = b_use(b);
        uint8_t st = b_state(b);
        bool movable = use == USE_FREE || (use == USE_REL && !(st & ST_LOCK));
        if (!movable) {
            if (run > best)
                best = run;
            run = 0;
            continue;
        }
        if (use == USE_FREE || (with_purge && (st & ST_PURGE)))
            run += b_psize(b);
    }
    run += growth_room(z);
    if (run > best)
        best = run;
    return best > HDR ? best - HDR : 0;
}

int32_t tb_FreeMem(bool sys_p)
{
    gaddr z = pick_zone(sys_p);
    set_mem_err(noErr);
    return (int32_t)cap(total_free(z) + growth_room(z));
}

int32_t tb_MaxBlock(bool sys_p)
{
    set_mem_err(noErr);
    return (int32_t)cap(largest_possible(pick_zone(sys_p), false));
}

int32_t tb_MaxMem(uint32_t *growp, bool sys_p)
{
    gaddr z = pick_zone(sys_p);
    purge(z);
    compact(z);
    *growp = 0;
    set_mem_err(noErr);
    return (int32_t)cap(largest_possible(z, false));
}

int32_t tb_CompactMem(int32_t sizeneeded, bool sys_p)
{
    gaddr z = pick_zone(sys_p);
    uint32_t largest = compact(z);
    set_mem_err(noErr);
    return (int32_t)cap(largest > HDR ? largest - HDR : 0);
}

void tb_PurgeMem(int32_t needed, bool sys_p)
{
    gaddr z = pick_zone(sys_p);
    purge(z);
    uint32_t largest = compact(z);
    set_mem_err(largest >= (uint32_t)needed + HDR ? noErr : memFullErr);
}

void tb_PurgeSpace(uint32_t *totalp, uint32_t *contigp, bool sys_p)
{
    gaddr z = pick_zone(sys_p);
    uint32_t total = total_free(z);
    gaddr lim = z_lim(z);
    for (gaddr b = z_first(z); b < lim; b = b_next(b))
        if (b_use(b) == USE_REL && (b_state(b) & (ST_PURGE | ST_LOCK)) == ST_PURGE)
            total += b_psize(b);
    *totalp = cap(total);
    *contigp = cap(largest_possible(z, true));
    LOGF(LOG_TRAPS, "  PurgeSpace in zone %08X: total %u, contiguous %u\n", z, *totalp, *contigp);
    set_mem_err(noErr);
}

void tb_ReserveMem(int32_t needed, bool sys_p)
{
    gaddr z = pick_zone(sys_p);
    uint32_t ps = phys_for(needed < 0 ? 0 : (uint32_t)needed);
    set_mem_err(ps && make_room(z, ps) ? noErr : memFullErr);
}

int32_t tb_StackSpace(void)
{
    gaddr limit = rd32(LM_ApplLimit);
    set_mem_err(noErr);
    return g_cpu && g_cpu->a[7] > limit ? (int32_t)(g_cpu->a[7] - limit) : 0;
}

void tb_SetApplLimit(gaddr newlimit)
{
    gaddr z = appl_zone();
    if (newlimit < z_lim(z) + HDR) {
        set_mem_err(memFullErr);
        return;
    }
    wr32(LM_ApplLimit, newlimit);
    set_mem_err(noErr);
}

void tb_MaxApplZone(void)
{
    gaddr z = appl_zone();
    gaddr limit = rd32(LM_ApplLimit);
    if (appl_max_limit && limit > appl_max_limit)
        limit = appl_max_limit;
    if (limit > z_lim(z) + HDR)
        grow_zone(z, limit - HDR - z_lim(z));
    set_mem_err(noErr);
}

void tb_SetApplBase(gaddr newbase) { set_mem_err(noErr); }

void tb_BlockMove(gaddr src, gaddr dst, int32_t cnt, bool flush_p)
{
    if (cnt > 0)
        gmemcpy(dst, src, (uint32_t)cnt);
    set_mem_err(noErr);
}

/* ---------------------------------------------------------------------- */
/* Handle and pointer utilities (ToolboxUtil) */

int16_t tb_HandToHand(uint32_t *h)
{
    gaddr src = *h;
    int32_t n = tb_GetHandleSize(src);
    if (n < 0)
        return (int16_t)n;
    gaddr nh = tb_NewHandle(n, false, false);
    if (!nh)
        return memFullErr;
    gmemcpy(rd32(nh), rd32(src), (uint32_t)n);
    *h = nh;
    return noErr;
}

int16_t tb_PtrToHand(gaddr p, uint32_t *h, int32_t s)
{
    gaddr nh = tb_NewHandle(s, false, false);
    if (!nh) {
        *h = 0;
        return memFullErr;
    }
    gmemcpy(rd32(nh), p, (uint32_t)s);
    *h = nh;
    return noErr;
}

int16_t tb_PtrToXHand(gaddr p, gaddr h, int32_t s)
{
    tb_SetHandleSize(h, s);
    int16_t e = (int16_t)rd16(LM_MemErr);
    if (e)
        return e;
    gmemcpy(rd32(h), p, (uint32_t)s);
    return noErr;
}

int16_t tb_PtrAndHand(gaddr p, gaddr h, int32_t s1)
{
    int32_t n = tb_GetHandleSize(h);
    if (n < 0)
        return (int16_t)n;
    tb_SetHandleSize(h, n + s1);
    int16_t e = (int16_t)rd16(LM_MemErr);
    if (e)
        return e;
    gmemcpy(rd32(h) + (uint32_t)n, p, (uint32_t)s1);
    return noErr;
}

int16_t tb_HandAndHand(gaddr h1, gaddr h2)
{
    int32_t n1 = tb_GetHandleSize(h1);
    if (n1 < 0)
        return (int16_t)n1;
    /* Lock h1 so growing h2 cannot move it out from under the copy. */
    int8_t st = tb_HGetState(h1);
    tb_HLock(h1);
    int16_t e = tb_PtrAndHand(rd32(h1), h2, n1);
    tb_HSetState(h1, st);
    return e;
}

/* ---------------------------------------------------------------------- */
/* Debugging */

bool mm_check_zone(gaddr z)
{
    gaddr lim = z_lim(z);
    gaddr b = z_first(z);
    while (b < lim) {
        uint32_t ps = b_psize(b);
        if (ps < HDR || b + ps > lim || (ps & 3)) {
            log_printf("mm: zone %08X damaged at block %08X (size %u)\n", z, b, ps);
            return false;
        }
        if (b_use(b) == USE_REL) {
            gaddr mp = z + b_loc(b);
            /* The top byte may be the handle's flags, set as programs of
             * the 24-bit days set them (Shufflepuck locks its handles with
             * BSET #7), harmless to blocks below 16 MB. */
            uint32_t v = rd32(mp);
            if (v != b + HDR && ((v & 0x00FFFFFFu) != b + HDR || b + HDR > 0x00FFFFFFu)) {
                log_printf("mm: zone %08X: block %08X master pointer %08X does not point back (it holds %08X)\n", z, b,
                           mp, rd32(mp));
                return false;
            }
        }
        b += ps;
    }
    if (b != lim) {
        log_printf("mm: zone %08X: blocks end at %08X, bkLim is %08X\n", z, b, lim);
        return false;
    }
    return true;
}

/* BLUEBOX_HEAPCHECK=N: the system and application zones checked after
 * every Nth Toolbox call (1, or nothing, for every one), so a call that
 * damages a heap is named rather than whatever trips over it later (an
 * "accepted" Boolean written through a stray pointer broke a menu icon's
 * master pointer, and the crash came two redraws on).  False, with the
 * damage logged, when a zone is damaged. */
static int every = -1;
static const char *checked_after; /* the call the last check followed */

void machine_set_heap_check(int n) { every = n > 0 ? n : 0; }

static int heap_check_every(void)
{
    if (every < 0) {
        const char *e = getenv("BLUEBOX_HEAPCHECK");
        every = e ? (atoi(e) > 0 ? atoi(e) : 1) : 0;
    }
    return every;
}

static bool zones_intact(void)
{
    gaddr zones[2] = { rd32(LM_SysZone), rd32(LM_ApplZone) };
    for (int i = 0; i < 2; i++)
        if (zones[i] && !mm_check_zone(zones[i]))
            return false;
    return true;
}

bool mm_debug_after_call(const char *name)
{
    static unsigned calls;
    if (!heap_check_every() || ++calls % (unsigned)every)
        return true;
    checked_after = name;
    if (!zones_intact()) {
        log_printf("mm: the zone was damaged by %s (or within the %d calls before it)\n", name, every - 1);
        return false;
    }
    return true;
}

/* With every call checked, the zones are checked on the way in too, so
 * damage done by the program's own code between two calls is told from
 * damage the call does. */
bool mm_debug_before_call(const char *name)
{
    if (heap_check_every() != 1 || !checked_after)
        return true;
    if (!zones_intact()) {
        log_printf("mm: the zone was damaged before %s, since the check after %s: by code run between the two"
                   " (the program's own, an interrupt task's, or a call's that called back into the program)\n",
                   name, checked_after);
        return false;
    }
    return true;
}

/* InitZone (A019): A0 points to startPtr, limitPtr, cMoreMasters (word) and
 * pGrowZone.  The new zone becomes the current one. */
void raw_InitZone(cpu68k *c, uint16_t tw)
{
    gaddr pb = c->a[0];
    gaddr start = rd32(pb), limit = rd32(pb + 4);
    uint16_t more = rd16(pb + 8);
    gaddr grow = rd32(pb + 10);
    bool inner = zone_of(start) != 0;
    mm_init_zone(start, limit, more);
    if (inner && nsubzones < MAX_SUBZONES) {
        subzones[nsubzones].z = start;
        subzones[nsubzones++].lim = z_lim(start);
    }
    Zone_set_gzProc(start, grow);
    wr32(LM_TheZone, start);
    set_mem_err(noErr);
    c->d[0] = 0;
}
