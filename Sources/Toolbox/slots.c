/*
 * The Slot Manager: a Mac with no NuBus or PCI cards, only its built-in
 * video in slot 0.  Programs looking for video cards (SGetTypeSRsrc with
 * catDisplay, typeVideo) find the main screen there, with a mode list for
 * each depth the screen can take at its size; everything else ends with
 * smNoMoresRsrcs.
 *
 * The sResources are kept as a declaration ROM would have them, in guest
 * memory with all four byte lanes: a list is entries of an ID byte and a
 * 24-bit field, either data (bytes and words) or an offset from the entry
 * to a structure; a block is a long holding its own size and then the data.
 */
#include <string.h>

#include "qd_internal.h"
#include "tb_internal.h"

enum {
    smBadRefId = -330,
    smsPointerNil = -335,
    smNilsBlockErr = -336,
    smNoMoresRsrcs = -344,
    smBadsPtrErr = -346,
    smRecNotFnd = -351,
};

/* SpBlock */
enum {
    sp_result = 0, sp_sPointer = 4, sp_size = 8, sp_paramData = 24, sp_ioReserved = 36, sp_refNum = 38,
    sp_category = 40, sp_cType = 42, sp_drvrSW = 44, sp_drvrHW = 46, sp_tbMask = 48, sp_slot = 49, sp_id = 50,
    sp_extDev = 51, sp_hwDev = 52,
};

/* sResource list IDs */
enum {
    sRsrcType = 1, sRsrcName = 2, sRsrcFlags = 7, sRsrcHWDevId = 8, boardId = 32,
    mVidParams = 1, mPageCnt = 3, mDevType = 4,
};

typedef struct {
    uint8_t slot, id, extdev, hwdev;
    uint16_t category, ctype, drsw, drhw;
    uint32_t list; /* offset of its list in the ROM image */
} srsrc;

enum { NRSRC = 2 };
static srsrc table[NRSRC];
static gaddr rom;
static uint32_t rom_size;

/* ---- Building the image ---- */

typedef struct {
    uint8_t b[2048];
    uint32_t n;
} image;

static uint32_t put(image *im, const void *p, uint32_t len)
{
    uint32_t at = im->n;
    if (at + len <= sizeof im->b) {
        memcpy(im->b + at, p, len);
        im->n += len;
    }
    return at;
}

static void be(uint8_t *p, uint32_t v, int n)
{
    for (int i = n - 1; i >= 0; i--, v >>= 8)
        p[i] = (uint8_t)v;
}

/* An entry pointing at `target`, or holding `data` when target is 0. */
typedef struct {
    uint8_t id;
    uint32_t target, data;
} entry;

static uint32_t put_list(image *im, const entry *e, int n)
{
    uint32_t at = im->n;
    for (int i = 0; i < n; i++) {
        uint8_t w[4] = { e[i].id };
        uint32_t here = at + 4 * (uint32_t)i;
        be(w + 1, e[i].target ? e[i].target - here : e[i].data, 3);
        put(im, w, 4);
    }
    uint8_t end[4] = { 0xFF, 0, 0, 0 };
    put(im, end, 4);
    return at;
}

static uint32_t put_type(image *im, uint16_t cat, uint16_t type, uint16_t sw, uint16_t hw)
{
    uint8_t t[8];
    be(t, cat, 2);
    be(t + 2, type, 2);
    be(t + 4, sw, 2);
    be(t + 6, hw, 2);
    return put(im, t, 8);
}

static uint32_t put_cstring(image *im, const char *s)
{
    uint32_t at = put(im, s, (uint32_t)strlen(s) + 1);
    while (im->n & 1)
        put(im, "", 1);
    return at;
}

/* A VPBlock, in a block, for the screen at one depth. */
static uint32_t put_vpblock(image *im, int depth)
{
    uint8_t b[4 + 42] = { 0 };
    be(b, sizeof b, 4);
    uint8_t *vp = b + 4;
    int w = g_screen_width, h = g_screen_height;
    be(vp + 4, (uint32_t)((w * depth + 31) / 32 * 4), 2); /* vpRowBytes */
    be(vp + 10, (uint32_t)h, 2);                           /* vpBounds.bottom */
    be(vp + 12, (uint32_t)w, 2);                           /* vpBounds.right */
    be(vp + 18, 0, 4);                                     /* vpPackSize */
    be(vp + 22, 72u << 16, 4);                             /* vpHRes */
    be(vp + 26, 72u << 16, 4);                             /* vpVRes */
    be(vp + 30, depth > 8 ? 16 : 0, 2);                    /* vpPixelType: RGBDirect or chunky */
    be(vp + 32, (uint32_t)depth, 2);
    be(vp + 34, depth > 8 ? 3 : 1, 2);
    be(vp + 36, (uint32_t)(depth == 16 ? 5 : depth == 32 ? 8 : depth), 2);
    return put(im, b, sizeof b);
}

static void build(void)
{
    static image im;
    im.n = 0;
    static const int depths[] = { 1, 2, 4, 8, 16, 32 };
    enum { NDEPTHS = sizeof depths / sizeof depths[0] };

    /* The board: catBoard, typeBoard. */
    uint32_t btype = put_type(&im, 1, 0, 0, 0), bname = put_cstring(&im, "Blue Box");
    entry board[] = { { sRsrcType, btype, 0 }, { sRsrcName, bname, 0 }, { boardId, 0, 0x0001 } };
    uint32_t blist = put_list(&im, board, 3);

    /* The video: catDisplay, typeVideo, drSwApple, and a mode list a depth. */
    uint32_t vtype = put_type(&im, 3, 1, 1, 1), vname = put_cstring(&im, "Display_Video_Apple_BlueBox");
    uint32_t modes[NDEPTHS];
    for (int i = 0; i < NDEPTHS; i++) {
        uint32_t vp = put_vpblock(&im, depths[i]);
        /* mDevType: clutType for indexed depths, directType above */
        entry m[] = { { mVidParams, vp, 0 }, { mPageCnt, 0, 1 }, { mDevType, 0, depths[i] > 8 ? 2 : 0 } };
        modes[i] = put_list(&im, m, 3);
    }
    entry video[4 + NDEPTHS] = { { sRsrcType, vtype, 0 }, { sRsrcName, vname, 0 },
                                 { sRsrcFlags, 0, 0x0006 }, /* fOpenAtStart, f32BitMode */
                                 { sRsrcHWDevId, 0, 1 } };
    for (int i = 0; i < NDEPTHS; i++)
        video[4 + i] = (entry){ (uint8_t)(0x80 + i), modes[i], 0 };
    uint32_t vlist = put_list(&im, video, 4 + NDEPTHS);

    rom_size = im.n;
    rom = tb_NewPtr((int32_t)rom_size, true, true);
    if (rom)
        gput(rom, im.b, rom_size);
    table[0] = (srsrc){ 0, 1, 0, 0, 1, 0, 0, 0, blist };
    table[1] = (srsrc){ 0, 0x80, 0, 1, 3, 1, 1, 1, vlist };
}

void slots_reset(void) { rom = 0; }

static bool ready(void)
{
    if (!rom)
        build();
    return rom != 0;
}

/* ---- Lists ---- */

static bool in_rom(gaddr p) { return p >= rom && p < rom + rom_size; }

/* The entry with this ID in the list at p, or 0. */
static gaddr find_entry(gaddr p, uint8_t id)
{
    for (; in_rom(p); p += 4) {
        uint8_t eid = rd8(p);
        if (eid == 0xFF)
            break;
        if (eid == id)
            return p;
    }
    return 0;
}

static uint32_t entry_data(gaddr e) { return rd32(e) & 0xFFFFFF; }

static gaddr entry_target(gaddr e)
{
    int32_t off = (int32_t)(rd32(e) << 8) >> 8;
    return e + (gaddr)off;
}

/* The entry an SpBlock names (spsPointer, spID), or an error. */
static int16_t named_entry(gaddr spb, gaddr *e)
{
    gaddr list = rd32(spb + sp_sPointer);
    if (!list)
        return smsPointerNil;
    if (!in_rom(list))
        return smBadsPtrErr;
    *e = find_entry(list, rd8(spb + sp_id));
    return *e ? noErr : smBadRefId;
}

/* ---- The sResource table ---- */

static int16_t main_refnum(void)
{
    gaddr gdh = rd32(LM_MainDevice);
    return gdh && rd32(gdh) ? (int16_t)rd16(rd32(gdh) + GDevice_gdRefNum_off) : 0;
}

static void describe(gaddr spb, const srsrc *s)
{
    wr8(spb + sp_slot, s->slot);
    wr8(spb + sp_id, s->id);
    wr8(spb + sp_extDev, s->extdev);
    wr8(spb + sp_hwDev, s->hwdev);
    wr32(spb + sp_sPointer, rom + s->list);
    wr16(spb + sp_refNum, (uint16_t)(s->category == 3 ? main_refnum() : 0));
    wr16(spb + sp_ioReserved, 0);
    wr16(spb + sp_category, s->category);
    wr16(spb + sp_cType, s->ctype);
    wr16(spb + sp_drvrSW, s->drsw);
    wr16(spb + sp_drvrHW, s->drhw);
}

static uint32_t key(uint8_t slot, uint8_t id, uint8_t extdev) { return (uint32_t)slot << 16 | (uint32_t)id << 8 | extdev; }

/* The first sResource at or after (strictly after, for `next`) the
 * SpBlock's slot, ID and external device; of its type when `typed`
 * (spTBMask's bits 0-3 leave out DrvrHW, DrvrSW, CType, Category). */
static int16_t search(gaddr spb, bool next, bool typed, bool one_slot)
{
    uint8_t slot = rd8(spb + sp_slot);
    uint32_t from = key(slot, rd8(spb + sp_id), rd8(spb + sp_extDev));
    uint8_t mask = rd8(spb + sp_tbMask);
    for (int i = 0; i < NRSRC; i++) {
        const srsrc *s = &table[i];
        uint32_t k = key(s->slot, s->id, s->extdev);
        if (next ? k <= from : k < from)
            continue;
        if (one_slot && s->slot != slot)
            continue;
        if (typed && (((mask & 8) == 0 && s->category != rd16(spb + sp_category)) ||
                      ((mask & 4) == 0 && s->ctype != rd16(spb + sp_cType)) ||
                      ((mask & 2) == 0 && s->drsw != rd16(spb + sp_drvrSW)) ||
                      ((mask & 1) == 0 && s->drhw != rd16(spb + sp_drvrHW))))
            continue;
        describe(spb, s);
        return noErr;
    }
    return smNoMoresRsrcs;
}

/* A block's data in a new pointer: spResult. */
static int16_t get_block(gaddr spb, gaddr e, bool cstring)
{
    gaddr p = entry_target(e);
    if (!in_rom(p))
        return smBadsPtrErr;
    uint32_t n = cstring ? 0 : rd32(p) - 4;
    if (cstring)
        while (in_rom(p + n) && rd8(p + n))
            n++;
    gaddr out = tb_NewPtr((int32_t)n + (cstring ? 1 : 0), false, true);
    if (!out)
        return smNilsBlockErr;
    gmemcpy(out, cstring ? p : p + 4, n);
    wr32(spb + sp_result, out);
    return noErr;
}

int16_t slot_manager(uint16_t sel, gaddr spb)
{
    if (!spb || !ready())
        return smNoMoresRsrcs;
    uint32_t flags = rd32(spb + sp_paramData);
    gaddr e = 0;
    int16_t err;
    switch (sel) {
    case 0x00: /* SReadByte */
    case 0x01: /* SReadWord */
        if ((err = named_entry(spb, &e)) == noErr)
            wr32(spb + sp_result, sel ? entry_data(e) & 0xFFFF : entry_data(e) & 0xFF);
        return err;
    case 0x02: /* SReadLong */
        if ((err = named_entry(spb, &e)) == noErr)
            wr32(spb + sp_result, rd32(entry_target(e)));
        return err;
    case 0x03: /* SGetCString */
        return (err = named_entry(spb, &e)) ? err : get_block(spb, e, true);
    case 0x05: /* SGetBlock */
        return (err = named_entry(spb, &e)) ? err : get_block(spb, e, false);
    case 0x06: /* SFindStruct */
        if ((err = named_entry(spb, &e)) == noErr)
            wr32(spb + sp_sPointer, entry_target(e));
        return err;
    case 0x07: { /* SReadStruct: spSize bytes at spsPointer into spResult */
        gaddr p = rd32(spb + sp_sPointer);
        uint32_t n = rd32(spb + sp_size);
        if (!in_rom(p) || !in_rom(p + n - 1))
            return smBadsPtrErr;
        gmemcpy(rd32(spb + sp_result), p, n);
        return noErr;
    }
    case 0x0B: /* SGetSRsrc */
        return search(spb, flags & 4, false, flags & 2);
    case 0x0C: /* SGetTypeSRsrc */
        return search(spb, true, true, flags & 2);
    case 0x14: /* SNextSRsrc */
        return search(spb, true, false, false);
    case 0x15: /* SNextTypeSRsrc */
        return search(spb, true, true, false);
    case 0x16: { /* SRsrcInfo: exactly this one */
        uint32_t k = key(rd8(spb + sp_slot), rd8(spb + sp_id), rd8(spb + sp_extDev));
        for (int i = 0; i < NRSRC; i++)
            if (key(table[i].slot, table[i].id, table[i].extdev) == k) {
                describe(spb, &table[i]);
                return noErr;
            }
        return smRecNotFnd;
    }
    case 0x17: /* SDisposePtr */
        if (rd32(spb + sp_result))
            tb_DisposePtr(rd32(spb + sp_result));
        return noErr;
    case 0x19: { /* SReadDrvrName: the sResource's name with a period, as a Str255 at spResult */
        uint32_t k = key(rd8(spb + sp_slot), rd8(spb + sp_id), 0);
        for (int i = 0; i < NRSRC; i++)
            if (key(table[i].slot, table[i].id, 0) == k) {
                gaddr name = entry_target(find_entry(rom + table[i].list, sRsrcName)), out = rd32(spb + sp_result);
                uint8_t n = 1;
                wr8(out + 1, '.');
                while (rd8(name + n - 1) && n < 255) {
                    wr8(out + 1 + n, rd8(name + n - 1));
                    n++;
                }
                wr8(out, n);
                return noErr;
            }
        return smRecNotFnd;
    }
    default:
        return smNoMoresRsrcs;
    }
}

/* SlotManager (A06E): A0 the SpBlock, D0 the selector and the result. */
void raw_SlotManager(cpu68k *c, uint16_t tw) { c->d[0] = (uint32_t)(int32_t)slot_manager((uint16_t)c->d[0], c->a[0]); }
