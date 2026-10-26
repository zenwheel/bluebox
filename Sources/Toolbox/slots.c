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
 * The format block is at the top, its directory offset leading to the
 * sResource directory.  The NuBus slots ($9-$E) are empty.
 *
 * The slot resource table can be changed as on a Mac: sResources disabled
 * (SetSRsrcState), inserted from a list in RAM (InsertSRTRec), updated and
 * deleted; the startup-time calls build it again from the declaration ROM.
 */
#include <string.h>

#include "qd_internal.h"
#include "tb_internal.h"

enum {
    smEmptySlot = -300,
    smBadRefId = -330,
    smsPointerNil = -335,
    smNilsBlockErr = -336,
    smSlotOOBErr = -337,
    smSelOOBErr = -338,
    smReservedErr = -332,
    smCodeRevErr = -333,
    smNoMoresRsrcs = -344,
    smBadsPtrErr = -346,
    smSRTOvrFlErr = -350,
    smRecNotFnd = -351,
};

/* SpBlock */
enum {
    sp_result = 0, sp_sPointer = 4, sp_size = 8, sp_offsetData = 12, sp_paramData = 24, sp_ioReserved = 36,
    sp_refNum = 38, sp_category = 40, sp_cType = 42, sp_drvrSW = 44, sp_drvrHW = 46, sp_tbMask = 48, sp_slot = 49,
    sp_id = 50, sp_extDev = 51, sp_hwDev = 52, sp_byteLanes = 53, sp_flags = 54,
};

/* sResource list IDs */
enum {
    sRsrcType = 1, sRsrcName = 2, sRsrcFlags = 7, sRsrcHWDevId = 8, boardId = 32,
    mVidParams = 1, mPageCnt = 3, mDevType = 4,
};

typedef struct {
    uint8_t slot, id, extdev, hwdev;
    uint16_t category, ctype, drsw, drhw;
    gaddr list; /* its sResource list */
    int16_t refnum, ioreserved;
    bool disabled, own_refnum;
} srsrc;

enum { MAXRSRC = 16, FHEADER_SIZE = 20 };
static srsrc table[MAXRSRC];
static int nrsrc;
static gaddr rom, sinfo; /* the ROM image; slot 0's SInfoRecord */
static uint32_t rom_size;
static uint8_t slot_pram[16][8]; /* each slot's parameter RAM record */
enum { SLOT_QUEUE = 8 };
static gaddr slot_queue[16][SLOT_QUEUE]; /* SIntInstall's handlers */

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
    be(vp + 4, (uint32_t)((w * depth + 15) / 16 * 2), 2); /* vpRowBytes: as the screen's (qd_screen.c) */
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

    /* The directory, and the format block at the top pointing to it. */
    entry dir[] = { { 1, blist, 0 }, { 0x80, vlist, 0 } };
    uint32_t dlist = put_list(&im, dir, 2);
    uint8_t fh[FHEADER_SIZE] = { 0 };
    uint32_t at = im.n;
    be(fh, (dlist - at) & 0xFFFFFF, 4);      /* fhDirOffset, from itself */
    be(fh + 4, at + FHEADER_SIZE, 4);        /* fhLength */
    fh[12] = 1;                              /* fhROMRev */
    fh[13] = 1;                              /* fhFormat: appleFormat */
    be(fh + 14, 0x5A932BC7, 4);              /* fhTstPat */
    fh[19] = 0x0F;                           /* fhByteLanes: all four */
    put(&im, fh, FHEADER_SIZE);

    rom_size = im.n;
    rom = tb_NewPtr((int32_t)rom_size, true, true);
    if (rom)
        gput(rom, im.b, rom_size);
    table[0] = (srsrc){ 0, 1, 0, 0, 1, 0, 0, 0, rom + blist, 0, 0, false, false };
    table[1] = (srsrc){ 0, 0x80, 0, 1, 3, 1, 1, 1, rom + vlist, 0, 0, false, true };
    nrsrc = 2;

    /* Slot 0's SInfoRecord: initialized, the ROM's top at its end. */
    sinfo = tb_NewPtr(24, true, true);
    if (sinfo) {
        wr32(sinfo, rom + dlist);              /* siDirPtr */
        wr8(sinfo + 9, 0x0F);                  /* siCPUByteLanes */
        wr8(sinfo + 10, 0x0F);                 /* siTopOfROM */
        wr32(sinfo + 16, rom + rom_size - 1);  /* siROMAddr */
    }
    memset(slot_pram, 0, sizeof slot_pram);
    slot_pram[0][1] = 0x01; /* the board ID */
}

void slots_reset(void)
{
    rom = 0;
    memset(slot_queue, 0, sizeof slot_queue);
}

static bool ready(void)
{
    if (!rom)
        build();
    return rom != 0;
}

/* ---- Lists ---- */

static bool in_rom(gaddr p) { return p >= rom && p < rom + rom_size; }
/* A list or structure: in the ROM, or one in RAM a program inserted. */
static bool readable(gaddr p) { return p >= GA_SYSHEAP && p < GA_SCREEN; }

/* The entry with this ID in the list at p, or 0. */
static gaddr find_entry(gaddr p, uint8_t id)
{
    for (int n = 0; readable(p) && n < 256; p += 4, n++) {
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
    if (!readable(list))
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
    wr32(spb + sp_sPointer, s->list);
    wr16(spb + sp_refNum, (uint16_t)(s->own_refnum ? main_refnum() : s->refnum));
    wr16(spb + sp_ioReserved, (uint16_t)s->ioreserved);
    wr16(spb + sp_category, s->category);
    wr16(spb + sp_cType, s->ctype);
    wr16(spb + sp_drvrSW, s->drsw);
    wr16(spb + sp_drvrHW, s->drhw);
}

static uint32_t key(uint8_t slot, uint8_t id, uint8_t extdev) { return (uint32_t)slot << 16 | (uint32_t)id << 8 | extdev; }

/* The first sResource at or after (strictly after, for `next`) the
 * SpBlock's slot, ID and external device; of its type when `typed`
 * (spTBMask's bits 0-3 leave out DrvrHW, DrvrSW, CType, Category). */
static int16_t search(gaddr spb, bool next, bool typed, bool one_slot, bool disabled_too)
{
    uint8_t slot = rd8(spb + sp_slot);
    uint32_t from = key(slot, rd8(spb + sp_id), rd8(spb + sp_extDev));
    uint8_t mask = rd8(spb + sp_tbMask);
    const srsrc *best = NULL;
    for (int i = 0; i < nrsrc; i++) {
        const srsrc *s = &table[i];
        uint32_t k = key(s->slot, s->id, s->extdev);
        if (next ? k <= from : k < from)
            continue;
        if (one_slot && s->slot != slot)
            continue;
        if (s->disabled && !disabled_too)
            continue;
        if (typed && (((mask & 8) == 0 && s->category != rd16(spb + sp_category)) ||
                      ((mask & 4) == 0 && s->ctype != rd16(spb + sp_cType)) ||
                      ((mask & 2) == 0 && s->drsw != rd16(spb + sp_drvrSW)) ||
                      ((mask & 1) == 0 && s->drhw != rd16(spb + sp_drvrHW))))
            continue;
        if (!best || k < key(best->slot, best->id, best->extdev))
            best = s; /* inserted ones keep the table's order */
    }
    if (!best)
        return smNoMoresRsrcs;
    describe(spb, best);
    return noErr;
}

/* The table's entry for the SpBlock's slot, ID and external device. */
static srsrc *lookup(gaddr spb)
{
    uint32_t k = key(rd8(spb + sp_slot), rd8(spb + sp_id), rd8(spb + sp_extDev));
    for (int i = 0; i < nrsrc; i++)
        if (key(table[i].slot, table[i].id, table[i].extdev) == k)
            return &table[i];
    return NULL;
}

/* Slots: 0 the built-in one, $9-$E NuBus, all empty. */
static int16_t check_slot(uint8_t slot)
{
    if (slot == 0)
        return noErr;
    return slot >= 9 && slot <= 14 ? smEmptySlot : smSlotOOBErr;
}

/* The base address of an sResource's device: the screen's for the video. */
static int16_t dev_base(gaddr spb)
{
    srsrc *s = lookup(spb);
    if (!s || s->disabled)
        return smRecNotFnd;
    gaddr gdh = rd32(LM_MainDevice);
    if (s->category != 3 || !gdh || !rd32(gdh))
        return smBadRefId; /* no MinorBaseOS: no device memory */
    gaddr pm = rd32(rd32(gdh) + GDevice_gdPMap_off);
    wr32(spb + sp_result, pm && rd32(pm) ? rd32(rd32(pm)) : rd32(LM_ScrnBase));
    return noErr;
}

/* InsertSRTRec: the sResource at spsPointer (or the card's own list for
 * spID when nil) into the table, enabled or not by spParamData. */
static int16_t insert(gaddr spb)
{
    uint8_t slot = rd8(spb + sp_slot);
    int16_t err = check_slot(slot);
    if (err)
        return err;
    gaddr list = rd32(spb + sp_sPointer);
    if (!list) {
        gaddr e = find_entry(rd32(sinfo), rd8(spb + sp_id));
        if (!e)
            return smBadRefId;
        list = entry_target(e);
    }
    if (!readable(list))
        return smBadsPtrErr;
    srsrc *s = lookup(spb);
    if (!s) {
        if (nrsrc == MAXRSRC)
            return smSRTOvrFlErr;
        s = &table[nrsrc++];
    }
    gaddr te = find_entry(list, sRsrcType), t = te ? entry_target(te) : 0;
    gaddr he = find_entry(list, sRsrcHWDevId);
    *s = (srsrc){ slot, rd8(spb + sp_id), rd8(spb + sp_extDev), he ? (uint8_t)entry_data(he) : 0,
                  t ? rd16(t) : 0, t ? rd16(t + 2) : 0, t ? rd16(t + 4) : 0, t ? rd16(t + 6) : 0,
                  list, (int16_t)rd16(spb + sp_refNum), 0, (rd32(spb + sp_paramData) & 1) != 0, false };
    return noErr;
}

/* A block's data in a new pointer: spResult. */
static int16_t get_block(gaddr spb, gaddr e, bool cstring)
{
    gaddr p = entry_target(e);
    if (!readable(p))
        return smBadsPtrErr;
    uint32_t n = cstring ? 0 : rd32(p) - 4;
    if (cstring)
        while (readable(p + n) && n < 0x10000 && rd8(p + n))
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
    srsrc *s;
    switch (sel) {
    case 0x08: /* SVersion: the Slot Manager's version in spResult (1, System 7's) */
        wr32(spb + sp_result, 1);
        wr32(spb + sp_sPointer, 0);
        return noErr;
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
        if (!readable(p) || !readable(p + n - 1))
            return smBadsPtrErr;
        gmemcpy(rd32(spb + sp_result), p, n);
        return noErr;
    }
    case 0x09: /* SetSRsrcState: spParamData 0 enables, 1 disables */
        if (!(s = lookup(spb)))
            return smRecNotFnd;
        s->disabled = (flags & 1) != 0;
        return noErr;
    case 0x0A: /* InsertSRTRec */
        return insert(spb);
    case 0x0B: /* SGetSRsrc */
        return search(spb, flags & 4, false, flags & 2, flags & 1);
    case 0x0C: /* SGetTypeSRsrc */
        return search(spb, true, true, flags & 2, flags & 1);
    case 0x10: /* SReadInfo: the slot's SInfoRecord, copied to spResult */
        if ((err = check_slot(rd8(spb + sp_slot))))
            return err;
        gmemcpy(rd32(spb + sp_result), sinfo, 24);
        return noErr;
    case 0x11: /* SReadPRAMRec */
    case 0x12: /* SPutPRAMRec */
        if ((err = check_slot(rd8(spb + sp_slot))))
            return err;
        if (sel == 0x11)
            gput(rd32(spb + sp_result), slot_pram[0], 8);
        else /* the board ID stays the card's */
            gget(slot_pram[0] + 2, rd32(spb + sp_sPointer) + 2, 6);
        return noErr;
    case 0x13: /* SReadFHeader: the format block, to spResult */
        if ((err = check_slot(rd8(spb + sp_slot))))
            return err;
        gmemcpy(rd32(spb + sp_result), rom + rom_size - FHEADER_SIZE, FHEADER_SIZE);
        return noErr;
    case 0x14: /* SNextSRsrc */
        return search(spb, true, false, false, false);
    case 0x15: /* SNextTypeSRsrc */
        return search(spb, true, true, false, false);
    case 0x16: /* SRsrcInfo: exactly this one */
        if (!(s = lookup(spb)) || s->disabled)
            return smRecNotFnd;
        describe(spb, s);
        return noErr;
    case 0x17: /* SDisposePtr */
        if (rd32(spb + sp_result))
            tb_DisposePtr(rd32(spb + sp_result));
        return noErr;
    case 0x18: /* SCkCardStat: the card initialized */
        return check_slot(rd8(spb + sp_slot));
    case 0x19: { /* SReadDrvrName: the sResource's name with a period, as a Str255 at spResult */
        uint8_t id = rd8(spb + sp_id), slot = rd8(spb + sp_slot);
        for (int i = 0; i < nrsrc; i++)
            if (table[i].slot == slot && table[i].id == id) {
                gaddr ne = find_entry(table[i].list, sRsrcName), out = rd32(spb + sp_result);
                if (!ne)
                    return smBadRefId;
                gaddr name = entry_target(ne);
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
    case 0x1B: /* SFindDevBase */
    case 0x1C: /* SFindBigDevBase */
        return dev_base(spb);
    case 0x1D: /* SGetSRsrcPtr: the list of this one, disabled ones too if asked */
        if (!(s = lookup(spb)) || (s->disabled && !(flags & 1)))
            return smRecNotFnd;
        wr32(spb + sp_sPointer, s->list);
        return noErr;
    case 0x20: /* InitSDeclMgr */
    case 0x21: /* SPrimaryInit */
    case 0x25: /* SInitPRAMRecs */
    case 0x29: /* SInitSRsrcTable: all built again from the declaration ROM */
        build();
        return rom ? noErr : smNilsBlockErr;
    case 0x22: /* SCardChanged: no card is ever changed */
        if ((err = check_slot(rd8(spb + sp_slot))))
            return err;
        wr32(spb + sp_result, 0);
        return noErr;
    case 0x23: /* SExec: the ROM has no sExec blocks */
        return (err = named_entry(spb, &e)) ? err : smCodeRevErr;
    case 0x24: /* SOffsetData: an entry's 24-bit field */
        if ((err = named_entry(spb, &e)) == noErr) {
            wr32(spb + sp_offsetData, entry_data(e));
            wr8(spb + sp_byteLanes, 0x0F);
        }
        return err;
    case 0x26: { /* SReadPBSize: the size of the block an entry points to */
        if ((err = named_entry(spb, &e)))
            return err;
        uint32_t n = rd32(entry_target(e));
        if ((rd8(spb + sp_flags) & 2) && (n >> 24)) /* fCkReserved */
            return smReservedErr;
        wr32(spb + sp_size, n & 0xFFFFFF);
        return noErr;
    }
    case 0x28: /* SCalcStep: all four byte lanes, so bytes are 1 apart and IDs 4 */
        wr32(spb + sp_result, (rd8(spb + sp_flags) & 1) ? 1 : 4);
        return noErr;
    case 0x2A: /* SSearchSRT: this one or (fCkForNext) the next */
        if (rd8(spb + sp_flags) & 2)
            return search(spb, true, false, false, true) ? smRecNotFnd : noErr;
        if (!(s = lookup(spb)))
            return smRecNotFnd;
        describe(spb, s);
        return noErr;
    case 0x2B: /* SUpdateSRT: a new reference number and IOReserved */
        if (!(s = lookup(spb)))
            return insert(spb);
        s->refnum = (int16_t)rd16(spb + sp_refNum);
        s->ioreserved = (int16_t)rd16(spb + sp_ioReserved);
        s->own_refnum = false;
        return noErr;
    case 0x2C: { /* SCalcSPointer: spOffsetData bytes on from spsPointer */
        gaddr p = rd32(spb + sp_sPointer);
        if (!p)
            return smsPointerNil;
        wr32(spb + sp_sPointer, p + rd32(spb + sp_offsetData));
        return noErr;
    }
    case 0x2D: /* SGetDriver: the card has no driver of its own in its ROM */
        if (!(s = lookup(spb)))
            return smRecNotFnd;
        return find_entry(s->list, 4 /* sRsrcDrvrDir */) ? smCodeRevErr : smBadRefId;
    case 0x2E: { /* SPtrToSlot */
        gaddr p = rd32(spb + sp_sPointer);
        if (in_rom(p)) {
            wr8(spb + sp_slot, 0);
            return noErr;
        }
        uint8_t slot = (uint8_t)(p >> 24);
        if (slot >= 0xF9 && slot <= 0xFE) { /* a NuBus slot's space */
            wr8(spb + sp_slot, slot & 0xF);
            return noErr;
        }
        return smBadsPtrErr;
    }
    case 0x2F: /* SFindSInfoRecPtr */
        if ((err = check_slot(rd8(spb + sp_slot))))
            return err;
        wr32(spb + sp_result, sinfo);
        return noErr;
    case 0x30: /* SFindSRsrcPtr */
        if (!(s = lookup(spb)) || s->disabled)
            return smRecNotFnd;
        wr32(spb + sp_sPointer, s->list);
        return noErr;
    case 0x31: /* SDeleteSRTRec */
        if (!(s = lookup(spb)))
            return smRecNotFnd;
        *s = table[--nrsrc];
        return noErr;
    default:
        return smSelOOBErr;
    }
}

/* SIntInstall ($A075) and SIntRemove ($A076): a slot's interrupt queue.
 * The NuBus slots are empty, so their handlers are kept but never run. */

int16_t tb_SIntInstall(gaddr sIntQElemPtr, int16_t theSlot)
{
    if (theSlot < 9 || theSlot > 14)
        return smSlotOOBErr;
    for (int i = 0; i < SLOT_QUEUE; i++)
        if (!slot_queue[theSlot][i]) {
            slot_queue[theSlot][i] = sIntQElemPtr;
            return noErr;
        }
    return memFullErr;
}

int16_t tb_SIntRemove(gaddr sIntQElemPtr, int16_t theSlot)
{
    if (theSlot < 9 || theSlot > 14)
        return smSlotOOBErr;
    for (int i = 0; i < SLOT_QUEUE; i++)
        if (slot_queue[theSlot][i] == sIntQElemPtr) {
            slot_queue[theSlot][i] = 0;
            return noErr;
        }
    return -1; /* qErr: not in the queue */
}

/* InterfaceLib's calls on an SpBlock, each the trap's selector. */
int16_t tb_SReadInfo(gaddr spBlkPtr) { return slot_manager(0x10, spBlkPtr); }
int16_t tb_SReadPRAMRec(gaddr spBlkPtr) { return slot_manager(0x11, spBlkPtr); }
int16_t tb_SPutPRAMRec(gaddr spBlkPtr) { return slot_manager(0x12, spBlkPtr); }
int16_t tb_SReadFHeader(gaddr spBlkPtr) { return slot_manager(0x13, spBlkPtr); }
int16_t tb_SCkCardStat(gaddr spBlkPtr) { return slot_manager(0x18, spBlkPtr); }
int16_t tb_SFindDevBase(gaddr spBlkPtr) { return slot_manager(0x1B, spBlkPtr); }
int16_t tb_SFindBigDevBase(gaddr spBlkPtr) { return slot_manager(0x1C, spBlkPtr); }
int16_t tb_InitSDeclMgr(gaddr spBlkPtr) { return slot_manager(0x20, spBlkPtr); }
int16_t tb_SPrimaryInit(gaddr spBlkPtr) { return slot_manager(0x21, spBlkPtr); }
int16_t tb_SCardChanged(gaddr spBlkPtr) { return slot_manager(0x22, spBlkPtr); }
int16_t tb_SExec(gaddr spBlkPtr) { return slot_manager(0x23, spBlkPtr); }
int16_t tb_SOffsetData(gaddr spBlkPtr) { return slot_manager(0x24, spBlkPtr); }
int16_t tb_SInitPRAMRecs(gaddr spBlkPtr) { return slot_manager(0x25, spBlkPtr); }
int16_t tb_SReadPBSize(gaddr spBlkPtr) { return slot_manager(0x26, spBlkPtr); }
int16_t tb_SCalcStep(gaddr spBlkPtr) { return slot_manager(0x28, spBlkPtr); }
int16_t tb_SInitSRsrcTable(gaddr spBlkPtr) { return slot_manager(0x29, spBlkPtr); }
int16_t tb_SSearchSRT(gaddr spBlkPtr) { return slot_manager(0x2A, spBlkPtr); }
int16_t tb_SUpdateSRT(gaddr spBlkPtr) { return slot_manager(0x2B, spBlkPtr); }
int16_t tb_SCalcSPointer(gaddr spBlkPtr) { return slot_manager(0x2C, spBlkPtr); }
int16_t tb_SGetDriver(gaddr spBlkPtr) { return slot_manager(0x2D, spBlkPtr); }
int16_t tb_SPtrToSlot(gaddr spBlkPtr) { return slot_manager(0x2E, spBlkPtr); }
int16_t tb_SFindSInfoRecPtr(gaddr spBlkPtr) { return slot_manager(0x2F, spBlkPtr); }
int16_t tb_SFindSRsrcPtr(gaddr spBlkPtr) { return slot_manager(0x30, spBlkPtr); }
int16_t tb_SDeleteSRTRec(gaddr spBlkPtr) { return slot_manager(0x31, spBlkPtr); }
int16_t tb_SetSRsrcState(gaddr spBlkPtr) { return slot_manager(0x09, spBlkPtr); }
int16_t tb_InsertSRTRec(gaddr spBlkPtr) { return slot_manager(0x0A, spBlkPtr); }
int16_t tb_SGetSRsrcPtr(gaddr spBlkPtr) { return slot_manager(0x1D, spBlkPtr); }

/* OpenSlot: the Device Manager's open of a card's driver, the slot and
 * sResource in the parameter block (ioSlot, ioID) beside its name. */
int16_t tb_OpenSlot(gaddr paramBlock, int8_t async) { return tb_PBOpen(paramBlock, async); }
int16_t tb_OpenSlotSync(gaddr paramBlock) { return tb_PBOpen(paramBlock, false); }
int16_t tb_OpenSlotAsync(gaddr paramBlock) { return tb_PBOpen(paramBlock, true); }

/* SlotManager (A06E): A0 the SpBlock, D0 the selector and the result. */
void raw_SlotManager(cpu68k *c, uint16_t tw) { c->d[0] = (uint32_t)(int32_t)slot_manager((uint16_t)c->d[0], c->a[0]); }
