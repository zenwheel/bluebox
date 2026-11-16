/*
 * PowerPC (32-bit, big-endian) disassembler, MacsBug style: lowercase
 * mnemonics padded to 10 columns, simplified mnemonics where they apply,
 * branch targets as absolute $XXXXXXXX.  Covers the 601 (POWER leftovers
 * included), 603/604/G3 user and supervisor instructions and the AltiVec
 * loads and stores of opcode 31; the AltiVec vector ops (opcode 4) print
 * as .long.  Pure function, no state.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ppc.h"

typedef struct {
    char mn[40];
    char ops[112];
    char t[3][24];            /* scratch for operand pieces */
} Dis;

#define RD(o) ((int)(((o) >> 21) & 31))
#define RA(o) ((int)(((o) >> 16) & 31))
#define RB(o) ((int)(((o) >> 11) & 31))
#define SIMM(o) ((int)(int16_t)((o) & 0xFFFF))
#define UIMM(o) ((unsigned)((o) & 0xFFFF))

#define OPS(d, ...) snprintf((d)->ops, sizeof (d)->ops, __VA_ARGS__)

static void setmn(Dis *d, const char *name, int oe, int rc)
{
    snprintf(d->mn, sizeof d->mn, "%s%s%s", name, oe ? "o" : "", rc ? "." : "");
}

/* A signed 16-bit value as -0x0004 / 0x0010. */
static const char *sx(char *t, int v)
{
    snprintf(t, 24, "%s0x%04X", v < 0 ? "-" : "", (unsigned)(v < 0 ? -v : v));
    return t;
}

/* rA in an address: r0 means the value 0. */
static const char *ra0(char *t, int n)
{
    if (n)
        snprintf(t, 24, "r%d", n);
    else
        strcpy(t, "0");
    return t;
}

static void disp(Dis *d, int regch, int rt, int off, int ra)
{
    OPS(d, "%c%d,%s(%s)", regch, rt, sx(d->t[0], off), ra0(d->t[1], ra));
}

static const char *crfx(char *t, int crf)
{
    if (crf)
        snprintf(t, 24, "cr%d,", crf);
    else
        t[0] = 0;
    return t;
}

/* Trap condition names for the twXX / twXXi forms. */
static const char *tocond(int to)
{
    switch (to) {
    case 16: return "lt";
    case 20: return "le";
    case 4: return "eq";
    case 12: return "ge";
    case 8: return "gt";
    case 24: return "ne";
    case 2: return "llt";
    case 6: return "lle";
    case 5: return "lge";
    case 1: return "lgt";
    }
    return NULL;
}

/* rlwinm and its simplified forms. */
static void do_rlwinm(Dis *d, int ra, int rs, int sh, int mb, int me, int rc)
{
    const char *nm;
    int a = -1, b = -1;

    if (mb == 0 && sh && me == 31 - sh) { nm = "slwi"; a = sh; }
    else if (sh && mb && me == 31 && sh == 32 - mb) { nm = "srwi"; a = mb; }
    else if (sh == 0 && me == 31 && mb) { nm = "clrlwi"; a = mb; }
    else if (sh == 0 && mb == 0 && me < 31) { nm = "clrrwi"; a = 31 - me; }
    else if (mb == 0 && me == 31) { nm = "rotlwi"; a = sh; }
    else if (sh && mb && me == 31 - sh && mb + sh <= 31) { nm = "clrlslwi"; a = mb + sh; b = sh; }
    else if (mb == 0) { nm = "extlwi"; a = me + 1; b = sh; }
    else if (mb && me == 31 && sh >= 32 - mb) { nm = "extrwi"; a = 32 - mb; b = sh - a; }
    else {
        setmn(d, "rlwinm", 0, rc);
        OPS(d, "r%d,r%d,%d,%d,%d", ra, rs, sh, mb, me);
        return;
    }
    setmn(d, nm, 0, rc);
    if (b < 0)
        OPS(d, "r%d,r%d,%d", ra, rs, a);
    else
        OPS(d, "r%d,r%d,%d,%d", ra, rs, a, b);
}

static void do_rlwimi(Dis *d, int ra, int rs, int sh, int mb, int me, int rc)
{
    if (mb <= me) {
        int n = me - mb + 1, b = mb;
        if (sh == ((32 - b) & 31)) {
            setmn(d, "inslwi", 0, rc);
            OPS(d, "r%d,r%d,%d,%d", ra, rs, n, b);
            return;
        }
        if (sh == ((32 - (b + n)) & 31)) {
            setmn(d, "insrwi", 0, rc);
            OPS(d, "r%d,r%d,%d,%d", ra, rs, n, b);
            return;
        }
    }
    setmn(d, "rlwimi", 0, rc);
    OPS(d, "r%d,r%d,%d,%d,%d", ra, rs, sh, mb, me);
}

/* bc, bclr, bcctr (kind 0, 1, 2) with the simplified branch mnemonics. */
static void do_branch(Dis *d, uint32_t addr, uint32_t op, int kind)
{
    static const char *const ctrcond[4] = {"bdnzf", "bdzf", "bdnzt", "bdzt"};
    static const char *const tr[4] = {"lt", "gt", "eq", "so"};
    static const char *const fl[4] = {"ge", "le", "ne", "ns"};
    int bo = RD(op), bi = RA(op), lk = (int)(op & 1), aa = (op & 2) != 0;
    int hint = bo & 1;
    int32_t bd = (int16_t)(op & 0xFFFC);
    uint32_t target = aa ? (uint32_t)bd : addr + (uint32_t)bd;
    char base[16], name[40], cr[24], tg[16];
    int raw = 0, withbi = 0, crf = 0;

    base[0] = 0;
    cr[0] = 0;
    snprintf(tg, sizeof tg, "$%08X", (unsigned)target);
    if ((bo & 0x14) == 0x14) {
        if ((bo & 0x0A) || kind == 0)
            raw = 1;
        else
            strcpy(base, "b");
    } else if ((bo & 0x14) == 0x04) {
        if (bo & 2)
            raw = 1;
        else {
            snprintf(base, sizeof base, "b%s", (bo & 8) ? tr[bi & 3] : fl[bi & 3]);
            crf = bi >> 2;
            crfx(cr, crf);
        }
    } else if ((bo & 0x14) == 0x10) {
        if ((bo & 8) || kind == 2)
            raw = 1;
        else
            strcpy(base, (bo & 2) ? "bdz" : "bdnz");
    } else {
        if (kind == 2)
            raw = 1;
        else {
            strcpy(base, ctrcond[((bo >> 2) & 2) | ((bo >> 1) & 1)]);
            withbi = 1;
        }
    }
    if (raw) {
        snprintf(name, sizeof name, "%s%s%s", kind == 0 ? "bc" : kind == 1 ? "bclr" : "bcctr",
                 lk ? "l" : "", (kind == 0 && aa) ? "a" : "");
        strcpy(d->mn, name);
        if (kind == 0)
            OPS(d, "%d,%d,%s", bo, bi, tg);
        else
            OPS(d, "%d,%d", bo, bi);
        return;
    }
    snprintf(name, sizeof name, "%s%s%s%s%s", base, kind == 1 ? "lr" : kind == 2 ? "ctr" : "",
             lk ? "l" : "", (kind == 0 && aa) ? "a" : "",
             !hint ? "" : (kind == 0 && bd < 0) ? "-" : "+");
    strcpy(d->mn, name);
    if (kind == 0) {
        if (withbi)
            OPS(d, "%d,%s", bi, tg);
        else
            OPS(d, "%s%s", cr, tg);
    } else if (withbi) {
        OPS(d, "%d", bi);
    } else {
        size_t n = strlen(cr);
        if (n)
            cr[n - 1] = 0;                  /* no trailing comma */
        OPS(d, "%s", cr);
    }
}

/* Opcode 31 forms handled by table. */
enum {
    F_DAB,      /* rD,rA,rB */
    F_DA,       /* rD,rA */
    F_SAB,      /* rA,rS,rB */
    F_AS,       /* rA,rS */
    F_ASH,      /* rA,rS,SH */
    F_LX,       /* rD,rA|0,rB */
    F_FLX,      /* fD,rA|0,rB */
    F_VLX,      /* vD,rA|0,rB */
    F_CACHE,    /* rA|0,rB */
    F_NONE,
    F_B         /* rB */
};
enum { RC_NO, RC_OK, RC_ONE, OE_OK };

typedef struct {
    uint16_t xo;
    uint8_t fmt, flags;
    const char *name;
} X31;

static const X31 x31[] = {
    /* integer arithmetic (xo is the 9-bit field; OE where flags say so) */
    {8, F_DAB, OE_OK, "subfc"}, {10, F_DAB, OE_OK, "addc"}, {11, F_DAB, RC_OK, "mulhwu"},
    {40, F_DAB, OE_OK, "subf"}, {75, F_DAB, RC_OK, "mulhw"}, {104, F_DA, OE_OK, "neg"},
    {136, F_DAB, OE_OK, "subfe"}, {138, F_DAB, OE_OK, "adde"}, {200, F_DA, OE_OK, "subfze"},
    {202, F_DA, OE_OK, "addze"}, {232, F_DA, OE_OK, "subfme"}, {234, F_DA, OE_OK, "addme"},
    {235, F_DAB, OE_OK, "mullw"}, {266, F_DAB, OE_OK, "add"}, {459, F_DAB, OE_OK, "divwu"},
    {491, F_DAB, OE_OK, "divw"},
    /* POWER (601) */
    {107, F_DAB, OE_OK, "mul"}, {331, F_DAB, OE_OK, "div"}, {363, F_DAB, OE_OK, "divs"},
    {264, F_DAB, OE_OK, "doz"}, {360, F_DA, OE_OK, "abs"}, {488, F_DA, OE_OK, "nabs"},
    {531, F_DA, RC_OK, "clcs"}, {277, F_LX, RC_OK, "lscbx"},
    {29, F_SAB, RC_OK, "maskg"}, {541, F_SAB, RC_OK, "maskir"}, {537, F_SAB, RC_OK, "rrib"},
    {153, F_SAB, RC_OK, "sle"}, {217, F_SAB, RC_OK, "sleq"}, {184, F_ASH, RC_OK, "sliq"},
    {248, F_ASH, RC_OK, "slliq"}, {216, F_SAB, RC_OK, "sllq"}, {152, F_SAB, RC_OK, "slq"},
    {952, F_ASH, RC_OK, "sraiq"}, {920, F_SAB, RC_OK, "sraq"}, {665, F_SAB, RC_OK, "sre"},
    {921, F_SAB, RC_OK, "srea"}, {729, F_SAB, RC_OK, "sreq"}, {696, F_ASH, RC_OK, "sriq"},
    {760, F_ASH, RC_OK, "srliq"}, {728, F_SAB, RC_OK, "srlq"}, {664, F_SAB, RC_OK, "srq"},
    /* logical, shifts */
    {28, F_SAB, RC_OK, "and"}, {60, F_SAB, RC_OK, "andc"}, {124, F_SAB, RC_OK, "nor"}, {444, F_SAB, RC_OK, "or"},
    {284, F_SAB, RC_OK, "eqv"}, {316, F_SAB, RC_OK, "xor"}, {412, F_SAB, RC_OK, "orc"},
    {476, F_SAB, RC_OK, "nand"}, {24, F_SAB, RC_OK, "slw"}, {536, F_SAB, RC_OK, "srw"},
    {792, F_SAB, RC_OK, "sraw"}, {26, F_AS, RC_OK, "cntlzw"}, {922, F_AS, RC_OK, "extsh"},
    {954, F_AS, RC_OK, "extsb"}, {824, F_ASH, RC_OK, "srawi"},
    /* loads and stores, indexed */
    {20, F_LX, RC_NO, "lwarx"}, {23, F_LX, RC_NO, "lwzx"}, {55, F_LX, RC_NO, "lwzux"},
    {87, F_LX, RC_NO, "lbzx"}, {119, F_LX, RC_NO, "lbzux"}, {151, F_LX, RC_NO, "stwx"},
    {183, F_LX, RC_NO, "stwux"}, {215, F_LX, RC_NO, "stbx"}, {247, F_LX, RC_NO, "stbux"},
    {279, F_LX, RC_NO, "lhzx"}, {311, F_LX, RC_NO, "lhzux"}, {343, F_LX, RC_NO, "lhax"},
    {375, F_LX, RC_NO, "lhaux"}, {407, F_LX, RC_NO, "sthx"}, {439, F_LX, RC_NO, "sthux"},
    {534, F_LX, RC_NO, "lwbrx"}, {662, F_LX, RC_NO, "stwbrx"}, {790, F_LX, RC_NO, "lhbrx"},
    {918, F_LX, RC_NO, "sthbrx"}, {533, F_LX, RC_NO, "lswx"}, {661, F_LX, RC_NO, "stswx"},
    {310, F_LX, RC_NO, "eciwx"}, {438, F_LX, RC_NO, "ecowx"},
    {535, F_FLX, RC_NO, "lfsx"}, {567, F_FLX, RC_NO, "lfsux"}, {599, F_FLX, RC_NO, "lfdx"},
    {631, F_FLX, RC_NO, "lfdux"}, {663, F_FLX, RC_NO, "stfsx"}, {695, F_FLX, RC_NO, "stfsux"},
    {727, F_FLX, RC_NO, "stfdx"}, {759, F_FLX, RC_NO, "stfdux"}, {983, F_FLX, RC_NO, "stfiwx"},
    /* AltiVec loads and stores */
    {6, F_VLX, RC_NO, "lvsl"}, {38, F_VLX, RC_NO, "lvsr"}, {7, F_VLX, RC_NO, "lvebx"},
    {39, F_VLX, RC_NO, "lvehx"}, {71, F_VLX, RC_NO, "lvewx"}, {103, F_VLX, RC_NO, "lvx"},
    {359, F_VLX, RC_NO, "lvxl"}, {135, F_VLX, RC_NO, "stvebx"}, {167, F_VLX, RC_NO, "stvehx"},
    {199, F_VLX, RC_NO, "stvewx"}, {231, F_VLX, RC_NO, "stvx"}, {487, F_VLX, RC_NO, "stvxl"},
    /* cache and system */
    {54, F_CACHE, RC_NO, "dcbst"}, {86, F_CACHE, RC_NO, "dcbf"}, {246, F_CACHE, RC_NO, "dcbtst"},
    {278, F_CACHE, RC_NO, "dcbt"}, {470, F_CACHE, RC_NO, "dcbi"}, {982, F_CACHE, RC_NO, "icbi"},
    {1014, F_CACHE, RC_NO, "dcbz"}, {758, F_CACHE, RC_NO, "dcba"},
    {306, F_B, RC_NO, "tlbie"}, {370, F_NONE, RC_NO, "tlbia"}, {566, F_NONE, RC_NO, "tlbsync"},
    {598, F_NONE, RC_NO, "sync"}, {854, F_NONE, RC_NO, "eieio"},
};

static int dec31(Dis *d, uint32_t op)
{
    int xo = (int)((op >> 1) & 0x3FF);
    int rd = RD(op), ra = RA(op), rb = RB(op), rc = (int)(op & 1);
    int spr;
    size_t k;

    /* The special forms first. */
    switch (xo) {
    case 0:
    case 32:
        if (op & 0x00400000)
            return 0;
        if ((rd & 1)) {
            strcpy(d->mn, xo ? "cmpl" : "cmp");
            OPS(d, "cr%d,1,r%d,r%d", rd >> 2, ra, rb);
        } else {
            strcpy(d->mn, xo ? "cmplw" : "cmpw");
            OPS(d, "%sr%d,r%d", crfx(d->t[0], rd >> 2), ra, rb);
        }
        return 1;
    case 4: {
        const char *c = tocond(rd);
        if (rc)
            return 0;
        if (rd == 31 && ra == 0 && rb == 0) {
            strcpy(d->mn, "trap");
            d->ops[0] = 0;
        } else if (c) {
            snprintf(d->mn, sizeof d->mn, "tw%s", c);
            OPS(d, "r%d,r%d", ra, rb);
        } else {
            strcpy(d->mn, "tw");
            OPS(d, "%d,r%d,r%d", rd, ra, rb);
        }
        return 1;
    }
    case 19:
        if (rc || ra || rb) return 0;
        strcpy(d->mn, "mfcr"); OPS(d, "r%d", rd); return 1;
    case 83:
        if (rc || ra || rb) return 0;
        strcpy(d->mn, "mfmsr"); OPS(d, "r%d", rd); return 1;
    case 146:
        if (rc || ra || rb) return 0;
        strcpy(d->mn, "mtmsr"); OPS(d, "r%d", rd); return 1;
    case 144: {
        unsigned crm = (op >> 12) & 0xFF;
        if (rc || (op & 0x00100800)) return 0;
        if (crm == 0xFF) {
            strcpy(d->mn, "mtcr"); OPS(d, "r%d", rd);
        } else {
            strcpy(d->mn, "mtcrf"); OPS(d, "0x%02X,r%d", crm, rd);
        }
        return 1;
    }
    case 150:
        if (!rc) return 0;
        strcpy(d->mn, "stwcx."); OPS(d, "r%d,%s,r%d", rd, ra0(d->t[0], ra), rb); return 1;
    case 210:
        if (rc || rb || (ra & 16)) return 0;
        strcpy(d->mn, "mtsr"); OPS(d, "%d,r%d", ra & 15, rd); return 1;
    case 595:
        if (rc || rb || (ra & 16)) return 0;
        strcpy(d->mn, "mfsr"); OPS(d, "r%d,%d", rd, ra & 15); return 1;
    case 242:
        if (rc || ra) return 0;
        strcpy(d->mn, "mtsrin"); OPS(d, "r%d,r%d", rd, rb); return 1;
    case 659:
        if (rc || ra) return 0;
        strcpy(d->mn, "mfsrin"); OPS(d, "r%d,r%d", rd, rb); return 1;
    case 597:
    case 725:
        if (rc) return 0;
        strcpy(d->mn, xo == 597 ? "lswi" : "stswi");
        OPS(d, "r%d,%s,%d", rd, ra0(d->t[0], ra), rb);
        return 1;
    case 444:                   /* or: mr */
        if (rd == rb) {
            setmn(d, "mr", 0, rc);
            OPS(d, "r%d,r%d", ra, rd);
            return 1;
        }
        break;
    case 124:                   /* nor: not */
        if (rd == rb) {
            setmn(d, "not", 0, rc);
            OPS(d, "r%d,r%d", ra, rd);
            return 1;
        }
        break;
    case 339:
    case 467:
        if (rc) return 0;
        spr = ra | (rb << 5);
        if (xo == 339) {
            const char *n = spr == 8 ? "mflr" : spr == 9 ? "mfctr" : spr == 1 ? "mfxer" : NULL;
            if (n) { strcpy(d->mn, n); OPS(d, "r%d", rd); }
            else { strcpy(d->mn, "mfspr"); OPS(d, "r%d,%d", rd, spr); }
        } else {
            const char *n = spr == 8 ? "mtlr" : spr == 9 ? "mtctr" : spr == 1 ? "mtxer" : NULL;
            if (n) { strcpy(d->mn, n); OPS(d, "r%d", rd); }
            else { strcpy(d->mn, "mtspr"); OPS(d, "%d,r%d", spr, rd); }
        }
        return 1;
    case 371:
        if (rc) return 0;
        spr = ra | (rb << 5);
        if (spr == 268) { strcpy(d->mn, "mftb"); OPS(d, "r%d", rd); }
        else if (spr == 269) { strcpy(d->mn, "mftbu"); OPS(d, "r%d", rd); }
        else { strcpy(d->mn, "mftb"); OPS(d, "r%d,%d", rd, spr); }
        return 1;
    case 512:
        if (rc || (op & 0x0063F800) || (rd & 3)) return 0;
        strcpy(d->mn, "mcrxr"); OPS(d, "cr%d", rd >> 2); return 1;
    case 822:   /* dss, dssall */
        if (rc) return 0;
        strcpy(d->mn, (op & 0x02000000) ? "dssall" : "dss");
        if (op & 0x02000000) d->ops[0] = 0; else OPS(d, "%d", (op >> 21) & 3);
        return 1;
    default:
        break;
    }

    for (k = 0; k < sizeof x31 / sizeof x31[0]; k++) {
        const X31 *e = &x31[k];
        int oe = 0;
        if (e->flags == OE_OK) {
            if (e->xo != (xo & 0x1FF))
                continue;
            oe = (xo >> 9) & 1;
        } else if (e->xo != xo)
            continue;
        if (e->flags == RC_NO && rc)
            return 0;
        if (((e->fmt == F_DA || e->fmt == F_AS) && rb) || (e->fmt == F_B && (rd || ra)) || (e->fmt == F_CACHE && rd) ||
            (e->fmt == F_NONE && (op & 0x03FFF800) &&
             !(e->xo == 598 && !(op & 0x039FF800))))
            return 0;
        if (e->fmt == F_NONE || e->fmt == F_CACHE || e->fmt == F_B) {
            if (e->fmt == F_NONE) d->ops[0] = 0;
            else if (e->fmt == F_B) OPS(d, "r%d", rb);
            else OPS(d, "%s,r%d", ra0(d->t[0], ra), rb);
            strcpy(d->mn, e->name);
            return 1;
        }
        setmn(d, e->name, oe, rc);
        switch (e->fmt) {
        case F_DAB: OPS(d, "r%d,r%d,r%d", rd, ra, rb); break;
        case F_DA: OPS(d, "r%d,r%d", rd, ra); break;
        case F_SAB: OPS(d, "r%d,r%d,r%d", ra, rd, rb); break;
        case F_AS: OPS(d, "r%d,r%d", ra, rd); break;
        case F_ASH: OPS(d, "r%d,r%d,%d", ra, rd, rb); break;
        case F_LX: OPS(d, "r%d,%s,r%d", rd, ra0(d->t[0], ra), rb); break;
        case F_FLX: OPS(d, "f%d,%s,r%d", rd, ra0(d->t[0], ra), rb); break;
        default: OPS(d, "v%d,%s,r%d", rd, ra0(d->t[0], ra), rb); break;
        }
        return 1;
    }
    return 0;
}

/* Opcode 19: condition register ops, branches to lr and ctr, rfi, isync. */
static int dec19(Dis *d, uint32_t addr, uint32_t op)
{
    int xo = (int)((op >> 1) & 0x3FF);
    int rd = RD(op), ra = RA(op), rb = RB(op);
    const char *n = NULL;

    switch (xo) {
    case 16:
    case 528:
        if (rb) return 0;
        do_branch(d, addr, op, xo == 16 ? 1 : 2);
        return 1;
    case 0:
        if ((op & 0x0063F801) || (rd & 3) || (ra & 3)) return 0;
        strcpy(d->mn, "mcrf");
        OPS(d, "cr%d,cr%d", rd >> 2, ra >> 2);
        return 1;
    case 50:
    case 150:
        if (op & 0x03FFF801) return 0;
        strcpy(d->mn, xo == 50 ? "rfi" : "isync");
        d->ops[0] = 0;
        return 1;
    case 33: n = "crnor"; break;
    case 129: n = "crandc"; break;
    case 193: n = "crxor"; break;
    case 225: n = "crnand"; break;
    case 257: n = "crand"; break;
    case 289: n = "creqv"; break;
    case 417: n = "crorc"; break;
    case 449: n = "cror"; break;
    default: return 0;
    }
    if (op & 1)
        return 0;
    if (xo == 289 && rd == ra && ra == rb) {
        strcpy(d->mn, "crset"); OPS(d, "%d", rd);
    } else if (xo == 193 && rd == ra && ra == rb) {
        strcpy(d->mn, "crclr"); OPS(d, "%d", rd);
    } else if (xo == 449 && ra == rb) {
        strcpy(d->mn, "crmove"); OPS(d, "%d,%d", rd, ra);
    } else if (xo == 33 && ra == rb) {
        strcpy(d->mn, "crnot"); OPS(d, "%d,%d", rd, ra);
    } else {
        strcpy(d->mn, n); OPS(d, "%d,%d,%d", rd, ra, rb);
    }
    return 1;
}

/* Opcodes 59 (single) and 63 (double) floating point. */
static int decfp(Dis *d, uint32_t op)
{
    int single = (op >> 26) == 59;
    int xo5 = (int)((op >> 1) & 31), xo = (int)((op >> 1) & 0x3FF);
    int rd = RD(op), ra = RA(op), rb = RB(op), rc = (int)(op & 1);
    int rcc = (int)((op >> 6) & 31);
    const char *n = NULL;
    char nm[24];
    enum { AB, AC, ACB, B } f = AB;      /* operand shape of the A-form */

    switch (xo5) {
    case 18: n = "fdiv"; break;
    case 20: n = "fsub"; break;
    case 21: n = "fadd"; break;
    case 22: n = "fsqrt"; f = B; break;
    case 23: if (single) return 0; n = "fsel"; f = ACB; break;
    case 24: if (!single) return 0; n = "fres"; f = B; break;
    case 25: n = "fmul"; f = AC; break;
    case 26: if (single) return 0; n = "frsqrte"; f = B; break;
    case 28: n = "fmsub"; f = ACB; break;
    case 29: n = "fmadd"; f = ACB; break;
    case 30: n = "fnmsub"; f = ACB; break;
    case 31: n = "fnmadd"; f = ACB; break;
    default: break;
    }
    if (n) {
        if ((f == AB && rcc) || (f == B && (ra || rcc)) || (f == AC && rb))
            return 0;
        snprintf(nm, sizeof nm, "%s%s", n, (single && xo5 != 24) ? "s" : "");
        setmn(d, nm, 0, rc);
        switch (f) {
        case AB: OPS(d, "f%d,f%d,f%d", rd, ra, rb); break;
        case AC: OPS(d, "f%d,f%d,f%d", rd, ra, rcc); break;
        case ACB: OPS(d, "f%d,f%d,f%d,f%d", rd, ra, rcc, rb); break;
        default: OPS(d, "f%d,f%d", rd, rb); break;
        }
        return 1;
    }
    if (single)
        return 0;
    switch (xo) {
    case 0: case 32:
        if (rc || (op & 0x00600000)) return 0;
        strcpy(d->mn, xo ? "fcmpo" : "fcmpu");
        OPS(d, "%sf%d,f%d", crfx(d->t[0], rd >> 2), ra, rb);
        return 1;
    case 12: n = "frsp"; break;
    case 14: n = "fctiw"; break;
    case 15: n = "fctiwz"; break;
    case 40: n = "fneg"; break;
    case 72: n = "fmr"; break;
    case 136: n = "fnabs"; break;
    case 264: n = "fabs"; break;
    case 583:
        if (ra || rb) return 0;
        setmn(d, "mffs", 0, rc); OPS(d, "f%d", rd); return 1;
    case 711:
        if (op & 0x02010000) return 0;
        setmn(d, "mtfsf", 0, rc);
        OPS(d, "0x%02X,f%d", (op >> 17) & 0xFF, rb);
        return 1;
    case 38: case 70:
        if (ra || rb) return 0;
        setmn(d, xo == 38 ? "mtfsb1" : "mtfsb0", 0, rc);
        OPS(d, "%d", rd);
        return 1;
    case 64:
        if (rc || (op & 0x0063F800) || (rd & 3) || (ra & 3)) return 0;
        strcpy(d->mn, "mcrfs");
        OPS(d, "cr%d,cr%d", rd >> 2, ra >> 2);
        return 1;
    case 134:
        if ((op & 0x007E0800) || (rd & 3)) return 0;
        setmn(d, "mtfsfi", 0, rc);
        OPS(d, "cr%d,%d", rd >> 2, (op >> 12) & 15);
        return 1;
    default:
        return 0;
    }
    if (ra)
        return 0;
    setmn(d, n, 0, rc);
    OPS(d, "f%d,f%d", rd, rb);
    return 1;
}

static int decode(Dis *d, uint32_t addr, uint32_t op)
{
    static const char *const ldst[24] = {
        "lwz", "lwzu", "lbz", "lbzu", "stw", "stwu", "stb", "stbu",
        "lhz", "lhzu", "lha", "lhau", "sth", "sthu", "lmw", "stmw",
        "lfs", "lfsu", "lfd", "lfdu", "stfs", "stfsu", "stfd", "stfdu"
    };
    int pri = (int)(op >> 26);
    int rd = RD(op), ra = RA(op), rb = RB(op), rc = (int)(op & 1);
    int simm = SIMM(op);
    unsigned uimm = UIMM(op);
    char *t = d->t[2];

    if (pri >= 32 && pri < 56) {
        strcpy(d->mn, ldst[pri - 32]);
        disp(d, pri >= 48 ? 'f' : 'r', rd, simm, ra);
        return 1;
    }
    switch (pri) {
    case 1:
        strcpy(d->mn, "NATIVE");
        OPS(d, "$%07X", (unsigned)(op & 0x03FFFFFF));
        return 1;
    case 3: {
        const char *c = tocond(rd);
        if (c) {
            snprintf(d->mn, sizeof d->mn, "tw%si", c);
            OPS(d, "r%d,%s", ra, sx(t, simm));
        } else {
            strcpy(d->mn, "twi");
            OPS(d, "%d,r%d,%s", rd, ra, sx(t, simm));
        }
        return 1;
    }
    case 7: strcpy(d->mn, "mulli"); OPS(d, "r%d,r%d,%s", rd, ra, sx(t, simm)); return 1;
    case 8: strcpy(d->mn, "subfic"); OPS(d, "r%d,r%d,%s", rd, ra, sx(t, simm)); return 1;
    case 9: strcpy(d->mn, "dozi"); OPS(d, "r%d,r%d,%s", rd, ra, sx(t, simm)); return 1;
    case 10:
    case 11:
        if (rd & 2)
            return 0;
        if (rd & 1) {
            strcpy(d->mn, pri == 10 ? "cmpli" : "cmpi");
            if (pri == 10)
                snprintf(t, 24, "0x%04X", uimm);
            else
                sx(t, simm);
            OPS(d, "cr%d,1,r%d,%s", rd >> 2, ra, t);
        } else {
            strcpy(d->mn, pri == 10 ? "cmplwi" : "cmpwi");
            if (pri == 10)
                snprintf(t, 24, "0x%04X", uimm);
            else
                sx(t, simm);
            OPS(d, "%sr%d,%s", crfx(d->t[0], rd >> 2), ra, t);
        }
        return 1;
    case 12:
    case 13:
        if (simm < 0) {
            strcpy(d->mn, pri == 12 ? "subic" : "subic.");
            OPS(d, "r%d,r%d,0x%04X", rd, ra, (unsigned)-simm);
        } else {
            strcpy(d->mn, pri == 12 ? "addic" : "addic.");
            OPS(d, "r%d,r%d,%s", rd, ra, sx(t, simm));
        }
        return 1;
    case 14:
        if (ra == 0) {
            strcpy(d->mn, "li"); OPS(d, "r%d,%s", rd, sx(t, simm));
        } else if (simm < 0) {
            strcpy(d->mn, "subi"); OPS(d, "r%d,r%d,0x%04X", rd, ra, (unsigned)-simm);
        } else {
            strcpy(d->mn, "addi"); OPS(d, "r%d,r%d,%s", rd, ra, sx(t, simm));
        }
        return 1;
    case 15:
        if (ra == 0) {
            strcpy(d->mn, "lis"); OPS(d, "r%d,0x%04X", rd, uimm);
        } else if (simm < 0) {
            strcpy(d->mn, "subis"); OPS(d, "r%d,r%d,0x%04X", rd, ra, (unsigned)-simm);
        } else {
            strcpy(d->mn, "addis"); OPS(d, "r%d,r%d,0x%04X", rd, ra, uimm);
        }
        return 1;
    case 16: do_branch(d, addr, op, 0); return 1;
    case 17:
        if (op != 0x44000002) return 0;
        strcpy(d->mn, "sc"); d->ops[0] = 0;
        return 1;
    case 18: {
        int32_t li = (int32_t)(op & 0x03FFFFFC);
        uint32_t target;
        if (li & 0x02000000) li -= 0x04000000;
        target = (op & 2) ? (uint32_t)li : addr + (uint32_t)li;
        snprintf(d->mn, sizeof d->mn, "b%s%s", (op & 1) ? "l" : "", (op & 2) ? "a" : "");
        OPS(d, "$%08X", (unsigned)target);
        return 1;
    }
    case 19: return dec19(d, addr, op);
    case 20: do_rlwimi(d, ra, rd, rb, (int)((op >> 6) & 31), (int)((op >> 1) & 31), rc); return 1;
    case 21: do_rlwinm(d, ra, rd, rb, (int)((op >> 6) & 31), (int)((op >> 1) & 31), rc); return 1;
    case 22:
        setmn(d, "rlmi", 0, rc);
        OPS(d, "r%d,r%d,r%d,%d,%d", ra, rd, rb, (int)((op >> 6) & 31), (int)((op >> 1) & 31));
        return 1;
    case 23: {
        int mb = (int)((op >> 6) & 31), me = (int)((op >> 1) & 31);
        if (mb == 0 && me == 31) {
            setmn(d, "rotlw", 0, rc);
            OPS(d, "r%d,r%d,r%d", ra, rd, rb);
        } else {
            setmn(d, "rlwnm", 0, rc);
            OPS(d, "r%d,r%d,r%d,%d,%d", ra, rd, rb, mb, me);
        }
        return 1;
    }
    case 24:
        if (op == 0x60000000) { strcpy(d->mn, "nop"); d->ops[0] = 0; return 1; }
        strcpy(d->mn, "ori"); OPS(d, "r%d,r%d,0x%04X", ra, rd, uimm); return 1;
    case 25: strcpy(d->mn, "oris"); OPS(d, "r%d,r%d,0x%04X", ra, rd, uimm); return 1;
    case 26: strcpy(d->mn, "xori"); OPS(d, "r%d,r%d,0x%04X", ra, rd, uimm); return 1;
    case 27: strcpy(d->mn, "xoris"); OPS(d, "r%d,r%d,0x%04X", ra, rd, uimm); return 1;
    case 28: strcpy(d->mn, "andi."); OPS(d, "r%d,r%d,0x%04X", ra, rd, uimm); return 1;
    case 29: strcpy(d->mn, "andis."); OPS(d, "r%d,r%d,0x%04X", ra, rd, uimm); return 1;
    case 31: return dec31(d, op);
    case 59:
    case 63: return decfp(d, op);
    }
    return 0;
}

/* Disassemble instruction word op, found at address addr (for branch targets), into buf. */
void ppc_disasm(uint32_t addr, uint32_t op, char *buf, int bufsize)
{
    Dis d;

    if (bufsize <= 0)
        return;
    d.mn[0] = 0;
    d.ops[0] = 0;
    if (!decode(&d, addr, op)) {
        snprintf(buf, (size_t)bufsize, ".long     0x%08X", (unsigned)op);
        return;
    }
    if (d.ops[0])
        snprintf(buf, (size_t)bufsize, "%-9s %s", d.mn, d.ops);
    else
        snprintf(buf, (size_t)bufsize, "%s", d.mn);
}
